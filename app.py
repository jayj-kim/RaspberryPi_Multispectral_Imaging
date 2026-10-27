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
                for ctrl in ['auto_exposure=1', 'exposure_time_absolute=4500']:
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


def registration_matrices(points=None, adjustments=None):
    """Map each original camera image into cam1 coordinates, then fine-tune."""
    points = points if points is not None else [[], [], []]
    adjustments = adjustments if adjustments is not None else [[0, 1]] * 3
    if len(points) != 3 or len(adjustments) != 3:
        raise ValueError('카메라 3대의 보정값이 필요합니다')
    arrays = []
    for group in points:
        a = np.asarray(group, dtype=np.float64)
        if a.size == 0:
            arrays.append(None)
            continue
        if a.shape != (4, 2) or not np.isfinite(a).all() or (a < 0).any() or (a[:, 0] >= 640).any() or (a[:, 1] >= 480).any():
            raise ValueError('각 영상에서 대응점 4개를 선택하세요')
        # Every three points must span an area: reject duplicates and near-collinear sets.
        for omit in range(4):
            tri = np.delete(a, omit, axis=0)
            u, v = tri[1] - tri[0], tri[2] - tri[0]
            if abs(u[0]*v[1] - u[1]*v[0]) < 20:
                raise ValueError('점은 일직선을 피하고 영상 전체에 넓게 선택하세요')
        arrays.append(a)
    result = []
    for i in range(3):
        h = np.eye(3)
        if i and arrays[i] is not None:
            if arrays[0] is None:
                raise ValueError('기준 cam1의 대응점 4개가 필요합니다')
            h = cv2.getPerspectiveTransform(arrays[i].astype(np.float32), arrays[0].astype(np.float32))
            if not np.isfinite(h).all() or abs(np.linalg.det(h)) < 1e-10 or np.linalg.cond(h) > 1e9:
                raise ValueError('불안정한 대응점입니다. 점 순서와 위치를 확인하세요')
            corners = np.float32([[0,0], [639,0], [639,479], [0,479]])
            denominators = (h @ np.c_[corners, np.ones(4)].T)[2]
            if np.min(denominators) <= 0 <= np.max(denominators):
                raise ValueError('영상 내부에서 변환이 뒤집힙니다. 점 순서를 확인하세요')
        pair = np.asarray(adjustments[i], dtype=float)
        if pair.shape != (2,) or not np.isfinite(pair).all() or abs(pair[0]) > 180 or not .2 <= pair[1] <= 5:
            raise ValueError('회전은 ±180도, 배율은 0.2~5 범위입니다')
        fine = np.eye(3)
        fine[:2] = cv2.getRotationMatrix2D((320, 240), float(pair[0]), float(pair[1]))
        result.append(fine @ h)
    return result


def automatic_registration(frames):
    """Estimate two static homographies at the selected timestamp; fail atomically."""
    detector = cv2.ORB_create(nfeatures=3000, fastThreshold=12)
    reference_keys, reference_desc = detector.detectAndCompute(frames[0], None)
    if reference_desc is None or len(reference_keys) < 25:
        raise ValueError('cam1 특징점이 부족합니다. 밝고 선명하며 무늬가 많은 장면을 선택하세요')
    estimates, reports = [], []
    for i in (1, 2):
        keys, desc = detector.detectAndCompute(frames[i], None)
        if desc is None or len(keys) < 25:
            raise ValueError(f'cam{i+1} 특징점이 부족합니다')
        pairs = cv2.BFMatcher(cv2.NORM_HAMMING).knnMatch(desc, reference_desc, k=2)
        good = [pair[0] for pair in pairs if len(pair) == 2 and pair[0].distance < .75 * pair[1].distance]
        # Keep only the best source for each reference feature.
        unique = {}
        for match in sorted(good, key=lambda m: m.distance):
            unique.setdefault(match.trainIdx, match)
        good = list(unique.values())
        if len(good) < 20:
            raise ValueError(f'cam{i+1}: 유효 대응점 {len(good)}개로 부족합니다 (최소 20개)')
        source = np.float32([keys[m.queryIdx].pt for m in good])
        target = np.float32([reference_keys[m.trainIdx].pt for m in good])
        h, mask = cv2.findHomography(source, target, cv2.RANSAC, 3.0)
        if h is None or mask is None:
            raise ValueError(f'cam{i+1}: 원근 변환 계산 실패')
        selected = mask.ravel().astype(bool)
        count = int(selected.sum())
        ratio = count / len(good)
        if count < 15 or ratio < .45:
            raise ValueError(f'cam{i+1}: 정합 신뢰도 부족 ({count}/{len(good)} 대응점)')
        projected = cv2.perspectiveTransform(source[selected, None, :], h)[:, 0]
        error = float(np.median(np.linalg.norm(projected - target[selected], axis=1)))
        coverage = min(cv2.contourArea(cv2.convexHull(a[selected])) for a in (source, target)) / (640*480)
        if not np.isfinite(h).all() or not np.isfinite(error) or error > 2.5 or coverage < .03:
            raise ValueError(f'cam{i+1}: 오차가 크거나 특징점이 좁은 영역에 몰려 있습니다')
        estimates.append(h)
        reports.append({'camera': i+1, 'matches': len(good), 'inliers': count,
                        'inlier_ratio': round(ratio, 3), 'median_error_px': round(error, 3),
                        'coverage_percent': round(coverage*100, 1)})
    # Encode homographies as compatible four-point controls, shared in cam1 coordinates.
    # Find a broad reference rectangle whose inverse projections lie inside both sources.
    for margin in (60, 90, 120, 150):
        reference = np.float32([[margin,margin],[639-margin,margin],
                                [639-margin,479-margin],[margin,479-margin]])
        controls = [reference.tolist()]
        try:
            for h in estimates:
                source = cv2.perspectiveTransform(reference[None], np.linalg.inv(h))[0]
                controls.append(source.tolist())
            registration_matrices(controls)
        except (ValueError, np.linalg.LinAlgError):
            continue
        return {'points': controls, 'reports': reports,
                'note': '선택한 한 시점의 고정 변환입니다. 이동 영상 전체의 프레임별 자동 정합은 아닙니다.'}
    raise ValueError('세 영상의 안전한 공통 정합 영역이 부족합니다. 수동 정합으로 확인하세요')


def warp_channel(gray, matrix, offset):
    dx, dy = offset
    translation = np.float64([[1, 0, dx], [0, 1, dy], [0, 0, 1]])
    return cv2.warpPerspective(gray, translation @ matrix, (640, 480))


class TemporalRegistration:
    """Corner-space low-pass filter with deadband and sustained-jump confirmation."""
    MODES = {'strong': (.15, 1.0), 'balanced': (.3, .7), 'responsive': (.6, .3)}

    def __init__(self, initial, mode='balanced'):
        if mode not in self.MODES:
            raise ValueError('안정화 모드는 strong, balanced, responsive 중 하나입니다')
        self.alpha, self.deadband = self.MODES[mode]
        self.last = initial
        self.initialized = False
        self.pending = None
        self.pending_count = 0
        self.corners = np.float32([[[0,0],[639,0],[639,479],[0,479]]])

    def project(self, matrices):
        return np.array([cv2.perspectiveTransform(self.corners, h)[0] for h in matrices[1:]])

    def miss(self):
        self.pending, self.pending_count = None, 0

    def update(self, candidate):
        if not self.initialized:
            self.last, self.initialized = candidate, True
            return self.last, 'initialized'
        current, previous = self.project(candidate), self.project(self.last)
        if not np.isfinite(current).all():
            raise ValueError('유효하지 않은 모서리 투영')
        shifts = np.max(np.linalg.norm(current - previous, axis=2), axis=1)
        # A large single-frame estimate may be a switch of matched plane or an outlier.
        if float(shifts.max()) > 30:
            consistent = self.pending is not None and np.max(np.linalg.norm(current - self.pending, axis=2)) < 8
            self.pending_count = self.pending_count + 1 if consistent else 1
            self.pending = current
            if self.pending_count < 3:
                return self.last, 'held_jump'
            # A sustained change is allowed to catch up, rather than freezing forever.
            self.last = candidate
            self.miss()
            return self.last, 'reacquired'
        self.miss()
        smoothed = [candidate[0]]
        changed = False
        for j in range(2):
            if shifts[j] <= self.deadband:
                smoothed.append(self.last[j+1])
                continue
            changed = True
            dest = self.alpha * current[j] + (1-self.alpha) * previous[j]
            h = cv2.getPerspectiveTransform(self.corners[0], dest.astype(np.float32))
            denominator = (h @ np.c_[self.corners[0], np.ones(4)].T)[2]
            if not np.isfinite(h).all() or abs(np.linalg.det(h)) < 1e-10 or np.min(denominator) <= 0 <= np.max(denominator):
                raise ValueError('안정화 변환이 유효하지 않습니다')
            smoothed.append(h)
        self.last = smoothed
        return self.last, 'smoothed' if changed else 'held_small'


def compose(folder, mapping, offsets, points=None, adjustments=None, dynamic=False, progress=None, stabilization='balanced'):
    """Nearest host timestamp matching; cam1 defines output timeline."""
    if sorted(mapping) != [0, 1, 2]:
        raise ValueError('RGB 채널에 서로 다른 카메라를 선택하세요')
    if len(offsets) != 3 or any(len(x) != 2 or not all(np.isfinite(v) and abs(v) <= 640 for v in x) for x in offsets):
        raise ValueError('이동값은 -640~640 범위의 숫자여야 합니다')
    if type(dynamic) is not bool:
        raise ValueError('dynamic 값은 참/거짓이어야 합니다')
    matrices = registration_matrices(points, adjustments)
    last_valid = matrices
    temporal = TemporalRegistration(matrices, stabilization)
    successful = 0
    fallback = 0
    log = None
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
        destination = folder / ('composite_dynamic.avi' if dynamic else 'composite.avi')
        if dynamic:
            log = (folder / 'registration_dynamic.jsonl').open('w')
            (folder / 'registration_dynamic_summary.json').write_text(json.dumps({'completed': False}))
        writer = cv2.VideoWriter(str(destination), cv2.VideoWriter_fourcc(*'MJPG'), fps, (640, 480))
        if not writer.isOpened(): raise RuntimeError('합성 영상 저장기 초기화 실패')
        positions = [-1]*3
        frames = [None]*3
        for number, stamp in enumerate(targets):
            gray = []
            source_stamps = []
            for i in range(3):
                right = int(np.searchsorted(times[i], stamp))
                candidates = [k for k in (right-1, right) if 0 <= k < len(times[i])]
                idx = min(candidates, key=lambda k: abs(int(times[i][k])-int(stamp)))
                while positions[i] < idx:
                    ok, frames[i] = caps[i].read()
                    if not ok: raise ValueError('프레임 기록과 영상 길이가 다릅니다')
                    positions[i] += 1
                source_stamps.append(int(times[i][idx]))
                gray.append(cv2.cvtColor(frames[i], cv2.COLOR_BGR2GRAY))
            state, reason, quality = 'static', None, []
            filter_state = None
            if dynamic:
                try:
                    result = automatic_registration(gray)
                    candidate = registration_matrices(result['points'], adjustments)
                    last_valid, filter_state = temporal.update(candidate)
                    quality = result['reports']
                    if filter_state == 'held_jump':
                        fallback += 1
                        state, reason = 'fallback_previous', '큰 변환 변화: 연속 3회 일관된 추정 확인 대기'
                    else:
                        successful += 1
                        state = 'estimated'
                except (ValueError, cv2.error, np.linalg.LinAlgError) as exc:
                    temporal.miss()
                    fallback += 1
                    state = 'fallback_previous' if successful else 'fallback_initial'
                    reason = str(exc)
                matrices = last_valid
                log.write(json.dumps({'frame': number, 'monotonic_ns': int(stamp),
                    'source_monotonic_ns': source_stamps, 'state': state, 'reason': reason,
                    'filter_state': filter_state, 'quality': quality, 'matrices': [h.tolist() for h in matrices]}) + '\n')
            gray = [warp_channel(g, matrices[i], offsets[i]) for i, g in enumerate(gray)]
            if progress:
                progress({'running': True, 'done': number+1, 'total': len(targets),
                          'estimated': successful, 'fallback': fallback})
            writer.write(cv2.merge([gray[mapping[2]], gray[mapping[1]], gray[mapping[0]]]))
        (folder / 'alignment.json').write_text(json.dumps({'rgb_cameras': mapping, 'xy_offsets': offsets, 'points': points, 'adjustments': adjustments, 'dynamic': dynamic, 'stabilization': stabilization}, indent=2))
        summary = {'file': str(destination), 'frames': len(targets), 'fps': round(fps, 2),
                   'dynamic': dynamic, 'stabilization': stabilization, 'estimated': successful, 'fallback': fallback,
                   'warning': '자동 정합 성공 프레임이 없습니다. 초기 보정값만 사용했습니다.' if dynamic and not successful else None}
        if dynamic:
            (folder / 'registration_dynamic_summary.json').write_text(json.dumps({'completed': True, **summary}, indent=2))
        return summary
    finally:
        for cap in caps: cap.release()
        if writer is not None: writer.release()
        if log is not None: log.close()


def create_app(rig):
    app = Flask(__name__)
    job_lock = threading.Lock()
    processing = {'snapshot': {'running': False}}

    def update_progress(snapshot):
        processing['snapshot'] = snapshot

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
            return jsonify(processing=processing['snapshot'], cameras=rig.status, recording=rig.recording['folder'].name if rig.recording else None)

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
                update_progress({'running': True, 'done': 0, 'total': 0})
                try:
                    return jsonify(compose(folder, body['mapping'], body['offsets'], body.get('points'),
                        body.get('adjustments'), body.get('dynamic', False), update_progress, body.get('stabilization', 'balanced')))
                except Exception as exc:
                    update_progress({**processing['snapshot'], 'error': str(exc)})
                    raise
                finally:
                    update_progress({**processing['snapshot'], 'running': False})
            return jsonify(error='알 수 없는 작업'), 404
        except (ValueError, RuntimeError, KeyError, TypeError, OSError, cv2.error) as exc:
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
            if body.get('load_profile'):
                profile = folder / 'alignment.json'
                return jsonify(json.loads(profile.read_text()) if profile.is_file() else {})
            mapping = body['mapping']
            offsets = body['offsets']
            if sorted(mapping) != [0, 1, 2]:
                raise ValueError('RGB 채널에 서로 다른 카메라를 선택하세요')
            if len(offsets) != 3 or any(len(x) != 2 or not all(np.isfinite(v) and abs(v) <= 640 for v in x) for x in offsets):
                raise ValueError('이동값 범위를 확인하세요')
            matrices = registration_matrices(body.get('points'), body.get('adjustments'))
            auto = body.get('auto_register', False)
            raw = body.get('raw')
            if raw is not None and (type(raw) is not int or raw not in range(3)):
                raise ValueError('잘못된 카메라 번호')
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
                gray.append(g if raw is not None or auto else warp_channel(g, matrices[i], offsets[i]))
            if auto:
                return jsonify(automatic_registration(gray))
            merged = cv2.merge([gray[mapping[2]], gray[mapping[1]], gray[mapping[0]]])
            ok, encoded = cv2.imencode('.jpg', merged if raw is None else gray[raw], [cv2.IMWRITE_JPEG_QUALITY, 90])
            if not ok:
                raise RuntimeError('미리보기 인코딩 실패')
            return Response(encoded.tobytes(), mimetype='image/jpeg', headers={'Cache-Control': 'no-store'})
        except (ValueError, RuntimeError, KeyError, TypeError, OSError, cv2.error) as exc:
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
