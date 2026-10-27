"""Hardware-independent end-to-end recorder and compositor checks."""
import tempfile
import time
import unittest
from pathlib import Path
import csv
import cv2
import numpy as np
from app import Rig, create_app


class WorkflowTest(unittest.TestCase):
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
            finally:
                rig.close()


if __name__ == '__main__':
    unittest.main()
