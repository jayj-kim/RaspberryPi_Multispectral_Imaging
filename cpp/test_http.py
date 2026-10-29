"""Exercise the standalone C++ HTTP server; no Python server is started."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.request
import urllib.error


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,default=Path(__file__).resolve().parent/'build/imaging_cpp')
    args=parser.parse_args()
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    with tempfile.TemporaryDirectory() as tmp:
        with open(Path(tmp)/'server.log','w+') as log:
            process=subprocess.Popen([str(args.binary.resolve()),'--demo','--port',str(port),'--output',tmp],stdout=log,stderr=log)
            base=f'http://127.0.0.1:{port}'
            def request(path,body=None,headers=None):
                hdr={'Content-Type':'application/json','X-Rig-Request':'1'}
                if headers is not None:hdr=headers
                req=urllib.request.Request(base+path,data=None if body is None else json.dumps(body).encode(),headers=hdr)
                try:
                    with urllib.request.urlopen(req,timeout=30) as r:return r.status,r.headers,r.read()
                except urllib.error.HTTPError as r:return r.code,r.headers,r.read()
            try:
                for _ in range(100):
                    try:
                        if request('/api/status')[0]==200:break
                    except OSError:pass
                    time.sleep(.05)
                else:raise RuntimeError('Server did not start')
                assert request('/')[0]==200
                code,headers,data=request('/api/cameras/settings')
                assert code==200 and 'attachment' in headers['Content-Disposition']
                diagnostics=json.loads(data)
                assert diagnostics['demo'] is True and len(diagnostics['cameras'])==3
                assert diagnostics['current_controls']==[]
                assert request('/api/live/start',{},headers={})[0]==403
                assert request('/api/live/start',{},headers={'X-Rig-Request':'1','Origin':'https://example.com'})[0]==403
                for _ in range(100):
                    data=json.loads(request('/api/status')[2])
                    if all(c['error'] is None for c in data['cameras']):break
                    time.sleep(.05)
                code,_,error=request('/api/live/start',{'mapping':[0,0,2]})
                assert code==400 and 'R=cam1' in json.loads(error)['error']
                assert request('/api/live/start',{'cpu_remap':'invalid'})[0]==400
                assert request('/api/live/start',{'flow_width':320,'target_fps':10,'cpu_remap':'fixed'})[0]==200
                assert request('/api/live/start',{})[0]==400
                assert request('/api/start',{})[0]==200
                time.sleep(1)
                recorded=json.loads(request('/api/stop',{})[2])['session']
                assert (Path(tmp)/recorded/'session.json').is_file()
                assert request('/api/compose',{'session':recorded})[0]==400
                with urllib.request.urlopen(base+'/live/stream',timeout=10) as stream:
                    assert stream.read(100).startswith(b'--frame\r\nContent-Type: image/jpeg')
                for _ in range(100):
                    if json.loads(request('/api/status')[2])['live']['processed']>=8:break
                    time.sleep(.1)
                else:raise RuntimeError('Live profiling did not finish warmup')
                assert request('/api/live/stop',{})[0]==200
                code,headers,profile=request('/api/live/profile?download=1')
                assert code==200 and 'attachment' in headers['Content-Disposition']
                assert json.loads(profile)['outputs_seen']>0
                assert json.loads(profile)['config']['local_backend']['cpu_remap']=='fixed'
                assert 'cpu_map_convert_ms' in json.loads(profile)['stages']
                assert recorded in json.loads(request('/api/sessions')[2])
                body={'session':recorded,'mapping':[0,1,2],'offsets':[[0,0]]*3}
                code,headers,image=request('/api/preview',{**body,'raw':0})
                assert code==200 and image.startswith(b'\xff\xd8')
                assert request('/api/preview',{**body,'session':'../escape'})[0]==400
                code,_,result=request('/api/compose',{**body,'dynamic':True,'local_refinement':True,'temporal_window':15})
                assert code==200,result
                output=json.loads(result)
                assert Path(output['file']).is_file() and Path(output['validity_file']).is_file()
                assert (Path(tmp)/recorded/'registration_smoothed.jsonl').is_file()
                assert request('/api/live/start',{'target_fps':30})[0]==200
                assert request('/api/live/stop',{})[0]==200
                print('PASS: native HTTP security, live MJPEG, recording, profile, sessions, preview, local offline composition, restart')
            except Exception:
                log.flush();log.seek(0);print(log.read());raise
            finally:
                process.terminate()
                try:process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill();process.wait();raise RuntimeError('Server shutdown hung')
            assert process.returncode==0,process.returncode


if __name__=='__main__':main()
