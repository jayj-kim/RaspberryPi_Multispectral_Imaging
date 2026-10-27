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


def fit_correspondences(source, target, camera):
    if len(source) < 20:
        raise ValueError(f'cam{camera}: 유효 대응점 {len(source)}개로 부족합니다')
    h, mask = cv2.findHomography(source, target, cv2.RANSAC, 3.0)
    if h is None or mask is None:
        raise ValueError(f'cam{camera}: 원근 변환 계산 실패')
    selected = mask.ravel().astype(bool)
    count = int(selected.sum())
    ratio = count / len(source)
    if count < 15 or ratio < .45:
        raise ValueError(f'cam{camera}: 정합 신뢰도 부족 ({count}/{len(source)})')
    projected = cv2.perspectiveTransform(source[selected, None, :], h)[:, 0]
    error = float(np.median(np.linalg.norm(projected - target[selected], axis=1)))
    coverage = min(cv2.contourArea(cv2.convexHull(a[selected])) for a in (source, target)) / (640*480)
    if not np.isfinite(h).all() or not np.isfinite(error) or error > 2.5 or coverage < .03:
        raise ValueError(f'cam{camera}: 오차가 크거나 특징점이 좁은 영역에 몰려 있습니다')
    report = {'camera': camera, 'matches': len(source), 'inliers': count,
              'inlier_ratio': round(ratio,3), 'median_error_px': round(error,3),
              'coverage_percent': round(coverage*100,1)}
    return h, report, (source[selected], target[selected])


def encode_registration(estimates, reports):
    for margin in (60, 90, 120, 150):
        reference = np.float32([[margin,margin],[639-margin,margin],
                                [639-margin,479-margin],[margin,479-margin]])
        controls = [reference.tolist()]
        try:
            for h in estimates:
                controls.append(cv2.perspectiveTransform(reference[None], np.linalg.inv(h))[0].tolist())
            registration_matrices(controls)
        except (ValueError, np.linalg.LinAlgError):
            continue
        return {'points': controls, 'reports': reports,
                'note': '선택한 한 시점의 변환입니다.'}
    raise ValueError('세 영상의 안전한 공통 정합 영역이 부족합니다')


def automatic_registration(frames, include_tracks=False):
    detector = cv2.ORB_create(nfeatures=3000, fastThreshold=12)
    reference_keys, reference_desc = detector.detectAndCompute(frames[0], None)
    if reference_desc is None or len(reference_keys) < 25:
        raise ValueError('cam1 특징점이 부족합니다. 밝고 선명하며 무늬가 많은 장면을 선택하세요')
    estimates, reports, tracks = [], [], []
    for i in (1,2):
        keys, desc = detector.detectAndCompute(frames[i], None)
        if desc is None or len(keys) < 25:
            raise ValueError(f'cam{i+1} 특징점이 부족합니다')
        pairs = cv2.BFMatcher(cv2.NORM_HAMMING).knnMatch(desc, reference_desc, k=2)
        good = [p[0] for p in pairs if len(p)==2 and p[0].distance < .75*p[1].distance]
        unique = {}
        for match in sorted(good, key=lambda m:m.distance):
            unique.setdefault(match.trainIdx,match)
        good = list(unique.values())
        source = np.float32([keys[m.queryIdx].pt for m in good]).reshape(-1,2)
        target = np.float32([reference_keys[m.trainIdx].pt for m in good]).reshape(-1,2)
        h, report, matched = fit_correspondences(source,target,i+1)
        estimates.append(h); reports.append(report); tracks.append(matched)
    result = encode_registration(estimates,reports)
    if include_tracks:
        result['tracks'] = tracks
    return result


def track_points(previous, current, points):
    points = np.asarray(points,np.float32).reshape(-1,1,2)
    moved, forward, _ = cv2.calcOpticalFlowPyrLK(previous,current,points,None,winSize=(21,21),maxLevel=3)
    if moved is None:
        raise ValueError('특징점 추적 실패')
    back, backward, _ = cv2.calcOpticalFlowPyrLK(current,previous,moved,None,winSize=(21,21),maxLevel=3)
    if back is None:
        raise ValueError('역방향 추적 실패')
    valid = (forward.ravel()==1)&(backward.ravel()==1)&(np.linalg.norm(back-points,axis=2).ravel()<1)
    xy=moved[:,0]
    valid &= np.isfinite(xy).all(axis=1)&(xy[:,0]>=0)&(xy[:,0]<640)&(xy[:,1]>=0)&(xy[:,1]<480)
    return xy, valid


class FlowRegistration:
    """Track cross-camera inliers through time; redetect on loss or every 30 frames."""
    def __init__(self):
        self.previous=None
        self.tracks=None
        self.age=0

    def estimate(self,frames):
        result=None
        if self.previous is not None and self.tracks is not None and self.age<30:
            try:
                estimates,reports,tracks=[],[],[]
                for j,i in enumerate((1,2)):
                    source,target=self.tracks[j]
                    a,valid_a=track_points(self.previous[i],frames[i],source)
                    b,valid_b=track_points(self.previous[0],frames[0],target)
                    valid=valid_a&valid_b
                    h,report,matched=fit_correspondences(a[valid],b[valid],i+1)
                    estimates.append(h);reports.append(report);tracks.append(matched)
                result=encode_registration(estimates,reports)
                result['tracks']=tracks
                result['method']='optical_flow'
                self.age+=1
            except (ValueError,cv2.error,np.linalg.LinAlgError):
                result=None
        if result is None:
            # Do not keep tracks after an unsuccessful reacquisition.
            self.tracks=None
            self.previous=None
            result=automatic_registration(frames,include_tracks=True)
            result['method']='orb_reacquire'
            self.age=0
        self.previous=[frame.copy() for frame in frames]
        self.tracks=result.pop('tracks')
        return result


class GlobalTranslationStabilizer:
    """Reduce whole-frame translation jitter from cam1; one correction for all channels."""
    def __init__(self):
        self.previous=None
        self.path=np.zeros(2)
        self.smooth=np.zeros(2)
        self.correction=np.zeros(2)

    def update(self,frame):
        state='initialized'
        reason=None
        if self.previous is not None:
            try:
                points=cv2.goodFeaturesToTrack(self.previous,300,.01,12)
                if points is None or len(points)<20:
                    raise ValueError('전체 안정화 특징점 부족')
                moved,valid=track_points(self.previous,frame,points)
                if valid.sum()<20:
                    raise ValueError('전체 안정화 추적점 부족')
                source=points[:,0][valid]
                target=moved[valid]
                # RANSAC translation estimate: suppress independently moving objects.
                affine,mask=cv2.estimateAffinePartial2D(source,target,method=cv2.RANSAC,ransacReprojThreshold=2)
                if affine is None or mask is None or mask.sum()<20 or mask.mean()<.5:
                    raise ValueError('전체 안정화 움직임 추정 실패')
                selected=mask.ravel().astype(bool)
                if cv2.contourArea(cv2.convexHull(source[selected]))/(640*480)<.08:
                    raise ValueError('전체 안정화 특징점 분포 부족')
                # Translation-only compensation. Rotation, zoom and depth effects remain.
                delta=np.median((target-source)[selected],axis=0)
                if not np.isfinite(delta).all() or np.linalg.norm(delta)>40:
                    raise ValueError('전체 안정화 급격한 움직임')
                self.path+=delta
                self.smooth=.12*self.path+.88*self.smooth
                self.correction=np.clip(self.smooth-self.path,-50,50)
                state='stabilized'
            except (ValueError,cv2.error) as exc:
                state,reason='held',str(exc)
        self.previous=frame.copy()
        return self.correction.copy(), {'state':state,'reason':reason,'translation_px':self.correction.tolist()}


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


def compose(folder, mapping, offsets, points=None, adjustments=None, dynamic=False, progress=None, stabilization='balanced', global_stabilization=False, registration_method='orb'):
    """Nearest host timestamp matching; cam1 defines output timeline."""
    if sorted(mapping) != [0, 1, 2]:
        raise ValueError('RGB 채널에 서로 다른 카메라를 선택하세요')
    if len(offsets) != 3 or any(len(x) != 2 or not all(np.isfinite(v) and abs(v) <= 640 for v in x) for x in offsets):
        raise ValueError('이동값은 -640~640 범위의 숫자여야 합니다')
    if registration_method not in ('orb', 'tracking'):
        raise ValueError('정합 방식은 orb 또는 tracking이어야 합니다')
    if type(global_stabilization) is not bool:
        raise ValueError('전체 안정화 값은 참/거짓이어야 합니다')
    if global_stabilization and not dynamic:
        raise ValueError('전체 안정화는 프레임별 정합 모드에서 사용하세요')
    if type(dynamic) is not bool:
        raise ValueError('dynamic 값은 참/거짓이어야 합니다')
    matrices = registration_matrices(points, adjustments)
    last_valid = matrices
    temporal = TemporalRegistration(matrices, stabilization)
    tracker = FlowRegistration()
    whole_frame = GlobalTranslationStabilizer()
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
        output_name = 'composite_dynamic' if dynamic else 'composite'
        if dynamic and registration_method == 'tracking':
            output_name += '_tracking'
        if global_stabilization:
            output_name += '_stabilized'
        destination = folder / (output_name + '.avi')
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
            method = None
            correction = np.zeros(2)
            global_report = None
            if global_stabilization:
                correction, global_report = whole_frame.update(gray[0])
            if dynamic:
                try:
                    if registration_method == 'tracking':
                        result = tracker.estimate(gray)
                        method = result['method']
                    else:
                        result = automatic_registration(gray)
                        method = 'orb'
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
                    'method': method, 'global_stabilization': global_report, 'filter_state': filter_state, 'quality': quality, 'matrices': [h.tolist() for h in matrices]}) + '\n')
            gray = [warp_channel(g, matrices[i], offsets[i]) for i, g in enumerate(gray)]
            if progress:
                progress({'running': True, 'done': number+1, 'total': len(targets),
                          'estimated': successful, 'fallback': fallback})
            composite=cv2.merge([gray[mapping[2]], gray[mapping[1]], gray[mapping[0]]])
            if global_stabilization:
                composite=cv2.warpAffine(composite,np.float32([[1,0,correction[0]],[0,1,correction[1]]]),(640,480))
            writer.write(composite)
        (folder / 'alignment.json').write_text(json.dumps({'rgb_cameras': mapping, 'xy_offsets': offsets, 'points': points, 'adjustments': adjustments, 'dynamic': dynamic, 'stabilization': stabilization, 'global_stabilization': global_stabilization, 'registration_method': registration_method}, indent=2))
        summary = {'file': str(destination), 'frames': len(targets), 'fps': round(fps, 2),
                   'dynamic': dynamic, 'stabilization': stabilization, 'global_stabilization': global_stabilization, 'registration_method': registration_method, 'estimated': successful, 'fallback': fallback,
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
                        body.get('adjustments'), body.get('dynamic', False), update_progress, body.get('stabilization', 'balanced'), body.get('global_stabilization', False), body.get('registration_method', 'orb')))
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
