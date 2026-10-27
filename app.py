"""Local-only USB camera recorder and offline channel compositor."""
import argparse
import atexit
import csv
import json
from pathlib import Path
import subprocess
import threading
import time
from datetime import datetime

import cv2
import numpy as np
from flask import Flask, Response, jsonify, render_template, request

ROOT = Path(__file__).resolve().parent
DEVICES = [f'/dev/v4l/by-path/platform-xhci-hcd.{bus}-usb-0:{port}:1.0-video-index0'
           for bus, port in [(1, 2), (0, 2), (1, 1)]]


class Rig:
    def __init__(self, demo=False, output=None):
        self.demo = demo
        self.output = Path(output or ROOT / 'captures')
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.frames = [None] * 3
        self.status = [{'frames': 0, 'fps': 0, 'error': '시작 중'} for _ in range(3)]
        self.recording = None
        self.threads = []

    def start(self):
        for i in range(3):
            t = threading.Thread(target=self.capture, args=(i,), daemon=True)
            t.start()
            self.threads.append(t)

    def capture(self, i):
        cap = None
        try:
            if not self.demo:
                dev = DEVICES[i]
                for ctrl in ['auto_exposure=1', 'exposure_time_absolute=625']:
                    subprocess.run(['v4l2-ctl', '-d', dev, '--set-ctrl=' + ctrl],
                                   check=True, capture_output=True, timeout=5)
                cap = cv2.VideoCapture(dev, cv2.CAP_V4L2)
                cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'YUYV'))
                cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
                cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
                cap.set(cv2.CAP_PROP_FPS, 30)
                if not cap.isOpened():
                    raise RuntimeError('카메라 열기 실패: ' + dev)
            started = time.monotonic()
            count = 0
            while not self.stop.is_set():
                if self.demo:
                    self.stop.wait(1 / 30)
                    frame = np.zeros((480, 640, 3), np.uint8)
                    frame[:, :, i] = 160
                    cv2.circle(frame, (int(time.monotonic() * 70) % 600 + 20, 240), 30, (255, 255, 255), -1)
                    cv2.putText(frame, f'DEMO cam{i+1}', (20, 50), 0, 1, (255, 255, 255), 2)
                else:
                    ok, frame = cap.read()
                    if not ok:
                        raise RuntimeError('프레임 수신 실패; USB 연결을 확인하고 재시작하세요')
                stamp = time.monotonic_ns()
                count += 1
                with self.lock:
                    self.frames[i] = frame
                    self.status[i] = {'frames': count, 'fps': round(count / (time.monotonic() - started), 2), 'error': None}
                    if self.recording:
                        rec = self.recording
                        h, w = frame.shape[:2]
                        if (w, h) != (640, 480):
                            raise RuntimeError(f'예상하지 못한 해상도: {w}x{h}')
                        rec['writers'][i].write(frame)
                        rec['csv'][i].writerow([rec['counts'][i], stamp])
                        rec['counts'][i] += 1
        except Exception as exc:
            with self.lock:
                self.status[i]['error'] = str(exc)
                self.finish()
        finally:
            if cap is not None:
                cap.release()

    def begin(self):
        with self.lock:
            if self.recording:
                raise ValueError('이미 녹화 중입니다')
            if any(s['error'] for s in self.status):
                raise ValueError('세 카메라가 모두 준비되어야 합니다')
            folder = self.output / datetime.now().strftime('%Y%m%d_%H%M%S_%f')
            folder.mkdir(parents=True)
            rec = {'folder': folder, 'writers': [], 'files': [], 'csv': [], 'counts': [0]*3}
            try:
                for i in range(3):
                    writer = cv2.VideoWriter(str(folder / f'cam{i+1}.avi'), cv2.VideoWriter_fourcc(*'MJPG'), 30, (640, 480))
                    rec['writers'].append(writer)
                    if not writer.isOpened():
                        raise RuntimeError('MJPEG 파일 저장기 초기화 실패')
                    f = (folder / f'cam{i+1}.csv').open('w', newline='')
                    rec['files'].append(f)
                    table = csv.writer(f)
                    table.writerow(['frame', 'monotonic_ns'])
                    rec['csv'].append(table)
                self.recording = rec
            except Exception:
                for w in rec['writers']: w.release()
                for f in rec['files']: f.close()
                raise
            return folder.name

    def finish(self):
        with self.lock:
            rec = self.recording
            if rec is None:
                return None
            self.recording = None
            for w in rec['writers']: w.release()
            for f in rec['files']: f.close()
            (rec['folder'] / 'session.json').write_text(json.dumps({
                'demo': self.demo, 'devices': DEVICES, 'frames': rec['counts'],
                'nominal_fps': 30, 'timestamp': 'host monotonic_ns after frame read; not sensor exposure time'
            }, indent=2))
            return rec['folder'].name

    def close(self):
        self.stop.set()
        for t in self.threads: t.join(timeout=2)
        self.finish()


def compose(folder, mapping, offsets):
    """Nearest host timestamp matching; cam1 defines output timeline."""
    if sorted(mapping) != [0, 1, 2]:
        raise ValueError('RGB 채널에 서로 다른 카메라를 선택하세요')
    if len(offsets) != 3 or any(len(x) != 2 or not all(np.isfinite(v) and abs(v) <= 640 for v in x) for x in offsets):
        raise ValueError('이동값은 -640~640 범위의 숫자여야 합니다')
    times = []
    caps = []
    writer = None
    try:
        for i in range(3):
            with (folder / f'cam{i+1}.csv').open() as f:
                times.append(np.array([int(r['monotonic_ns']) for r in csv.DictReader(f)], dtype=np.int64))
            if len(times[-1]) < 2: raise ValueError('각 카메라에 최소 2프레임이 필요합니다')
            cap = cv2.VideoCapture(str(folder / f'cam{i+1}.avi'))
            caps.append(cap)
            if not cap.isOpened(): raise ValueError('영상 파일 열기 실패')
        begin = max(t[0] for t in times)
        end = min(t[-1] for t in times)
        targets = times[0][(times[0] >= begin) & (times[0] <= end)]
        if len(targets) < 2: raise ValueError('세 영상의 공통 촬영 구간이 부족합니다')
        fps = float(1e9 / np.median(np.diff(targets)))
        destination = folder / 'composite.avi'
        writer = cv2.VideoWriter(str(destination), cv2.VideoWriter_fourcc(*'MJPG'), fps, (640, 480))
        if not writer.isOpened(): raise RuntimeError('합성 영상 저장기 초기화 실패')
        positions = [-1]*3
        frames = [None]*3
        for stamp in targets:
            gray = []
            for i in range(3):
                right = int(np.searchsorted(times[i], stamp))
                candidates = [k for k in (right-1, right) if 0 <= k < len(times[i])]
                idx = min(candidates, key=lambda k: abs(int(times[i][k])-int(stamp)))
                while positions[i] < idx:
                    ok, frames[i] = caps[i].read()
                    if not ok: raise ValueError('프레임 기록과 영상 길이가 다릅니다')
                    positions[i] += 1
                g = cv2.cvtColor(frames[i], cv2.COLOR_BGR2GRAY)
                dx, dy = offsets[i]
                gray.append(cv2.warpAffine(g, np.float32([[1, 0, dx], [0, 1, dy]]), (640, 480)))
            writer.write(cv2.merge([gray[mapping[2]], gray[mapping[1]], gray[mapping[0]]]))
        (folder / 'alignment.json').write_text(json.dumps({'rgb_cameras': mapping, 'xy_offsets': offsets}, indent=2))
        return {'file': str(destination), 'frames': len(targets), 'fps': round(fps, 2)}
    finally:
        for cap in caps: cap.release()
        if writer is not None: writer.release()


def create_app(rig):
    app = Flask(__name__)
    job_lock = threading.Lock()

    @app.before_request
    def local_requests():
        # Local browser only. Reject foreign origins/hosts and cross-site mutations.
        if request.host.split(':')[0] not in ('localhost', '127.0.0.1'):
            return 'Local access only', 403
        if request.method == 'POST' and request.headers.get('X-Rig-Request') != '1':
            return 'Missing request header', 403
        origin = request.headers.get('Origin')
        if origin and origin != request.host_url.rstrip('/'):
            return 'Foreign origin', 403

    @app.get('/')
    def home(): return render_template('index.html', demo=rig.demo)

    @app.get('/api/status')
    def status():
        with rig.lock:
            return jsonify(cameras=rig.status, recording=rig.recording['folder'].name if rig.recording else None)

    @app.get('/stream/<int:i>')
    def stream(i):
        if i not in range(3): return '', 404
        def images():
            while not rig.stop.is_set():
                with rig.lock:
                    frame = rig.frames[i]
                    frame = None if frame is None else frame.copy()
                if frame is not None:
                    ok, encoded = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 65])
                    if ok: yield b'--frame\r\nContent-Type: image/jpeg\r\n\r\n' + encoded.tobytes() + b'\r\n'
                time.sleep(.1)
        return Response(images(), mimetype='multipart/x-mixed-replace; boundary=frame')

    @app.post('/api/<action>')
    def action(action):
        if not job_lock.acquire(blocking=False): return jsonify(error='다른 작업이 진행 중입니다'), 409
        try:
            if action == 'start': return jsonify(session=rig.begin())
            if action == 'stop': return jsonify(session=rig.finish())
            if action == 'compose':
                body = request.get_json()
                name = body['session']
                if not isinstance(name, str) or Path(name).name != name or name in ('.', '..'):
                    raise ValueError('잘못된 세션 이름')
                folder = (rig.output / name).resolve()
                if folder.parent != rig.output.resolve(): raise ValueError('잘못된 경로')
                with rig.lock:
                    if rig.recording: raise ValueError('녹화를 중지한 후 합성하세요')
                if not (folder / 'session.json').is_file(): raise ValueError('완료된 세션이 아닙니다')
                return jsonify(compose(folder, body['mapping'], body['offsets']))
            return jsonify(error='알 수 없는 작업'), 404
        except (ValueError, RuntimeError, KeyError, TypeError, OSError) as exc:
            return jsonify(error=str(exc)), 400
        finally:
            job_lock.release()

    @app.post('/api/preview')
    def preview():
        try:
            body = request.get_json()
            name = body['session']
            if not isinstance(name, str) or Path(name).name != name or name in ('.', '..'):
                raise ValueError('잘못된 세션 이름')
            folder = (rig.output / name).resolve()
            if folder.parent != rig.output.resolve() or not (folder / 'session.json').is_file():
                raise ValueError('완료된 세션이 아닙니다')
            mapping = body['mapping']
            offsets = body['offsets']
            if sorted(mapping) != [0, 1, 2]:
                raise ValueError('RGB 채널에 서로 다른 카메라를 선택하세요')
            if len(offsets) != 3 or any(len(x) != 2 or not all(np.isfinite(v) and abs(v) <= 640 for v in x) for x in offsets):
                raise ValueError('이동값 범위를 확인하세요')
            fraction = float(body.get('position', 0.5))
            if not np.isfinite(fraction) or not 0 <= fraction <= 1:
                raise ValueError('잘못된 재생 위치')
            times = []
            for i in range(3):
                with (folder / f'cam{i+1}.csv').open() as f:
                    times.append(np.array([int(r['monotonic_ns']) for r in csv.DictReader(f)], dtype=np.int64))
                if len(times[-1]) == 0:
                    raise ValueError('빈 영상입니다')
            start = max(t[0] for t in times)
            end = min(t[-1] for t in times)
            if end < start:
                raise ValueError('공통 촬영 구간이 없습니다')
            stamp = start + int((end - start) * fraction)
            gray = []
            for i in range(3):
                idx = int(np.argmin(np.abs(times[i] - stamp)))
                cap = cv2.VideoCapture(str(folder / f'cam{i+1}.avi'))
                try:
                    cap.set(cv2.CAP_PROP_POS_FRAMES, idx)
                    ok, frame = cap.read()
                    if not ok:
                        raise ValueError('영상 프레임 읽기 실패')
                finally:
                    cap.release()
                g = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                dx, dy = offsets[i]
                gray.append(cv2.warpAffine(g, np.float32([[1, 0, dx], [0, 1, dy]]), (640, 480)))
            merged = cv2.merge([gray[mapping[2]], gray[mapping[1]], gray[mapping[0]]])
            ok, encoded = cv2.imencode('.jpg', merged, [cv2.IMWRITE_JPEG_QUALITY, 90])
            if not ok:
                raise RuntimeError('미리보기 인코딩 실패')
            return Response(encoded.tobytes(), mimetype='image/jpeg', headers={'Cache-Control': 'no-store'})
        except (ValueError, RuntimeError, KeyError, TypeError, OSError) as exc:
            return jsonify(error=str(exc)), 400

    @app.get('/api/sessions')
    def sessions():
        return jsonify(sorted([p.parent.name for p in rig.output.glob('*/session.json')], reverse=True))
    return app


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--demo', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--port', type=int, default=8000)
    args = parser.parse_args()
    rig = Rig(args.demo, args.output)
    atexit.register(rig.close)
    rig.start()
    create_app(rig).run(host='127.0.0.1', port=args.port, threaded=True, debug=False, use_reloader=False)
