"""Build a local side-by-side comparison without uploading camera footage."""
import argparse
from pathlib import Path

import cv2


def make_comparison(session):
    candidates=sorted(session.glob('*_local.avi'), key=lambda p:p.stat().st_mtime)
    if not candidates:
        raise ValueError('국소 정합 결과가 없습니다. 웹에서 국소 정합을 켜고 합성하세요.')
    local=candidates[-1]
    # A centered-smoothing run retains the unfiltered global baseline, not necessarily
    # an independently rendered smoothed global video.
    preferred=local.with_stem(local.stem.removesuffix('_local'))
    fallback=local.with_stem(local.stem.removesuffix('_local').removesuffix('_smooth'))
    baseline=preferred if preferred.is_file() else fallback
    validity=local.with_stem(local.stem+'_validity')
    if not baseline.is_file():
        raise ValueError('비교할 전역 정합 영상이 없습니다.')
    inputs=[(baseline,'Global'),(local,'Local')]
    if validity.is_file():inputs.append((validity,'Consistency'))
    destination=session/'comparison.avi'
    if destination.exists():
        raise ValueError('comparison.avi가 이미 있습니다. 이름을 바꾸거나 다른 곳으로 옮긴 뒤 다시 실행하세요.')
    caps=[]
    writer=None
    completed=False
    try:
        for path,_ in inputs:
            cap=cv2.VideoCapture(str(path));caps.append(cap)
            if not cap.isOpened():raise ValueError(f'영상 열기 실패: {path}')
        fps=caps[1].get(cv2.CAP_PROP_FPS)
        if fps<=0:raise ValueError('잘못된 영상 프레임률')
        writer=cv2.VideoWriter(str(destination),cv2.VideoWriter_fourcc(*'MJPG'),fps,(640*len(inputs),480))
        if not writer.isOpened():raise ValueError('비교 영상 저장 실패')
        count=0
        while True:
            frames=[cap.read() for cap in caps]
            if not any(ok for ok,_ in frames):break
            if not all(ok for ok,_ in frames):raise ValueError('비교 영상의 프레임 수가 다릅니다.')
            images=[]
            for (ok,frame),(path,label) in zip(frames,inputs):
                if frame.shape[:2]!=(480,640):raise ValueError('640×480 원본 합성 영상이 필요합니다.')
                cv2.rectangle(frame,(0,0),(640,35),(0,0,0),-1)
                cv2.putText(frame,label,(10,25),0,.7,(255,255,255),1,cv2.LINE_AA)
                images.append(frame)
            writer.write(cv2.hconcat(images));count+=1
        if not count:raise ValueError('영상에 프레임이 없습니다.')
        completed=True
        return destination,count
    finally:
        for cap in caps:cap.release()
        if writer is not None:writer.release()
        if not completed:destination.unlink(missing_ok=True)


def main():
    parser=argparse.ArgumentParser(description='국소 정합 전후 비교 영상을 로컬에 만듭니다.')
    parser.add_argument('--session',type=Path,help='captures 아래의 촬영 세션 폴더')
    args=parser.parse_args()
    root=Path(__file__).resolve().parent/'captures'
    try:
        if args.session is None:
            files=list(root.glob('*/*_local.avi'))
            if not files:raise ValueError('국소 정합 결과가 없습니다. 먼저 웹에서 합성하세요.')
            session=max(files,key=lambda p:p.stat().st_mtime).parent
        else:session=args.session
        file,count=make_comparison(session)
        print(f'저장 완료: {file} ({count}프레임)')
        print('왼쪽: 전역 정합 / 가운데: 국소 정합 / 오른쪽: 일관성 검사(있는 경우)')
        print('일관성 검사 흰색도 정확도를 보장하지 않으며, 검은색은 미검증 영역입니다.')
    except (ValueError,OSError,cv2.error) as exc:
        parser.exit(1,str(exc)+'\n')


if __name__=='__main__':main()
