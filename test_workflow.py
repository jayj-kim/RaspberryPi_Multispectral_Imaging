"""Hardware-independent end-to-end recorder and compositor checks."""
import tempfile
import time
import unittest
from pathlib import Path
import csv
import json
import cv2
import numpy as np
from app import Rig, create_app, registration_matrices, warp_channel, automatic_registration, TemporalRegistration


class WorkflowTest(unittest.TestCase):
    def test_perspective_registration(self):
        reference = np.float32([[80,60],[560,60],[560,420],[80,420]])
        source = np.float32([[100,80],[510,50],[550,390],[65,400]])
        matrices = registration_matrices([reference.tolist(), source.tolist(), []])
        projected = cv2.perspectiveTransform(source[None], matrices[1])[0]
        np.testing.assert_allclose(projected, reference, atol=1e-3)
        # Interior points must also agree, not merely the four selected corners.
        original = np.zeros((480,640), np.uint8)
        for x,y in [(160,120),(320,240),(480,360)]:
            cv2.circle(original,(x,y),12,255,-1)
        distorted = cv2.warpPerspective(original, np.linalg.inv(matrices[1]), (640,480))
        corrected = warp_channel(distorted, matrices[1], [0,0])
        self.assertLess(float(np.abs(original.astype(float)-corrected).mean()), 1)
        with self.assertRaises(ValueError):
            registration_matrices([reference.tolist(), [[10,10],[20,20],[30,30],[40,40]], []])
        with self.assertRaises(ValueError):
            registration_matrices([[], source.tolist(), []])
        with self.assertRaises(ValueError):
            registration_matrices(adjustments=[[0,0], [0,1], [0,1]])

    def test_automatic_registration(self):
        rng = np.random.default_rng(42)
        reference = cv2.GaussianBlur(rng.integers(0, 256, (480,640), dtype=np.uint8), (3,3), 0)
        known = [np.eye(3), np.array([[1.02,-.025,8],[.018,.98,-5],[.00004,-.00003,1]]),
                 np.array([[.98,.02,-7],[-.015,1.015,9],[-.00003,.00004,1]])]
        frames = [cv2.warpPerspective(reference, np.linalg.inv(h), (640,480)) for h in known]
        estimated = automatic_registration(frames)
        matrices = registration_matrices(estimated['points'])
        probes = np.float32([[[100,100],[300,200],[500,350]]])
        for i in (1,2):
            actual = cv2.perspectiveTransform(probes, matrices[i])
            expected = cv2.perspectiveTransform(probes, known[i])
            self.assertLess(float(np.max(np.linalg.norm(actual-expected, axis=2))), 2)
            self.assertGreaterEqual(estimated['reports'][i-1]['inliers'], 15)
        with self.assertRaises(ValueError):
            automatic_registration([np.zeros((480,640),np.uint8)]*3)
        with self.assertRaises(ValueError):
            automatic_registration([reference, rng.integers(0,256,(480,640),dtype=np.uint8), frames[2]])
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)/'fixture'
            folder.mkdir()
            (folder/'session.json').write_text('{}')
            for i, frame in enumerate(frames):
                writer = cv2.VideoWriter(str(folder/f'cam{i+1}.avi'), cv2.VideoWriter_fourcc(*'MJPG'), 30, (640,480))
                self.assertTrue(writer.isOpened())
                for _ in range(3): writer.write(cv2.cvtColor(frame,cv2.COLOR_GRAY2BGR))
                writer.release()
                (folder/f'cam{i+1}.csv').write_text('frame,monotonic_ns\n0,1000000000\n1,1033333333\n2,1066666666\n')
            client = create_app(Rig(demo=True, output=temp)).test_client()
            body = {'session':'fixture','mapping':[0,1,2],'offsets':[[0,0]]*3,'auto_register':True}
            response = client.post('/api/preview',json=body,headers={'X-Rig-Request':'1'})
            self.assertEqual(response.status_code,200,response.json)
            self.assertEqual(len(response.json['reports']),2)
            self.assertFalse((folder/'alignment.json').exists())

    def test_temporal_stability_and_motion(self):
        def matrices(dx):
            return [np.eye(3),np.array([[1.,0,dx],[0,1.,0],[0,0,1.]]),np.eye(3)]
        initial=matrices(0)
        temporal=TemporalRegistration(initial, 'strong')
        temporal.update(initial)
        raw, filtered=[],[]
        for n in range(80):
            dx=2 if n%2 else -2
            result,event=temporal.update(matrices(dx))
            raw.append(dx)
            filtered.append(float(result[1][0,2]))
        self.assertLess(np.std(filtered),np.std(raw)*.3)
        before=temporal.last[1].copy()
        result,event=temporal.update(matrices(80))
        self.assertEqual(event,'held_jump')
        np.testing.assert_allclose(result[1],before)
        temporal.update(matrices(0))
        # An isolated outlier is not adopted. A sustained real change can reacquire.
        self.assertLess(abs(float(temporal.last[1][0,2])),1)
        for n in range(3): result,event=temporal.update(matrices(80))
        self.assertEqual(event,'reacquired')
        self.assertAlmostEqual(float(result[1][0,2]),80)
        temporal=TemporalRegistration(initial,'balanced')
        temporal.update(initial)
        for n in range(1,50):
            result,event=temporal.update(matrices(n))
        self.assertLess(abs(float(result[1][0,2])-49),3)
        with self.assertRaises(ValueError):
            TemporalRegistration(initial,'invalid')

    def test_dynamic_video_registration_and_fallback(self):
        rng = np.random.default_rng(9)
        texture = cv2.GaussianBlur(rng.integers(0,256,(480,640),dtype=np.uint8),(3,3),0)
        known = {}
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)/'moving'
            folder.mkdir()
            (folder/'session.json').write_text('{}')
            (folder/'composite.avi').write_bytes(b'preserve static output')
            writers = [cv2.VideoWriter(str(folder/f'cam{i+1}.avi'),cv2.VideoWriter_fourcc(*'MJPG'),30,(640,480)) for i in range(3)]
            for n in range(6):
                transforms = [np.eye(3), np.array([[1+n*.003, -.01, 6+n*2],[.01,1,-3],[0,0,1.]]),
                              np.array([[1,-.01,-6-n*2],[.01,1+n*.003,3],[0,0,1.]])]
                known[n] = transforms
                for i in range(3):
                    frame = np.zeros_like(texture) if n in (0,5) else cv2.warpPerspective(texture,np.linalg.inv(transforms[i]),(640,480))
                    writers[i].write(cv2.cvtColor(frame,cv2.COLOR_GRAY2BGR))
            for i,w in enumerate(writers):
                w.release()
                (folder/f'cam{i+1}.csv').write_text('frame,monotonic_ns\n'+''.join(f'{n},{1000000000+n*33333333}\n' for n in range(6)))
            client = create_app(Rig(demo=True,output=temp)).test_client()
            response = client.post('/api/compose',headers={'X-Rig-Request':'1'},json={
                'session':'moving','mapping':[0,1,2],'offsets':[[0,0]]*3,'dynamic':True,'stabilization':'responsive'})
            self.assertEqual(response.status_code,200,response.json)
            self.assertEqual(response.json['estimated'],4)
            self.assertEqual(response.json['fallback'],2)
            records = [json.loads(line) for line in (folder/'registration_dynamic.jsonl').read_text().splitlines()]
            self.assertEqual(len(records),6)
            self.assertEqual(records[0]['state'],'fallback_initial')
            self.assertEqual(records[-1]['state'],'fallback_previous')
            np.testing.assert_allclose(records[-1]['matrices'],records[-2]['matrices'])
            probes=np.float32([[[150,150],[320,240],[480,320]]])
            for n in range(1,5):
                self.assertEqual(records[n]['state'],'estimated')
                for i in (1,2):
                    estimated=cv2.perspectiveTransform(probes,np.array(records[n]['matrices'][i]))
                    expected=cv2.perspectiveTransform(probes,known[n][i])
                    self.assertLess(float(np.max(np.linalg.norm(estimated-expected,axis=2))),3)
            cap=cv2.VideoCapture(response.json['file'])
            count=0
            while cap.read()[0]: count+=1
            cap.release()
            self.assertEqual(count,6)
            self.assertEqual((folder/'composite.avi').read_bytes(),b'preserve static output')
            self.assertTrue(json.loads((folder/'registration_dynamic_summary.json').read_text())['completed'])
            progress=client.get('/api/status').json['processing']
            self.assertFalse(progress['running'])
            self.assertEqual(progress['done'],6)

    def test_record_and_compose(self):
        with tempfile.TemporaryDirectory() as temp:
            rig = Rig(demo=True, output=Path(temp))
            rig.start()
            try:
                client = create_app(rig).test_client()
                headers = {'X-Rig-Request': '1'}
                deadline = time.monotonic() + 5
                while any(s['error'] for s in rig.status) and time.monotonic() < deadline:
                    time.sleep(.02)
                self.assertEqual(client.get('/').status_code, 200)
                self.assertEqual(client.post('/api/start').status_code, 403)
                self.assertEqual(client.post('/api/start', headers={**headers, 'Origin': 'https://example.com'}).status_code, 403)
                response = client.post('/api/start', headers=headers)
                self.assertEqual(response.status_code, 200, response.json)
                name = response.json['session']
                self.assertEqual(client.post('/api/start', headers=headers).status_code, 400)
                time.sleep(1)
                self.assertEqual(client.post('/api/stop', headers=headers).status_code, 200)
                for i in range(3):
                    with (Path(temp)/name/f'cam{i+1}.csv').open() as f:
                        rows = list(csv.DictReader(f))
                    self.assertGreater(len(rows), 5)
                    self.assertTrue(all(int(b['monotonic_ns']) > int(a['monotonic_ns']) for a,b in zip(rows, rows[1:])))
                payload = {'session': name, 'mapping': [0,1,2], 'offsets': [[0,0]]*3}
                preview = client.post('/api/preview', headers=headers, json={**payload, 'position': .5})
                self.assertEqual(preview.status_code, 200, preview.data[:100])
                image = cv2.imdecode(np.frombuffer(preview.data, np.uint8), cv2.IMREAD_COLOR)
                self.assertEqual(image.shape, (480, 640, 3))
                moved = client.post('/api/preview', headers=headers, json={**payload, 'offsets': [[100, 0], [0, 0], [0, 0]]})
                self.assertEqual(moved.status_code, 200)
                shifted = cv2.imdecode(np.frombuffer(moved.data, np.uint8), cv2.IMREAD_COLOR)
                self.assertGreater(int(image[400, 50, 2]), int(shifted[400, 50, 2]) + 10)
                rejected = client.post('/api/preview', headers=headers, json={**payload, 'session': '../escape'})
                self.assertEqual(rejected.status_code, 400)
                bad = client.post('/api/compose', headers=headers, json={**payload, 'mapping': [0,0,2]})
                self.assertEqual(bad.status_code, 400)
                raw = client.post('/api/preview', headers=headers, json={**payload, 'raw': 1})
                self.assertEqual(raw.status_code, 200)
                self.assertEqual(cv2.imdecode(np.frombuffer(raw.data, np.uint8), 0).shape, (480,640))
                reference = [[80,60],[560,60],[560,420],[80,420]]
                payload.update(points=[reference, [[100,80],[510,50],[550,390],[65,400]], reference], adjustments=[[0,1],[1,1.01],[0,1]])
                registered = client.post('/api/preview', headers=headers, json=payload)
                self.assertEqual(registered.status_code, 200, registered.data[:100])
                response = client.post('/api/compose', headers=headers, json=payload)
                self.assertEqual(response.status_code, 200, response.json)
                self.assertGreater(response.json['frames'], 5)
                cap = cv2.VideoCapture(response.json['file'])
                count = 0
                while True:
                    ok, frame = cap.read()
                    if not ok: break
                    self.assertEqual(frame.shape, (480,640,3))
                    # All three source intensities must reach their output channels.
                    self.assertTrue((frame[400,400] > 8).all())
                    count += 1
                cap.release()
                self.assertEqual(count, response.json['frames'])
                self.assertIn(name, client.get('/api/sessions').json)
                saved = client.post('/api/preview', headers=headers, json={'session': name, 'load_profile': True})
                self.assertEqual(saved.status_code, 200)
                self.assertEqual(saved.json['points'], payload['points'])
                self.assertEqual(saved.json['adjustments'], payload['adjustments'])
            finally:
                rig.close()


if __name__ == '__main__':
    unittest.main()
