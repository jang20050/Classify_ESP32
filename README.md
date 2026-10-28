# Classify_ESP32

**XIAO ESP32-S3 Sense + Edge Impulse 이미지 분류**

ESP32-S3 Sense 카메라로 사진을 모으고, Edge Impulse에서 분류 모델을 학습했습니다.
학습한 모델은 **ESP32 보드 안에서 직접(온디바이스)** 실행되며, 무엇을 비추고 있고 어떻게 분류하는지 웹 대시보드에서 실시간으로 볼 수 있습니다.

- 보드: Seeed Studio XIAO ESP32-S3 Sense (OV3660 카메라, 8MB PSRAM)
- 분류 클래스: `eyedrop`(안약), `GlueStick`(딱풀)
- 결과: int8 모델 test 정확도 100% (46/46), 보드에서 약 5회/초 분류 (모델 추론 약 86ms)

---

## 1. Edge Impulse란?

[Edge Impulse](https://edgeimpulse.com)는 **마이크로컨트롤러 같은 작은 기기에서 돌아가는 머신러닝 모델**을 만들어 주는 플랫폼입니다.
데이터 수집부터 학습, 평가, 기기용 코드 생성까지 한곳에서 처리합니다.

| 단계 | Edge Impulse가 하는 일 | 이 프로젝트에서 |
|---|---|---|
| Data acquisition | 학습용 데이터를 라벨과 함께 저장 | 수집 대시보드에서 API로 업로드 (training/testing 자동 분할) |
| Impulse design | 입력 → 전처리(Processing block) → 학습(Learning block) 구성 | 96x96 이미지 → RGB → MobileNetV1 전이학습 |
| Training | 클라우드에서 학습, float32와 int8(양자화) 모델 생성 | 30회 학습, 학습 중 사진을 회전·밝기 변경 등으로 변형 |
| Model testing | 학습에 쓰지 않은 test 데이터로 평가 | 46장 모두 맞힘 |
| Deployment | 기기용 C++ / **Arduino 라이브러리** 생성 | `ei_model/ei-arduino-library.zip` |

생성된 라이브러리는 인터넷 없이 보드 안에서만 실행됩니다.
그래서 지연이 짧고, 영상을 외부로 보내지 않아 개인정보 걱정도 적습니다.

### 이 프로젝트의 사용 방식: 이미지 분류 (Classification)
카메라 화면 가운데 정사각형 영역을 96x96으로 줄여서 모델에 넣으면, 각 클래스일 확률이 나옵니다.

```
카메라(QVGA 320x240 JPEG) → RGB 변환 → 가운데 240x240 잘라내기 → 96x96 축소
   → MobileNetV1 0.25 (int8) → { GlueStick: 0.95, eyedrop: 0.05 }
```

> **분류와 객체 감지(Object Detection)의 차이**
> 분류는 "화면 전체가 무엇인지"를 하나의 라벨로 답합니다. 물체의 위치나 개수는 알 수 없습니다.
> 위치까지 알고 싶다면 Edge Impulse의 **FOMO**(ESP32에서도 돌아가는 가벼운 객체 감지 모델)를 쓰면 됩니다. 대신 사진마다 물체 위치를 박스로 표시하는 작업이 필요합니다.

---

## 2. 구조

```
firmware/
  xiao_cam_server/     데이터 수집용 펌웨어 (사진 캡처, 실시간 영상, 카메라 설정)
  xiao_inference/      온디바이스 추론 펌웨어 + 웹 대시보드(dashboard.h)
dashboard/             PC용 수집 대시보드 (Flask)
train_ei.py            Edge Impulse API로 학습하고 Arduino 라이브러리 다운로드
split_dataset.py       data/ → dataset/train, dataset/test 로 80/20 분할 (로컬 학습용)
ei_model/              학습된 모델 (Edge Impulse Arduino 라이브러리 zip)
data/<클래스>/         수집한 이미지 (저장소에는 포함하지 않음)
```

---

## 3. 실행 방법과 주소

### 준비
```bash
pip install -r dashboard/requirements.txt
cp .env.example .env                                   # EI_API_KEY 입력 (Edge Impulse > Dashboard > Keys)
cp firmware/xiao_cam_server/secrets.h.example firmware/xiao_cam_server/secrets.h   # Wi-Fi 입력
cp firmware/xiao_inference/secrets.h.example  firmware/xiao_inference/secrets.h
```
- ESP32-S3는 **2.4GHz Wi-Fi만** 지원합니다. 휴대폰 핫스팟을 쓸 때는 2.4GHz로 설정하세요.
- PC와 ESP32는 **같은 네트워크**에 있어야 합니다. 학교 Wi-Fi는 기기끼리 통신을 막는 경우가 많습니다.
- Arduino 보드 설정: **XIAO_ESP32S3**, **Tools > PSRAM > OPI PSRAM** (필수)

### ① 데이터 수집
```bash
arduino-cli compile --upload -p COM6 --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi firmware/xiao_cam_server
python dashboard/app.py
```

| 주소 | 내용 |
|---|---|
| **http://localhost:5000** | 수집 대시보드 (오른쪽 위에 ESP32 IP 입력) |
| `http://<ESP32 IP>/capture` | 사진 한 장 (JPEG) |
| `http://<ESP32 IP>/status` | 카메라 설정 (JSON) |
| `http://<ESP32 IP>/control?var=<이름>&val=<값>` | 카메라 설정 변경 |
| `http://<ESP32 IP>:81/stream` | MJPEG 실시간 영상 |

ESP32 IP는 시리얼 모니터(115200)에 `Ready! http://...`로 출력됩니다. `http://esp32cam.local`로도 접속할 수 있습니다.

| 수집 대시보드 기능 | 조작 |
|---|---|
| 클래스 추가/선택 | 입력창 + 추가, 클래스 칩 클릭 또는 숫자키 `1`–`9` |
| 촬영 / 연속 촬영 | `Space` / `B` (장수·간격 설정, 다시 누르면 중지) |
| 마지막 촬영 취소 | `Z` |
| 선택 삭제 | 클릭, Shift+클릭 범위 선택, `Delete` |
| 크게 보기 | 더블클릭 → `←` `→` 이동, `Del` 삭제, `Esc` 닫기 |
| 다운로드 | 클래스 ZIP / 전체 ZIP |
| Edge Impulse 업로드 | 자동 분할 / training / testing 선택 (이미 올린 파일은 건너뜀) |

### ② 학습
```bash
python train_ei.py                # impulse 구성 → 특징 생성 → 학습 → ei_model/ei-arduino-library.zip
python train_ei.py --no-build     # 라이브러리 빌드 없이 정확도만 확인
python train_ei.py --cycles 50 --dropout 0.2
```

### ③ 온디바이스 추론 + 대시보드
```bash
arduino-cli lib install --zip-path ei_model/ei-arduino-library.zip   # 또는 Arduino IDE > Add .ZIP Library
arduino-cli compile --upload -p COM6 --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi firmware/xiao_inference
```
(`--zip-path`를 쓰려면 먼저 `arduino-cli config set library.enable_unsafe_install true` 를 실행해야 합니다. 첫 컴파일은 약 7분 걸립니다.)

| 주소 | 내용 |
|---|---|
| **`http://<ESP32 IP>/`** | 추론 대시보드 |
| `http://<ESP32 IP>/result` | 최신 예측 JSON (`labels`, `scores`, `top`, `fps`, `dsp_ms`, `nn_ms` …) |
| `http://<ESP32 IP>/input.bmp` | 모델이 실제로 보는 96x96 입력 이미지 |
| `http://<ESP32 IP>/control?var=threshold&val=0.7` | 확신 임계값, `vflip`, `hmirror`, `brightness` 변경 |
| `http://<ESP32 IP>:81/stream` | MJPEG 실시간 영상 |

추론 대시보드에서 볼 수 있는 것:
- 실시간 영상 위에 **모델이 보는 영역**(가운데 정사각형)이 표시되고, 테두리 색이 현재 예측 클래스를 나타냅니다.
- 현재 예측 클래스와 확률, 클래스별 확률 막대가 나옵니다. 임계값보다 낮으면 "확신 낮음"으로 표시합니다.
- 모델 입력 이미지(96x96), 최근 60번의 예측 흐름, 클래스가 바뀐 기록이 나옵니다.
- 추론 속도(fps), 전처리·모델 시간, Wi-Fi 신호 세기가 나옵니다.

---

## 4. 실습 진행

| # | 단계 | 내용 / 결과 |
|---|---|---|
| 1 | 수집 펌웨어 | ESP32에 사진 캡처, 실시간 영상, 설정 변경 서버를 구현했습니다. 영상 서버(포트 81)를 따로 띄워 영상을 보면서도 촬영할 수 있습니다. |
| 2 | 수집 대시보드 | Flask로 클래스 관리, 연속 촬영, 삭제, ZIP 다운로드, Edge Impulse 업로드 기능을 만들었습니다. |
| 3 | 데이터 수집 | `eyedrop` 120장, `GlueStick` 103장 (640x480). 모두 정상 파일이고 중복이나 노출 문제는 없었습니다. |
| 4 | 데이터 분할 | `split_dataset.py`로 train 178장, test 45장 (80/20). Edge Impulse에는 training 177장, testing 46장으로 올렸습니다. |
| 5 | 학습 1차 | MobileNetV2 0.35: float32 100%, **int8 72%**. 양자화 후 GlueStick이 eyedrop으로 잘못 분류됐습니다. |
| 6 | 학습 2차 | MobileNetV2 + dropout 0.25, 32 뉴런: int8 78%. 개선이 부족했습니다. |
| 7 | 학습 3차 (최종) | **MobileNetV1 0.25**: float32와 int8 모두 100%, test 46장 모두 맞힘. |
| 8 | 배포 문제 해결 | EON 컴파일러로 만든 라이브러리는 ESP32-S3에서 `Failed to allocate persistent buffer` 오류로 계속 재부팅됐습니다. 원인은 ESP-NN이 쓰는 버퍼가 EON이 계산한 메모리 영역을 넘는 것입니다. **TFLite 엔진**으로 다시 빌드해서 해결했습니다. |
| 9 | 온디바이스 추론 | 전처리 5ms + 모델 86ms, **약 5회/초**. 웹 대시보드로 실시간 확인했습니다. |

### 확인된 한계
- **background 클래스가 없습니다.** 클래스가 두 개뿐이라 물체가 없는 화면(천장, 사람 얼굴 등)도 `GlueStick 92%`처럼 둘 중 하나로 확신 있게 분류합니다.
- **test 점수가 실제보다 높게 나왔을 수 있습니다.** 연속 촬영한 비슷한 사진이 train과 test에 나뉘어 들어갔기 때문입니다.

---

## 5. 더 잘 구분하려면

1. **background(없음) 클래스 추가 (가장 효과가 큼)**
   빈 책상, 손만 있는 화면, 벽, 천장, 다른 물건 등 실제로 카메라에 비칠 장면을 100장 이상 찍으세요. 물체가 없을 때 엉뚱한 클래스로 답하는 문제가 사라집니다.
2. **장수보다 다양성**
   같은 각도로 연속 촬영을 반복하는 것보다 각도, 거리, 배경, 조명을 바꿔 가며 찍는 게 훨씬 효과적입니다.
   클래스당 150~200장, 클래스 간 장수는 비슷하게 맞추세요. 모델은 가운데 정사각형만 보니 물체를 화면 가운데에 두세요.
3. **test 사진은 따로 찍기**
   다른 날이나 다른 장소에서 클래스당 20~30장을 찍어 **testing**으로 올리세요. 그래야 실제 정확도를 알 수 있습니다.
4. **실제로 쓸 환경 그대로 찍기**
   추론할 때와 같은 카메라 설정(반전, 밝기)과 비슷한 조명에서 수집하세요.
5. **확신 임계값 활용**
   대시보드의 임계값(기본 60%)을 높이면 애매한 상황에서 "확신 낮음"으로 표시되어 잘못된 답을 줄일 수 있습니다.
6. **모델 조정**
   데이터가 늘면 `train_ei.py --cycles`, `--dropout`, 입력 크기 `--size 128` 등을 조절해 보세요.
   ESP32에서 쓰는 int8 모델은 양자화에 강한 MobileNetV1 계열이 유리했습니다. 바꿀 때는 꼭 **int8 정확도**를 확인하세요.
7. **위치까지 필요하면 FOMO**
   여러 물체를 동시에 찾거나 위치가 필요하면, Edge Impulse의 객체 감지(FOMO)로 바꾸는 것을 고려하세요.

---

## 참고
- [XIAO ESP32-S3 Sense Wiki](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- [Edge Impulse Docs](https://docs.edgeimpulse.com/) · [Ingestion API](https://docs.edgeimpulse.com/reference/ingestion-api) · [Studio API](https://docs.edgeimpulse.com/apis/studio)
