"""Verify actual Vulkan selection or an explicitly labelled CPU fallback over HTTP."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,default=Path(__file__).resolve().parent/'build/imaging_cpp')
    parser.add_argument('--software',action='store_true',help='Explicitly allow software Vulkan for correctness tests, never performance claims')
    parser.add_argument('--fallback',action='store_true',help='Force no ICD and verify the CPU fallback')
    args=parser.parse_args()
    if args.software and args.fallback:parser.error('Choose one mode')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    with tempfile.TemporaryDirectory() as folder:
        environment=os.environ.copy()
        if args.fallback:
            missing=str(Path(folder)/'missing-icd.json')
            environment['VK_DRIVER_FILES']=missing
            environment['VK_ICD_FILENAMES']=missing
        command=[str(args.binary.resolve()),'--demo','--port',str(port),'--backend','vulkan','--output',folder]
        if args.software:command.append('--allow-software-vulkan')
        with open(Path(folder)/'server.log','w+') as log:
            process=subprocess.Popen(command,env=environment,stdout=log,stderr=log)
            base=f'http://127.0.0.1:{port}'
            def request(path,body=None):
                req=urllib.request.Request(base+path,data=None if body is None else json.dumps(body).encode(),
                    headers={'Content-Type':'application/json','X-Rig-Request':'1'})
                with urllib.request.urlopen(req,timeout=20) as response:return response.read()
            try:
                for _ in range(100):
                    try:
                        page=request('/').decode();break
                    except OSError:time.sleep(.05)
                else:raise RuntimeError('Server did not start')
                assert "value='vulkan'" in page
                request('/api/live/start',{'flow_width':320,'target_fps':10})
                for _ in range(300):
                    state=json.loads(request('/api/status'))['live']
                    if state.get('processed',0)>=7:break
                    if not state['running']:raise RuntimeError(state)
                    time.sleep(.05)
                else:raise RuntimeError('No live frames')
                expected='cpu' if args.fallback else 'vulkan'
                assert state['backend']['actual']==expected,state
                if args.fallback:assert state['backend']['warning']
                else:
                    assert state['backend']['device']['software']==args.software,state
                    assert state['profile']['stages']['gpu_submit_wait_ms']['active_count']>0
                with urllib.request.urlopen(base+'/live/stream',timeout=10) as response:
                    assert response.read(100).startswith(b'--frame\r\nContent-Type: image/jpeg')
                request('/api/live/stop',{})
                profile=json.loads(request('/api/live/profile?download=1'))
                assert profile['config']['local_backend']['actual']==expected
                assert profile['measured_frames']>0
                print('PASS: Vulkan HTTP selection/profile/MJPEG' if not args.fallback else 'PASS: unavailable-Vulkan CPU fallback is explicit in HTTP/profile')
            except Exception:
                log.flush();log.seek(0);print(log.read());raise
            finally:
                process.terminate()
                try:process.wait(timeout=15)
                except subprocess.TimeoutExpired:process.kill();process.wait();raise RuntimeError('Shutdown hung')
            assert process.returncode==0,process.returncode


if __name__=='__main__':main()
