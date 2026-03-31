# [프로젝트] 스마트 화재 감지 및 능동형 대피 유도 시스템 시제품 개발기

안녕하세요. 이번 포스트에서는 화재 발생 시 이를 신속하게 감지하고, 안전한 대피 방향을 직관적인 LED 애니메이션으로 유도하는 **스마트 화재 감지기 시제품(Prototype)** 개발 과정을 공유하고자 합니다. 

단순히 경보만 울리는 기존 화재감지기의 한계를 넘어, 다중 노드 간의 무선 통신을 통해 화재 발생 위치를 특정하고 대피 방향을 능동적으로 계산하여 시각적으로 안내하는 시스템입니다.

![화재감지기 컨셉](https://post.jigumi.com/post/fire_detect_led_animation/img1.png)

---

## 1. 시스템 아키텍처 및 하드웨어 구성

본 시스템은 공간을 좌측(Left), 중앙(Center), 우측(Right) 3개의 구역으로 나누어 각각 독립적인 센서 노드를 배치하는 분산형 아키텍처를 채택했습니다. 각 노드는 화재를 감지하고, HC-12 RF 모듈을 통해 상태를 공유합니다.

### 1.1. 하드웨어 스펙
각 구역별 하드웨어 구성은 다음과 같습니다. MCU는 범용성과 개발 편의성을 고려해 **Arduino Nano**를 사용했습니다.

**좌/우측 노드 (Side Node)**
* **MCU:** Arduino Nano
* **센서부:** 가스 센서(MQ-2) 1개, 불꽃 감지 센서(적외선) 1개
* **통신부:** HC-12 (433MHz RF 트랜시버)
* **출력부:** 
  * 방향 지시용 화살표 NeoPixel (2px) 1개
  * 대피 유도용 줄(Strip) NeoPixel (30px) 1개 (논리적으로 3개 구간으로 분할 제어)
  * Active Buzzer (화재 트리거 시 `삐빕-삐빕-삐빕` 패턴 출력)

**중앙 노드 (Center Node)**
* **MCU:** Arduino Nano
* **센서부:** 가스 센서(MQ-2) 1개, 불꽃 감지 센서(적외선) 1개
* **통신부:** HC-12
* **출력부:** 방향 지시용 화살표 NeoPixel (2px) 2개

모든 노드는 HC-12를 통해 무선으로 통신하므로, 복잡한 배선 없이 유연한 설치가 가능합니다.

### 1.2. 회로 연결도
하드웨어 결선은 아래 이미지를 참고해 주시기 바랍니다.

![회로 연결도 1](https://post.jigumi.com/post/fire_detect_led_animation/img2.png)
![회로 연결도 2](https://post.jigumi.com/post/fire_detect_led_animation/img3.png)
![회로 연결도 3](https://post.jigumi.com/post/fire_detect_led_animation/img4.png)
![회로 연결도 4](https://post.jigumi.com/post/fire_detect_led_animation/img5.png)
![회로 연결도 5](https://post.jigumi.com/post/fire_detect_led_animation/img6.png)
![회로 연결도 6](https://post.jigumi.com/post/fire_detect_led_animation/img7.png)

---

## 2. 핵심 로직 및 대피 유도 알고리즘

시스템의 핵심은 **"화재 발생 위치를 제외한 안전한 방향으로 사람들을 유도하는 것"**입니다. 이를 위해 다음과 같은 상태 머신(State Machine) 기반의 시나리오를 구현했습니다.

### 시나리오 A: 좌측 구역 화재 감지 시
1. **감지 및 전파:** 좌측 노드의 가스 또는 불꽃 센서가 임계치(Threshold)를 초과하면 화재 상태(`FIRE_LEFT`)로 진입합니다. 이때 다른 노드에서의 중복 감지를 차단(Block)하여 시스템 혼선을 방지합니다.
2. **상태 동기화:** HC-12를 통해 화재 발생 사실을 전파합니다. (통신 신뢰성을 위해 ACK 응답 필수 확인)
3. **시각적 유도 (NeoPixel 제어):**
   * **RED (위험 구역):** 좌측 노드의 화살표, 중앙 노드의 좌측 화살표, 우측 노드의 좌측 화살표 및 줄 NeoPixel의 좌측 10px 구간.
   * **GREEN (안전/대피 구역):** 나머지 화살표. 줄 NeoPixel의 나머지 구간은 우측(정방향)으로 흐르는 애니메이션을 출력하여 대피 방향을 직관적으로 지시합니다.
4. **상태 복귀:** 화재 원인이 제거되어 센서 값이 정상화되면 `NORMAL` 상태로 복귀합니다.

### 시나리오 B: 중앙 구역 화재 감지 시
1. **감지 및 전파:** 중앙 노드 센서 트리거 시 `FIRE_CENTER` 상태로 진입 및 전파.
2. **시각적 유도:**
   * **RED:** 중앙 화살표들, 줄 NeoPixel의 중앙 10px 구간.
   * **GREEN:** 양 끝단 화살표. 줄 NeoPixel은 중앙에서 양방향(좌측은 역방향, 우측은 정방향)으로 퍼져나가는 애니메이션을 출력하여 중앙을 피해 양쪽으로 대피하도록 유도합니다.

*(우측 구역 화재 시에는 좌측 로직과 정확히 반대로 대피 방향이 설정됩니다.)*

---

## 3. 임베디드 소프트웨어 딥다이브 (Deep Dive)

단순히 센서 값을 읽어 LED를 켜는 것을 넘어, 현업 임베디드 시스템에서 요구되는 **신뢰성(Reliability)**과 **오동작 방지(Robustness)**를 위해 몇 가지 주요 기법을 코드에 적용했습니다.

### 3.1. Peak Tracking 및 동적 해제 알고리즘 (가스 센서)
가스 센서(MQ-2)는 아날로그 값이 출렁이거나(Noise), 잔류 가스로 인해 서서히 값이 떨어지는 특성이 있습니다. 이를 보정하기 위해 **Peak Tracking(최고점 추적)** 기법을 적용했습니다.

```cpp
// Peak 추적 갱신 + 해제 판정 로직 중 일부
if (cur > gasCPeak) {
  gasCPeak = cur;
  gasCDeclineCnt = 0;
  gasCPeakLocked = false;
} else if (cur < gasCPrev) {
  if (gasCDeclineCnt < 1000) gasCDeclineCnt++;
  if (gasCDeclineCnt >= DECLINE_CONFIRM) {
    gasCPeakLocked = true; // 감소 전환 확정
  }
}
```
단순히 특정 임계값 아래로 내려갔다고 화재 상황을 해제하는 것이 아니라, 가스 농도의 최고점(`gasCPeak`)을 추적하고 연속적인 하락 추세(`DECLINE_CONFIRM`)가 확인되었을 때, 최고점 대비 일정 수치(`PEAK_DROP`) 이상 떨어져야만 해제 후보로 판정합니다. 이를 통해 센서 노이즈로 인한 잦은 상태 변경(Chattering)을 완벽히 차단했습니다.

### 3.2. 상태 복귀 시 Rearm(재무장) 메커니즘
화재 상황이 종료되어 `NORMAL` 상태로 복귀하더라도, 잔류 가스가 여전히 화재 감지 임계값(`GAS_THRESHOLD`) 근처에 머물러 있다면 즉시 다시 화재로 오인식될 수 있습니다. 

```cpp
// Rearm 처리: 해제 후 가스가 충분히 낮아져야 재감지 허용
if (needRearmC && analogRead(GAS_C_PIN) < REARM_CLEAR_THR) {
  needRearmC = false;
  Serial.println("CENTER rearm OK");
}
```
이를 방지하기 위해 `needRearm` 플래그를 도입하여, 가스 농도가 완전히 안전한 수준(`REARM_CLEAR_THR`)까지 떨어질 때까지는 새로운 감지를 보류하도록 설계했습니다.

### 3.3. 신뢰성 있는 무선 통신 (ACK & Retry)
HC-12를 이용한 단방향 브로드캐스트는 패킷 유실 위험이 있습니다. 노드 간 상태가 불일치하면 대피 방향이 꼬일 수 있으므로, 소프트웨어적으로 ACK(응답) 기반의 재전송 로직을 구현했습니다.

```cpp
bool sendWithAck(const String& msg) {
  for (int i = 0; i < ACK_RETRY; i++) {
    hc12.println(msg);
    uint32_t t = millis();
    while (millis() - t < ACK_TIMEOUT_MS) {
      if (hc12.available()) {
        String r = hc12.readStringUntil('\n');
        r.trim();
        if (r == "ACK") return true;
      }
      // 블로킹 대기 중에도 LED 애니메이션은 유지되도록 처리
      if (millis() - dispMs >= DISP_INTERVAL) {
        dispMs = millis();
        applyDisplay();
      }
    }
  }
  return false;
}
```
특히 `delay()`를 사용하지 않고 `millis()` 기반의 Non-blocking 방식으로 대기 루프를 구현하여, 통신 응답을 기다리는 동안에도 LED 애니메이션과 부저 출력이 끊기지 않도록 처리한 점이 포인트입니다.

---
## 4. 작동 영상
[영상 보기](https://post.jigumi.com/post/fire_detect_led_animation/vod1.mp4)

---

## 5. 마무리

이번 시제품은 아두이노 나노라는 제한된 자원 환경에서도 **Peak Tracking, Non-blocking Task Scheduling, ACK 기반 RF 통신** 등 실무적인 임베디드 제어 기법을 적용하여 높은 신뢰성을 확보했다는 데 의의가 있습니다. 

향후에는 ESP32 기반으로 업그레이드하여 Mesh Network를 구성하고, 중앙 관제 서버(Web/App)로 실시간 화재 데이터를 전송하는 IoT 시스템으로 발전시켜 볼 계획입니다. 

코드 전문과 상세한 구현 내역은 첨부된 파일(`mid_v3.cpp`, `side_v3.cpp`)을 통해 확인하실 수 있습니다. 피드백이나 질문은 언제든 환영합니다!
