# 독립 C++ 카메라 프로그램

Python 프로그램은 비교 기준으로 그대로 둡니다. C++ 실행 파일은 카메라 입력, 웹 서버,
녹화, 수동/자동 정합, 특징점 추적, 후처리 평활화·국소 정합, 실시간 합성, 계측을 직접 처리합니다.
서버 실행과 영상 처리에 Python 프로세스·Flask를 사용하지 않습니다.
브라우저 화면은 기존 기능을 유지한 정적 HTML/JavaScript입니다.
Python은 선택적인 비교 실행기와 HTTP 검증 도구에서만 사용합니다.

현재 백엔드는 **CPU OpenCV**입니다. Vulkan Compute는 아직 구현하지 않았습니다.

## Raspberry Pi 5 빌드·실행

64비트 Raspberry Pi OS / Debian 13 기준입니다. Python 카메라 프로그램을 먼저 종료하세요.
같은 카메라를 두 프로그램에서 동시에 열면 비교가 성립하지 않습니다.

```bash
cd ~/RaspberryPi_Multispectral_Imaging
git pull origin main
sudo apt update
sudo apt install -y build-essential cmake libopencv-dev libcpp-httplib-dev nlohmann-json3-dev
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j 2
ctest --test-dir cpp/build --output-on-failure
./cpp/build/imaging_cpp --cv-threads 2
```

Pi 브라우저에서 `http://127.0.0.1:8001`을 엽니다. 기본 저장 위치는 `captures_cpp/`입니다.
카메라 USB 포트와 수동 노출 4500은 Python 버전과 같습니다. V4L2 제어는 ioctl로 직접 설정합니다.
카메라 연결 후 원본 세 화면, 짧은 녹화, 실시간 합성을 순서대로 확인하세요.
클라우드에서는 실제 Pi USB 입력을 검증할 수 없으므로 이 부분은 Pi에서 확인해야 합니다.

기존 Python 세션을 그대로 후처리하려면 출력 루트를 명시할 수 있습니다.
이 경우 같은 이름의 합성 파일을 덮어쓸 수 있으므로 먼저 세션을 복사해 비교하세요.

```bash
./cpp/build/imaging_cpp --output captures --port 8001 --cv-threads 2
```

카메라 없는 확인:

```bash
./cpp/build/imaging_cpp --demo
python3 cpp/test_http.py
```

서버는 loopback에만 바인딩합니다. Host/Origin 및 변경 요청의 전용 헤더를 검사합니다.
세션 경로는 출력 루트 안의 완료된 폴더로 제한합니다. 인터넷에 공개하지 마세요.
스레드 풀은 MJPEG 연결 여러 개와 상태 요청을 처리합니다. Ctrl+C로 중지합니다.
장치 드라이버의 읽기가 멈춘 상태에서는 종료가 지연될 수 있습니다.

## Pi에서 실시간 성능 비교

1. Python 버전에서 사용한 것과 동일한 카메라·노출·계산 폭 320·목표 30fps·스레드 2개로 설정합니다.
2. 처음에는 원본 녹화를 끄고 60초 이상 실행합니다.
3. 실시간 합성을 중지하고 JSON을 다운로드합니다. C++ 파일명은 `live_profile_cpp.json`입니다.
4. 필요하면 녹화를 켠 상태로 한 번 더 측정합니다.
5. Python과 C++ 서버는 순서대로 실행하며, 온도·전원 상태·다른 프로세스 조건도 맞추세요.

웹 계측은 양쪽 모두 첫 5개 출력을 제외하며 평균·실행 시 평균·최근 최대 300출력 P95를 집계합니다.
유효성 검사와 국소 리매핑은 C++에서 픽셀 루프를 통합·병렬화했습니다.
따라서 비교 결과는 단순 문법 변환이 아니라 메모리 순회·할당 최적화를 포함한 C++ 구현의 효과입니다.
브라우저 렌더링·카메라 노출·네트워크 전송은 처리 시간에 포함하지 않습니다.

## 같은 저장 영상으로 반복 비교

Pi에서 동일 OpenCV를 사용하는 Python을 선택하세요. 기존 `.venv`가 시스템 OpenCV를 사용하면
`python`으로 실행해도 됩니다. 비교 도구가 OpenCV 버전과 빌드 정보를 검사하고 다르면 중단합니다.
`/usr/bin/python3`로 실행할 경우 Python 서버 모듈의 import를 위해 Flask도 필요합니다.

```bash
sudo apt install -y python3-opencv python3-numpy python3-flask
/usr/bin/python3 cpp/compare_benchmarks.py captures/촬영세션이름 --flow-width 320 --threads 2 --repeat 3
```

출력은 `benchmarks/날짜_시각/comparison.json`과 구현별 JSON·AVI입니다.
이 파일은 카메라 영상이므로 Git에 포함하지 않습니다.
비교 순서를 번갈아 실행하며, 같은 입력 시각·ORB 갱신 주기·일관성 검사 주기로 계산합니다.
원본 디코딩, 결과 AVI 기록, 목표 fps 대기는 CPU 처리 시간에서 제외합니다. JPEG 인코딩은 포함합니다.
`--stride 3`은 입력 세 프레임마다 하나씩 처리합니다. 양쪽 모두 같은 프레임을 사용합니다.
처리시간으로 환산한 fps는 카메라·브라우저를 포함한 실제 fps가 아닙니다.
출력 픽셀 차이는 두 구현 사이의 일치도이며, 정합 정확도의 정답값이 아닙니다.

### 검증된 클라우드 결과

같은 사용자 제공 193프레임, OpenCV 4.10, 스레드 2개, 계산 폭 320, 반복 3회 기준입니다.
클라우드는 **x86_64**이며 아래 수치를 Pi 성능으로 해석하면 안 됩니다.
정확한 수치와 출력 차이는 `results/cloud_x86_64.json`에 기록합니다.

후처리도 같은 193프레임의 ORB/강하게/31프레임/국소 정합 설정으로 양쪽 출력을 생성했습니다.
양쪽 성공/대체값 횟수는 각각 122/71로 같았습니다. 평균 픽셀 차이는 약 0.048/255였습니다.
이것은 동일 기능의 구현 비교이며 손의 대응이 완벽하다는 뜻은 아닙니다.

## C++ 후처리 CLI

```bash
./cpp/build/imaging_cpp --compose-session captures/세션이름 --settings settings.json
./cpp/build/imaging_cpp --compare-session captures/세션이름
```

`settings.json` 예:

```json
{
  "mapping": [0, 1, 2],
  "offsets": [[0, 0], [0, 0], [0, 0]],
  "dynamic": true,
  "registration_method": "orb",
  "stabilization": "strong",
  "temporal_window": 31,
  "local_refinement": true,
  "global_stabilization": false
}
```

수동 `points`, `adjustments`도 기존 형식을 사용합니다.
전역 기준 영상, 최종 합성, validity 영상, 경로 JSONL과 요약을 저장합니다.
`--compare-session`은 기존 `comparison.avi`를 덮어쓰지 않습니다.
후처리는 보정 경로와 진단을 프레임 수에 비례한 메모리에 보관하므로 긴 영상은 나누어 처리하세요.

## 구현 간 차이와 한계

- Python은 유지합니다. C++ 기본 포트는 8001, 기본 데이터 디렉터리는 captures_cpp입니다.
- C++ 프런트엔드는 `cpp/web/index.html`에 미리 렌더링된 독립 정적 파일입니다. 실행 중 Jinja는 필요 없습니다.
- 원본 카메라 녹화와 실시간 미리보기의 동시 실행은 구현했습니다. 실시간 합성 결과 녹화는 양쪽 모두 지원하지 않습니다.
- 시간축 평활화는 후처리에만 있습니다. 실시간 결과는 후처리 결과와 다를 수 있습니다.
- 검사에 실패한 영역도 근사 변형으로 표시될 수 있습니다. validity가 흰색이어도 정확도를 보장하지 않습니다.
- 같은 파일명으로 후처리를 반복하면 합성·진단은 덮어씁니다. 원본 AVI/CSV는 바꾸지 않습니다.
- Pi 하드웨어 동작과 발열·스로틀링에 따른 성능은 Pi에서 측정해야 합니다.

## Vulkan 확장 방향

OpenCV DIS는 이 구현에서 CPU 연산입니다. Vulkan 옵션을 켜는 것만으로 GPU로 옮겨지지 않습니다.
Pi의 Mesa V3DV/Vulkan Compute 지원 상태를 먼저 확인하고, C++ 계측에서 남은 병목에 맞춰
흐름 확대·좌표 생성·유효성 검사·리매핑·RGB 합성을 compute shader로 묶는 경로를 검토합니다.
GPU 데이터 전달/동기화와 결과 회수까지 포함해 비교해야 하며, 정확도와 전력·발열도 함께 확인합니다.
CPU C++ 측정 전부터 GPU 향상 폭을 보장하지 않습니다.
