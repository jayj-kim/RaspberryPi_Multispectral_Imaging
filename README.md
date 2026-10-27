# RaspberryPi Multispectral Imaging

Raspberry Pi 5 + USB 카메라 3대용 로컬 브라우저 프로그램.
현재 기능: 3채널 미리보기, 동시 녹화 시작/중지, 카메라별 AVI/시간 CSV 저장,
완료된 세션의 시간 기반 RGB 합성, 카메라별 X/Y 이동 설정 저장.
필터 장착 전에는 파장별 다분광 측정이 아닌 기능 개발용입니다.

## Raspberry Pi 설치

64비트 Raspberry Pi OS에서 저장소 디렉터리로 이동한 후 실행합니다.
OS 패키지의 OpenCV를 사용하므로 Pi에서 pip로 OpenCV를 빌드할 필요가 없습니다.

```bash
sudo apt update
sudo apt install -y python3-venv python3-opencv python3-numpy python3-flask v4l-utils ffmpeg
python3 -m venv --system-site-packages .venv
source .venv/bin/activate
python -c "import cv2, numpy, flask; print(cv2.__version__)"
python app.py
```

라즈베리파이의 브라우저에서 `http://127.0.0.1:8000`을 엽니다.
다른 촬영 프로그램은 먼저 종료하세요. 서버 종료는 터미널의 Ctrl+C입니다.
서버는 로컬 접속만 허용하며 외부 인터넷에 노출하지 마세요.
다른 PC에서는 SSH 터널 `ssh -L 8000:127.0.0.1:8000 jonginkim@<Pi주소>`을
연결한 다음 해당 PC의 브라우저에서 같은 주소를 사용할 수 있습니다.

### 카메라 연결

`app.py`의 `DEVICES`는 다음 포트로 설정되어 있습니다.
USB 포트를 바꾼 경우 `ls -l /dev/v4l/by-path/`로 확인해 수정하세요.

| 이름 | /dev/v4l/by-path/ 아래 경로 |
|---|---|
| cam1 | platform-xhci-hcd.1-usb-0:2:1.0-video-index0 |
| cam2 | platform-xhci-hcd.0-usb-0:2:1.0-video-index0 |
| cam3 | platform-xhci-hcd.1-usb-0:1:1.0-video-index0 |

입력은 YUYV 640×480, 요청 프레임률 30fps입니다.
실행 시 수동 노출(`auto_exposure=1`, `exposure_time_absolute=625`)을 적용합니다.
종료 후에도 설정은 남습니다. 필요하면 각 장치에
`v4l2-ctl -d /dev/video0 --set-ctrl=auto_exposure=3`처럼 자동 노출을 복원하세요.
권한 오류는 `id`로 video 그룹 가입 여부를 확인하세요. 필요한 경우
`sudo usermod -aG video "$USER"` 후 로그아웃/로그인하세요.
카메라가 끊기면 녹화가 종료되며, 연결을 복구한 후 프로그램을 재시작하세요.

## 사용 순서

1. 세 미리보기와 프레임률을 확인합니다.
2. 녹화 시작 → 녹화 중지. 먼저 짧은 10초 촬영으로 저장 성능을 확인하세요.
3. 완료된 세션을 선택하고 RGB 채널에 서로 다른 카메라를 배치합니다.
4. 필요한 X/Y 이동값을 입력하고 합성 영상 저장을 누릅니다.
5. `captures/<세션>/composite.avi`를 VLC 등에서 확인합니다.
   합성은 동기 처리이므로 완료까지 기다리세요. 긴 영상은 시간이 걸립니다.

세션마다 `cam1~3.avi`(MJPEG), `cam1~3.csv`, `session.json`을 저장합니다.
합성 후 `alignment.json`과 `composite.avi`가 추가됩니다.
재합성은 이 두 파일을 덮어씁니다. 중요한 결과는 별도 복사하세요.
위치값은 현재 GUI에서 자동 복원되지 않으며 JSON으로 보존됩니다.
저장 위치는 `python app.py --output /원하는/디렉터리`로 변경합니다.
저장 공간을 확인하세요. MJPEG 인코딩과 디스크 성능은 실제 Pi에서 검증해야 합니다.

## 시간 및 정렬의 한계

CSV는 프레임 수신 직후의 호스트 단조 시각이며 센서 노출 시각이 아닙니다.
합성은 공통 촬영 구간에서 cam1의 시각마다 가장 가까운 다른 카메라 프레임을 선택합니다.
이는 하드웨어 동기화를 보장하지 않으며 버퍼 지연·움직이는 물체의 오차가 남습니다.
AVI는 고정 프레임률 30으로 기록하므로 드롭 시 원본 재생 시간이 실제와 다를 수 있습니다.
합성에서는 CSV를 사용하지만 출력 역시 중앙 프레임 간격을 사용한 고정 프레임률입니다.
현재 정렬은 평행 이동만 지원합니다. 회전/배율, 시간 오프셋 수동 보정,
실시간 합성, 저장 공간 감시, 자동 재연결은 후속 구현 대상입니다.
단일 평면 정렬로 서로 다른 거리의 물체에 생기는 시차를 모두 없앨 수는 없습니다.

## 하드웨어 없는 개발 및 테스트

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python -m unittest -v test_workflow.py
python app.py --demo
```

DEMO는 가상 영상입니다. 실제 카메라 성능을 검증하지 않습니다.
클라우드 검증에서는 녹화·시간 CSV·합성 파일 디코딩·채널 출력·요청 검사를 실행합니다.
실제 Pi에서 세 대 동시 수신 약 30fps는 확인했으나 이 프로그램의 실제 녹화 성능은 별도 확인해야 합니다.

## 참고

설계 참고: https://github.com/Project-326/True-Color-NIR-Camera
이 구현은 별도로 작성했으며 원본 소스를 복사하지 않았습니다.
