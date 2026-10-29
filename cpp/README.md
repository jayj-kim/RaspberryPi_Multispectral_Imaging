# 독립 C++ 카메라 프로그램

Python 프로그램은 비교 기준으로 그대로 둡니다. C++ 실행 파일은 카메라 입력, 웹 서버,
녹화, 수동/자동 정합, 특징점 추적, 후처리 평활화·국소 정합, 실시간 합성, 계측을 직접 처리합니다.
서버 실행과 영상 처리에 Python 프로세스·Flask를 사용하지 않습니다.
브라우저 화면은 기존 기능을 유지한 정적 HTML/JavaScript입니다.
Python은 선택적인 비교 실행기와 HTTP 검증 도구에서만 사용합니다.

기본 백엔드는 **CPU OpenCV**이며, 실시간 국소 리매핑에 **Vulkan Compute**를 선택할 수 있습니다.
GPU 장치 확인·설치·측정 방법은 아래 Vulkan 절을 참고하세요.

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

## Vulkan Compute 병렬 처리

### Pi 5 설치·빌드

```bash
sudo apt update
sudo apt install -y libvulkan-dev glslang-tools mesa-vulkan-drivers vulkan-tools
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release -DIMAGING_ENABLE_VULKAN=ON
cmake --build cpp/build -j 2
./cpp/build/imaging_cpp --vulkan-info
./cpp/build/vulkan_test
./cpp/build/imaging_cpp --backend vulkan --cv-threads 2
```

CMake에 `Vulkan compute backend enabled`가 표시되어야 합니다.
`--vulkan-info`의 `compiled`가 true이고, compute 지원 장치의 `software`가 false인지 확인하세요.
Pi의 하드웨어 장치명은 보통 V3D 계열입니다. llvmpipe/lavapipe는 CPU 소프트웨어 드라이버이며
기본 GPU 선택에서 제외합니다. `vulkan_test`는 선택 가능한 하드웨어가 없으면 종료 코드 77로
검사를 건너뜁니다. 건너뜀은 GPU 검증 성공이 아닙니다.

브라우저 `http://127.0.0.1:8001`에서 실시간 백엔드 **Vulkan GPU**를 선택하고 시작합니다.
CPU/자동 선택도 가능하며 설정 변경은 중지 후 재시작해야 반영됩니다.
실제 백엔드, 장치명, 소프트웨어 여부가 화면과 계측 JSON에 표시됩니다.
초기화 또는 디스패치 실패는 CPU로 전환하고 경고합니다. 일부 실패 프레임은 느릴 수 있습니다.
드라이버 자체가 멈춘 경우에는 정상적인 자동 복구를 보장하지 않습니다.

하드웨어가 안 보이면 `vulkaninfo --summary`, `ls -l /dev/dri`, `id` 결과로
드라이버와 render 장치 접근 권한을 확인하세요. headless 실행에서 surface/DISPLAY 경고만으로
compute 실패를 단정하지 마세요. 셰이더는 빌드 시 SPIR-V로 컴파일해 실행 파일에 포함하므로
실행 시 별도 셰이더 경로나 Python은 필요하지 않습니다.

### GPU로 옮긴 범위

- 축소 DIS 흐름을 원본 크기로 확대하고 픽셀별 좌표 생성
- 흐름 유효성·최대 이동 검사와 원본 영상 리매핑
- 검사 프레임에서 역방향 흐름 확대·왕복 일관성 검사

Vulkan 1.0 storage buffer와 8×8 workgroup을 사용합니다.
영상 입력·DIS/ORB·전역 영상 변환·CLAHE·RGB 합성·JPEG는 여전히 CPU입니다.
이 단계는 실시간 경로만 지원하며 기존 후처리 경로는 CPU로 유지합니다.
CPU와 같은 1/32 보간 좌표 양자화, border 처리, 흐름 제한과 검사 임계값을 사용합니다.
드라이버별 부동소수점 차이는 있을 수 있으므로 Pi에서 `vulkan_test`를 실행하세요.

입력·출력 버퍼와 pipeline은 프레임마다 재생성하지 않습니다.
Host-visible 메모리에 입력을 패킹하고 명령을 제출한 뒤 fence로 완료를 기다려 결과를 읽습니다.
메모리 종류에 따라 flush/invalidate를 수행하고 host/compute barrier를 사용합니다.
현재 한 프레임씩 동기 처리하며 GPU/CPU 이중 버퍼 비동기 파이프라인은 아직 없습니다.

### CPU와 비교 측정

동일 장면·계산 폭 320·목표 30fps·스레드 2개·원본 녹화 끄기로 CPU와 Vulkan을 각각 60초 측정합니다.
이전 JSON은 다음 실행 전에 다운로드하세요. 계측 JSON의 `config.local_backend.actual`이
`vulkan`이고 `device.software`가 false일 때만 실제 GPU 결과입니다.
자동 선택이 CPU로 돌아간 결과를 GPU 성능으로 해석하지 마세요.

`local_warp_ms`는 GPU 경로에서 패킹·버퍼 갱신·제출/대기·결과 읽기와 검사 커널 전체를 포함합니다.
`consistency_ms`는 GPU 검사도 이 항목에 합쳐지므로 0일 수 있습니다.
추가 `gpu_pack_ms`, `gpu_upload_ms`, `gpu_submit_wait_ms`, `gpu_download_ms`는 하위 계측이며
상위 `local_warp_ms`에 다시 더하지 않습니다.
`gpu_kernel_ms`는 지원 장치의 timestamp query 결과이며 제출/대기 시간과도 겹칩니다.
미지원 시 0과 `timestamp_supported: false`가 표시됩니다.
GPU 커널만 빨라져도 데이터 이동·동기화 때문에 전체 처리량이 좋아지지 않을 수 있습니다.

저장 영상으로 동일 입력 비교도 할 수 있습니다.

```bash
./cpp/build/imaging_cpp --benchmark-session captures/세션이름 --backend cpu --report benchmarks/cpu.json --cv-threads 2
./cpp/build/imaging_cpp --benchmark-session captures/세션이름 --backend vulkan --report benchmarks/vulkan.json --cv-threads 2
```

명시한 Vulkan 벤치마크가 CPU로 전환되면 실제 백엔드를 기록한 보고서를 남기고 오류로 종료합니다.
CPU 대 Python 비교 도구는 `--cpp-backend vulkan`도 지원합니다.

### 검증 범위

클라우드에는 실제 GPU가 없어 Mesa llvmpipe 소프트웨어 Vulkan으로 실행·일치도를 검사했습니다.
320/480/640, 검사 켜기/끄기, 경계·큰 흐름·비정상 값, Vulkan validation layer,
웹 MJPEG·계측·CPU 전환을 검증했습니다. 제공된 193프레임에서도 CPU와 Vulkan 소프트웨어
출력을 비교했습니다. 이는 Pi GPU의 성능이나 하드웨어 실행 검증을 뜻하지 않습니다.
Pi 하드웨어 처리 속도는 별도 측정이 필요하며 30fps를 보장하지 않습니다.

개발 검증에서만 소프트웨어 Vulkan을 허용하려면 명시적으로 다음을 사용합니다.

```bash
IMAGING_TEST_ALLOW_SOFTWARE_VULKAN=1 ./cpp/build/vulkan_test
python3 cpp/test_vulkan_http.py --software
python3 cpp/test_vulkan_http.py --fallback
```

HTTP 소프트웨어 검사기는 서버를 `--allow-software-vulkan`으로 실행합니다.
이 옵션은 실제 Pi 성능 측정에서 사용하지 마세요.
CPU만 빌드하려면 `-DIMAGING_ENABLE_VULKAN=OFF`를 사용합니다. Vulkan 의존성이 없는 경우에도
CPU stub과 명시적 fallback 상태로 빌드되며, `vulkan_test`는 생성되지 않습니다.

## CPU 처리 최적화

CPU 경로는 흐름 크기 보정·좌표 생성·유효성 판정을 한 번의 순회로 처리하고,
일관성 검사 결과도 해당 순회에서 카메라별 공통 마스크에 결합합니다.
축소 영상·CLAHE 출력·흐름 확대·좌표·검사 작업 버퍼를 재사용합니다.
반환 영상과 유효성 마스크는 별도 저장 공간을 유지합니다.
변환과 이동이 정확히 없는 640×480 영상은 원근 보간 대신 복사합니다.

정합 알고리즘, DIS 설정, 계산 폭, 5프레임 검사 간격 및 유효성 임계값은 유지합니다.
DIS 출력 버퍼는 매번 비워진 상태로 전달합니다. 이를 재사용하면 OpenCV가 이전 흐름을
초깃값으로 해석하여 정합 결과가 달라지기 때문입니다.
전역·국소 변환 통합, 저해상도 일관성 검사, 전역 정합 비동기화는 이번 변경에 포함하지 않습니다.

`cpu_parity_test`는 기존 계산식과 320/480/640 출력·유효성 마스크를 정확히 비교하고,
검사 간격·빈 영상·이전 반환 영상 보존·항등/비항등 변환을 검사합니다.
`results/cpu_optimization_cloud.json`은 동일 영상으로 수정 전/후를 번갈아 3회 실행한
클라우드 x86_64 결과입니다. 평균 전체 처리 시간은 계산 폭 320에서 약 8.9%,
480에서 약 11.2%, 640에서 약 2.9% 감소했습니다. 640은 실행 간 변동이 커
개선 폭을 확정하기 어렵습니다. 세 폭 모두 193프레임 비교 영상의 픽셀 차이는 0이었습니다.
라즈베리파이 성능으로 해석하지 마세요.

라즈베리파이에서 업데이트 후 CPU 모드로 같은 조건을 측정하세요.

```bash
git pull origin main
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j 2
ctest --test-dir cpp/build --output-on-failure
./cpp/build/imaging_cpp --backend cpu --cv-threads 2
```

계산 폭 320·목표 30fps·녹화 끔으로 1분 이상 실행 후 중지하고 프로필을 다운로드합니다.
촬영 장면·전원·온도·브라우저 미리보기 조건도 가능한 한 이전 측정과 맞추세요.

## 국소 리매핑 세부 측정과 좌표 맵 비교

실시간 CPU 모드에서 **CPU 좌표 맵**을 선택할 수 있습니다.
기본값 `기존 방식`은 부동소수점 좌표를 `remap`에 직접 전달합니다.
`고정소수점`은 `convertMaps`로 CV_16SC2/CV_16UC1 맵을 만들고, 같은 맵을
영상 보간과 검사 프레임의 역방향 흐름 보간에 재사용합니다.
설정 변경은 실시간 합성을 중지하고 다시 시작해야 반영됩니다.
Vulkan에는 이 선택이 적용되지 않으며, GPU 실패로 CPU로 전환하면 적용됩니다.
오프라인 합성은 기존 방식을 유지합니다.

프로필 JSON과 웹 화면에 다음 하위 계측을 추가했습니다. cam2·cam3의 합산 시간입니다.

| JSON 항목 | 처리 내용 | 포함되는 상위 시간 |
|---|---|---|
| `cpu_flow_resize_ms` | 흐름을 출력 크기로 확대 | `local_warp_ms` |
| `cpu_coords_ms` | 흐름 크기 보정, 좌표 생성, 유효성 판정 | `local_warp_ms` |
| `cpu_map_convert_ms` | 고정소수점 좌표 맵 생성 | `local_warp_ms` |
| `cpu_image_remap_ms` | 영상 보간 | `local_warp_ms` |
| `cpu_restore_ms` | 유효하지 않은 픽셀의 원본 복원 | `local_warp_ms` |
| `cpu_reverse_remap_ms` | 역방향 흐름 보간 (검사 시) | `consistency_ms` |

하위 시간은 상위 시간에 이미 포함되므로 합산하지 않습니다. 역방향 보간의 전체 평균은
검사하지 않은 프레임의 0도 포함하며, `active_mean_ms`는 검사한 프레임만의 평균입니다.
계측 호출과 분기 등 소량의 비용 때문에 하위 시간의 합과 상위 시간이 정확히 같지는 않습니다.

클라우드의 동일 영상 193프레임, 교대 순서 각 3회 결과는
`results/cpu_remap_cloud.json`에 있습니다. 폭 320에서 영상 보간은 0.739→0.667ms였지만
맵 생성에 0.252ms가 추가되어 전체 처리는 약 2.7% 느려졌습니다.
폭 480/640에서도 이번 측정에서는 전체 속도가 개선되지 않았습니다.
세 폭 모두 두 방식의 비교 영상 픽셀 차이는 0이었고, 별도 테스트로 압축 전 출력과
유효성 마스크도 기존 계산식과 정확히 일치함을 확인했습니다.
이는 실제 정합 정확도 검증이나 라즈베리파이 성능 측정은 아닙니다.
Pi에서 이득을 확인하기 전까지 기본값은 기존 방식으로 유지합니다.

같은 원본 녹화 세션으로 라즈베리파이에서 비교하려면 다음 명령을 실행하세요.
Python OpenCV·NumPy가 필요하며, 프로그램 서버 자체는 계속 C++로 실행됩니다.

```bash
python cpp/compare_cpu_remap.py captures_cpp/촬영세션이름 --widths 320 --repeat 3 --threads 2
```

결과는 `benchmarks/cpu-remap-날짜_시각/comparison.json`과 방식별 프로필·AVI에 저장됩니다.
기존 수동 설정을 사용하려면 `--settings 설정.json`을 추가합니다.
실시간 비교는 CPU·계산 폭 320·목표 30fps·녹화 끔 조건으로 각 좌표 맵을 1분 이상
실행하고 중지 후 프로필 JSON을 각각 다운로드하세요.

## CPU 좌표 생성 SIMD 최적화

국소 리매핑의 `cpu_coords_ms`에 해당하는 흐름 크기 보정·좌표 생성·유효성 판정을
128비트 SIMD로 처리합니다. aarch64 라즈베리파이에서는 ARM NEON 명령을 직접 사용하고,
x86_64 클라우드에서는 OpenCV SSE2를 사용합니다. SIMD를 지원하지 않는 빌드는 스칼라 루프로 처리합니다.
실시간 합성 상태와 프로필의 `config.local_backend.cpu_coordinate_kernel`에서
사용하는 커널을 확인할 수 있습니다.

검사하지 않는 프레임에서는 좌표 생성 후 쓰지 않는 확대 흐름을 메모리에 다시 저장하지 않습니다.
검사 프레임과 오프라인 합성에서는 역방향 일관성 계산에 필요하므로 기존과 동일하게 저장합니다.
크기가 60픽셀 미만인 유한 흐름만 허용하는 기존 판정은 유지합니다.
NaN/무한대는 크기 비교에서 제외되며, 유효 흐름에 출력 좌표를 더해도 유한하므로
중복 유한성 검사를 생략합니다. 별도의 고정소수점 맵 생성은 추가하지 않았습니다.
기본 좌표 맵은 계속 `기존 방식(float)`입니다.

클라우드 동일 영상 각 3회 비교는 `results/cpu_coordinates_cloud.json`에 저장했습니다.
폭 320에서 좌표 생성은 약 74%, 국소 변환 전체는 약 35%, 전체 처리는 약 11% 단축됐습니다.
폭 320/480/640 모두 193프레임 비교 영상의 픽셀 차이는 0이었습니다.
이는 클라우드 SSE2 결과이며 라즈베리파이 NEON 성능은 별도 측정해야 합니다.
`cpu_coordinates` 테스트는 스칼라·SIMD, 남는 픽셀, NaN/무한대,
60픽셀 경계와 검사 시 흐름 보존을 확인합니다. `cpu_parity`는 기존 계산식과
세 폭의 압축 전 출력·유효성 마스크를 비교합니다.

업데이트 후 `ctest --test-dir cpp/build --output-on-failure`로 Pi의 결과 일치를 확인하고,
CPU·기존 방식·폭 320·목표 30fps·녹화 끔으로 1분 이상 측정하세요.
역방향 흐름 보간 및 일관성 검사 방식은 이번 변경에서 유지합니다.


ARM OpenCV 헤더에 따라 `v_float32x4`의 `*`, `+`, 비교 연산자가 제공되지 않는
경우가 있어, NEON 경로는 `arm_neon.h`의 명시적 연산으로 구현했습니다.
이 경로는 aarch64 GCC 14로 파일 전체의 컴파일을 확인했습니다. 실제 Pi 실행·성능은
업데이트 후 네이티브 빌드와 CTest로 확인해야 합니다.
개발 환경에 크로스 컴파일러와 OpenCV 헤더가 있으면 다음 검사도 가능합니다.

```bash
aarch64-linux-gnu-g++ -std=c++17 -O3 -ffp-contract=off -Wall -Wextra -Wpedantic \
  -I/usr/include/opencv4 -c cpp/cpu_coords.cpp -o /tmp/cpu_coords_aarch64.o
```

## 카메라별 밝기가 달라졌을 때

카메라 스트림을 연 뒤 첫 프레임으로 초기화를 완료하고, 수동 노출 4500을 적용합니다.
기존의 카메라를 열기 전 설정은 장치 초기화 중 바뀔 가능성이 있어 순서를 변경했습니다.
화면에는 시작 시 읽은 실제 노출값과 자동 노출 모드(1은 수동)를 표시합니다.
읽기 실패나 요청값과의 불일치는 경고로 표시합니다.

상단 **카메라 실제 설정 JSON 다운로드**는 `/api/cameras/settings`에서
세 장치의 현재 노출·밝기·대비·감마·게인·화이트밸런스와 실제 장치 경로를 읽습니다.
지원되지 않는 항목은 null입니다. 다운로드 시 설정을 변경하지 않습니다.
원본 미리보기부터 밝기가 다른지, 원본은 비슷한데 합성 결과만 색이 치우치는지도
함께 확인하세요. 후자는 RGB 배치 및 정합 실패를 구분해야 합니다.

밝기·게인·화이트밸런스는 이번 변경에서 임의로 통일하거나 영상별로 정규화하지 않습니다.
노출 숫자가 같아도 장치 설정·광학계·장면 차이로 밝기가 같다고 보장되지는 않습니다.
밝기가 다른 원본이 국소 광류 추정에 영향을 줄 수 있으나, 화면 울렁임의 원인으로
확정하려면 원본과 설정 비교가 필요합니다. 이 수정의 실제 장치 동작은 Pi에서 확인해야 합니다.

## 세 카메라 공통 밝기 조절

상단 **세 카메라 공통 밝기**에서 0~255 값을 입력하고 **밝기 적용**을 누릅니다.
현재 장치에서 이전에 확인한 기본값은 120이며, 140부터 확인하고 필요하면 160으로
올려 보세요. 입력값을 바꾸기만 해서는 적용되지 않습니다.
하드웨어 `V4L2_CID_BRIGHTNESS`를 조절하므로 이후 원본 미리보기·녹화와 합성에 반영됩니다.
노출 4500, 자동 노출 모드, 게인과 화이트밸런스는 변경하지 않습니다.
밝기 수치가 같아도 장치별 실제 밝기가 같다고 보장되지는 않습니다.
소프트웨어로 센서 감도나 어두운 곳의 세부 정보를 복원하는 기능은 아닙니다.

서버는 세 장치의 지원 범위·스텝과 현재값을 먼저 읽고, 변경 후 실제 값을 확인합니다.
일부 장치에서 실패하면 변경한 장치의 이전 값으로 복원을 시도하고 실패 여부를 알립니다.
데모에서는 하드웨어 조절을 지원하지 않으며 녹화 중에는 변경할 수 없습니다.
적용값을 별도 설정 파일에 저장하지 않으므로 장치 재연결·드라이버 초기화로 값이 바뀌면
버튼으로 다시 적용하세요. 지나치게 밝아 윤곽이 하얗게 사라지면 값을 낮추세요.
이 기능의 x86/ARM 컴파일과 웹 입력 검증은 확인했으며, 실제 USB 장치의 지원과 결과는
라즈베리파이에서 확인해야 합니다.
