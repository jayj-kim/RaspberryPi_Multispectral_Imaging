"""Compare native float/fixed remapping on identical recordings (no Python server)."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import statistics
import subprocess


def compare_video(first, second):
    import cv2
    import numpy as np
    caps = [cv2.VideoCapture(str(p)) for p in (first, second)]
    if not all(cap.isOpened() for cap in caps):
        raise RuntimeError('Cannot open comparison videos')
    frames = total = pixels = maximum = 0
    try:
        while True:
            a, b = [cap.read() for cap in caps]
            if a[0] != b[0]:
                raise RuntimeError('Output frame counts differ')
            if not a[0]:
                break
            diff = np.abs(a[1].astype(np.int16) - b[1].astype(np.int16))
            maximum = max(maximum, int(diff.max()))
            total += int(diff.sum())
            pixels += diff.size
            frames += 1
    finally:
        for cap in caps:
            cap.release()
    if not frames:
        raise RuntimeError('No comparison frames')
    return dict(frames=frames, max_pixel_difference=maximum, mean_pixel_difference=total/pixels)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('--binary', type=Path, default=Path(__file__).parent/'build/imaging_cpp')
    parser.add_argument('--settings', type=Path)
    parser.add_argument('--widths', type=int, nargs='+', choices=[320, 480, 640], default=[320])
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--threads', type=int, default=2)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.repeat < 1 or args.threads < 1:
        parser.error('repeat and threads must be positive')
    import cv2  # Fail before running benchmarks if comparison dependencies are unavailable.
    import numpy
    output = args.output or Path('benchmarks')/datetime.now().strftime('cpu-remap-%Y%m%d-%H%M%S-%f')
    output.mkdir(parents=True, exist_ok=False)
    base = json.loads(args.settings.read_text()) if args.settings else {}
    summary = {'notes': ['Same native binary, CPU backend, alternating order, same CSV timeline.',
                         'First five frames excluded. Decode/write/target waits excluded.',
                         'cpu_* timings are nested; map conversion is included in local_warp_ms.',
                         'Video comparison is decoded MJPEG, not ground truth registration accuracy.'],
               'threads': args.threads, 'widths': {}}
    for width in args.widths:
        reports = {mode: [] for mode in ('float', 'fixed')}
        videos = {}
        for mode in reports:
            options = dict(base, flow_width=width, compute_backend='cpu', cpu_remap=mode)
            (output/f'{width}-{mode}-settings.json').write_text(json.dumps(options, indent=2))
        for run in range(args.repeat):
            for mode in (('float', 'fixed') if run % 2 == 0 else ('fixed', 'float')):
                report = output/f'{width}-{mode}-{run}.json'
                subprocess.run([str(args.binary.resolve()), '--benchmark-session', str(args.session.resolve()),
                                '--settings', str(output/f'{width}-{mode}-settings.json'),
                                '--report', str(report), '--cv-threads', str(args.threads)],
                               check=True, stdout=subprocess.DEVNULL)
                profile = json.loads(report.read_text())
                backend = profile['config']['local_backend']
                if backend['actual'] != 'cpu' or backend['cpu_remap'] != mode:
                    raise RuntimeError('Unexpected actual backend/remap mode')
                reports[mode].append(profile)
                videos.setdefault(mode, Path(profile['video']))
                print(f'{width} {mode} run {run+1}: {profile["stages"]["total_ms"]["mean_ms"]:.3f} ms', flush=True)
        means = {mode: {stage: statistics.mean(p['stages'][stage]['mean_ms'] for p in profiles)
                        for stage in profiles[0]['stages']} for mode, profiles in reports.items()}
        summary['widths'][str(width)] = {
            'mean_ms': means,
            'total_reduction_percent': 100*(1-means['fixed']['total_ms']/means['float']['total_ms']),
            'output_comparison': compare_video(videos['float'], videos['fixed']),
            'runs': {mode: [p['stages']['total_ms']['mean_ms'] for p in profiles]
                     for mode, profiles in reports.items()}}
        (output/'comparison.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(output/'comparison.json')


if __name__ == '__main__':
    main()
