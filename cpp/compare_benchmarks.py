"""Run native/Python pipelines on identical files and report speed AND output differences.
The driver needs Python; the C++ executable itself does not.
"""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

import cv2
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from app import (automatic_registration,registration_matrices,TemporalRegistration,
                 LiveLocalRefinement,PerformanceProfile,warp_channel)


def load_session(folder):
    import csv
    times=[]
    for i in range(3):
        with (folder/f'cam{i+1}.csv').open() as f:
            times.append(np.array([int(row['monotonic_ns']) for row in csv.DictReader(f)],dtype=np.int64))
    targets=times[0][(times[0]>=max(t[0] for t in times))&(times[0]<=min(t[-1] for t in times))]
    if len(targets)<2:raise ValueError('No common frames')
    return times,targets


def python_benchmark(folder,config,report,stride):
    cv2.setRNGSeed(0)
    times,targets=load_session(folder)
    fps=float(1e9/np.median(np.diff(targets)))
    caps=[cv2.VideoCapture(str(folder/f'cam{i+1}.avi')) for i in range(3)]
    if not all(c.isOpened() for c in caps):raise ValueError('Cannot open sources')
    video=report.with_suffix('.avi')
    writer=cv2.VideoWriter(str(video),cv2.VideoWriter_fourcc(*'MJPG'),fps/stride,(640,480))
    if not writer.isOpened():raise ValueError('Cannot create benchmark video')
    positions=[-1]*3
    frames=[None]*3
    matrices=registration_matrices(config.get('points'),config.get('adjustments'))
    temporal=TemporalRegistration(matrices,'responsive')
    refiner=LiveLocalRefinement(config['flow_width'])
    profiler=PerformanceProfile({**config,'implementation':'Python','opencv':cv2.__version__,
        'opencv_threads':cv2.getNumThreads(),'python':sys.version.split()[0],
        'machine':platform.machine(),'numpy':np.__version__,'opencv_build_info':cv2.getBuildInformation(),'benchmark_stride':stride})
    last_refresh=-1e30
    try:
        for stamp in targets[::stride]:
            for i in range(3):
                idx=int(np.argmin(abs(times[i]-stamp)))
                while positions[i]<idx:
                    ok,frames[i]=caps[i].read()
                    if not ok:raise ValueError('Source shorter than CSV')
                    positions[i]+=1
            started=time.monotonic();tick=time.perf_counter()
            gray=[cv2.cvtColor(f,cv2.COLOR_BGR2GRAY) for f in frames]
            stages={'frame_pick_ms':0.,'grayscale_ms':(time.perf_counter()-tick)*1000,'global_ms':0.}
            input_time=float(stamp-targets[0])/1e9
            refresh=input_time-last_refresh>=1
            if refresh:
                last_refresh=input_time;tick=time.perf_counter()
                try:
                    estimate=automatic_registration(gray)
                    matrices,_=temporal.update(registration_matrices(estimate['points'],config.get('adjustments')))
                except (ValueError,cv2.error,np.linalg.LinAlgError):pass
                stages['global_ms']=(time.perf_counter()-tick)*1000
            tick=time.perf_counter()
            warped=[warp_channel(g,matrices[i],config.get('offsets',[[0,0]]*3)[i]) for i,g in enumerate(gray)]
            stages['global_warp_ms']=(time.perf_counter()-tick)*1000
            corrected,checked,_=refiner.refine(warped)
            stages.update(refiner.last_timings)
            tick=time.perf_counter();mapping=config.get('mapping',[0,1,2])
            image=cv2.merge([corrected[mapping[2]],corrected[mapping[1]],corrected[mapping[0]]])
            stages['merge_ms']=(time.perf_counter()-tick)*1000
            tick=time.perf_counter()
            ok,_=cv2.imencode('.jpg',image,[cv2.IMWRITE_JPEG_QUALITY,75])
            if not ok:raise ValueError('JPEG failed')
            stages['jpeg_ms']=(time.perf_counter()-tick)*1000
            total=(time.monotonic()-started)*1000
            stages['other_ms']=max(0,total-sum(stages.values()));stages['total_ms']=total
            profiler.record(stages,refresh,checked)
            writer.write(image)
    finally:
        writer.release()
        for c in caps:c.release()
    result=profiler.summary();result['video']=str(video)
    report.write_text(json.dumps(result,indent=2))
    return result


def output_difference(a,b):
    caps=[cv2.VideoCapture(str(p)) for p in (a,b)]
    sums=[];maximum=0.;psnr=[]
    try:
        while True:
            frames=[c.read() for c in caps]
            if not any(ok for ok,_ in frames):break
            if not all(ok for ok,_ in frames):raise ValueError('Benchmark output frame counts differ')
            x,y=frames[0][1],frames[1][1]
            delta=np.abs(x.astype(np.float32)-y.astype(np.float32))
            sums.append(float(delta.mean()));maximum=max(maximum,float(delta.max()))
            mse=float((delta**2).mean())
            if mse>0:psnr.append(float(10*np.log10(255**2/mse)))
    finally:
        for c in caps:c.release()
    return {'frames':len(sums),'mean_absolute_pixel_difference':float(np.mean(sums)),
            'p95_frame_mean_difference':float(np.percentile(sums,95)),'max_channel_difference':maximum,
            'median_psnr_db':float(np.median(psnr)) if psnr else None,
            'note':'Agreement between implementations, not a ground-truth registration accuracy metric. MJPEG quantization is included.'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session',type=Path)
    parser.add_argument('--cpp',type=Path,default=ROOT/'cpp/build/imaging_cpp')
    parser.add_argument('--output',type=Path,default=ROOT/'benchmarks'/time.strftime('%Y%m%d_%H%M%S'))
    parser.add_argument('--flow-width',type=int,choices=[320,480,640],default=320)
    parser.add_argument('--threads',type=int,default=2)
    parser.add_argument('--cpp-backend',choices=['cpu','vulkan','auto'],default='cpu')
    parser.add_argument('--stride',type=int,default=1)
    parser.add_argument('--repeat',type=int,default=3)
    args=parser.parse_args()
    if min(args.threads,args.stride,args.repeat)<1:parser.error('Counts must be positive')
    if args.output.exists() and any(args.output.iterdir()):parser.error('Output directory must be empty to preserve prior reports')
    args.output.mkdir(parents=True,exist_ok=True)
    cv2.setNumThreads(args.threads)
    config={'flow_width':args.flow_width,'target_fps':30,'mapping':[0,1,2],'offsets':[[0,0]]*3}
    settings=args.output/'settings.json';settings.write_text(json.dumps(config))
    results={'python':[],'cpp':[]}
    for repeat in range(args.repeat):
        order=['python','cpp'] if repeat%2==0 else ['cpp','python']
        for implementation in order:
            report=args.output/f'{implementation}_{repeat+1}.json'
            print(f'Running {implementation} {repeat+1}/{args.repeat}',flush=True)
            if implementation=='python':result=python_benchmark(args.session,config,report,args.stride)
            else:
                subprocess.run([str(args.cpp.resolve()),'--benchmark-session',str(args.session.resolve()),
                    '--settings',str(settings.resolve()),'--report',str(report.resolve()),
                    '--cv-threads',str(args.threads),'--stride',str(args.stride),'--backend',args.cpp_backend],check=True,stdout=subprocess.DEVNULL)
                result=json.loads(report.read_text())
                if result['config']['opencv']!=cv2.__version__:
                    raise RuntimeError(f'OpenCV mismatch: Python {cv2.__version__}, C++ {result["config"]["opencv"]}; comparison stopped')
                if result['config']['opencv_build_info']!=cv2.getBuildInformation():
                    raise RuntimeError('OpenCV build options differ; use the same system OpenCV for both implementations')
            results[implementation].append(result)
    means={key:float(np.median([r['stages']['total_ms']['mean_ms'] for r in rows])) for key,rows in results.items()}
    comparison={'host_machine':platform.machine(),'opencv':cv2.__version__,'threads':args.threads,
        'flow_width':args.flow_width,'stride':args.stride,'repeat':args.repeat,'opencv_build_match':True,
        'median_run_mean_ms':means,'cpp_speedup':means['python']/means['cpp'],
        'cpp_backend':results['cpp'][-1]['config'].get('local_backend'),
        'cpu_pipeline_capacity_fps':{key:1000/value for key,value in means.items()},
        'output_comparison':output_difference(results['python'][-1]['video'],results['cpp'][-1]['video']),
        'notes':['Not a camera/browser end-to-end fps measurement.',
                 'Same timestamp timeline and ORB refresh schedule; input decoding/output video write excluded.',
                 'No target-fps sleep. First five outputs excluded in each run.',
                 'Run order alternates. On Pi run with other camera apps stopped.',
                 'C++ includes fused memory loops, so this measures the rewrite plus implementation optimizations.']}
    (args.output/'comparison.json').write_text(json.dumps(comparison,indent=2))
    print(json.dumps(comparison,indent=2))
    print('Saved:',args.output/'comparison.json')


if __name__=='__main__':main()
