// =====================================================
// mid_nano.ino  ─  중앙 아두이노 나노
// =====================================================

#include <Adafruit_NeoPixel.h>
#include <SoftwareSerial.h>

// ── 핀 정의 ───────────────────────────────────────────
#define HC12_RX_PIN   2
#define HC12_TX_PIN   3
#define PIN_ARR_C     4
#define FLAME_C_PIN   7
#define GAS_C_PIN     A0

// ── NeoPixel 설정 ─────────────────────────────────────
#define NUM_ARR  2
#define ARR_L    1
#define ARR_R    0

Adafruit_NeoPixel arrC = Adafruit_NeoPixel(NUM_ARR, PIN_ARR_C, NEO_GRB + NEO_KHZ800);

// ── HC12 ──────────────────────────────────────────────
SoftwareSerial hc12(HC12_RX_PIN, HC12_TX_PIN);

// ── 색상 상수 ─────────────────────────────────────────
const uint32_t C_RED     = 0xFF0000UL;
const uint32_t C_GREEN   = 0x00FF00UL;
const uint32_t C_STANDBY = 0x646464UL;

// ── 임계값 / 타이밍 ───────────────────────────────────
#define GAS_THRESHOLD      450   // 화재 감지 진입 기준 (ON 임계값)
#define PEAK_DROP           50   // peak 대비 이만큼 떨어지면 해제 후보
#define DECLINE_CONFIRM      2   // 연속 하락 N회로 감소 전환 확정
#define REARM_CLEAR_THR    400   // rearm 해제 기준 (임계값보다 충분히 아래)
#define ACK_TIMEOUT_MS     500
#define ACK_RETRY            5
#define DISP_INTERVAL       50
#define PREHEAT_SEC          5

// ── 상태 ──────────────────────────────────────────────
enum FireState { NORMAL, FIRE_LEFT, FIRE_CENTER, FIRE_RIGHT };
FireState state     = NORMAL;
bool      localFire = false;
bool      needRearmC = false;  // true: 가스가 임계값 아래로 내려가야 재감지 허용

// ── 타이머 ────────────────────────────────────────────
uint32_t dispMs      = 0;
uint32_t debugMs     = 0;

// ── Peak 추적 (center 가스센서) ──────────────────────
int      gasCPeak       = 0;     // 기록된 최고값
int      gasCPrev       = 0;     // 직전 읽기값
uint16_t gasCDeclineCnt = 0;     // 연속 하락 카운트
bool     gasCPeakLocked = false; // 감소 전환 확정 플래그

// ── 해제 지연 (2초 연속 유지 확정) ───────────────────
#define CLEAR_HOLD_MS   2000

bool     clearPending = false;
uint32_t clearStartMs = 0;

// ── 불꽃 감지 확정 (0.5초 연속 감지) ───────────────────
#define FLAME_HOLD_MS   500

bool     flameCPending   = false;
uint32_t flameCStartMs   = 0;
bool     flameCConfirmed = false;

// ══════════════════════════════════════════════════════
// 불꽃 감지 확정 갱신 (매 loop 호출)
// ══════════════════════════════════════════════════════
void updateFlameC() {
  if (digitalRead(FLAME_C_PIN) == LOW) {
    if (!flameCPending) {
      flameCPending = true;
      flameCStartMs = millis();
    } else if (!flameCConfirmed && millis() - flameCStartMs >= FLAME_HOLD_MS) {
      flameCConfirmed = true;
      Serial.println("FLAME C confirmed (2s hold)");
    }
  } else {
    flameCPending   = false;
    flameCConfirmed = false;
  }
}

// ══════════════════════════════════════════════════════
// 센서 읽기 – 감지 진입용
//   가스: 즉시 트리거 / 불꽃: 2초 확정 후 트리거
// ══════════════════════════════════════════════════════
bool sensorCenter() {
  return (analogRead(GAS_C_PIN) > GAS_THRESHOLD)
      || flameCConfirmed;
}

// ══════════════════════════════════════════════════════
// Peak 추적 초기화
// ══════════════════════════════════════════════════════
void resetPeakC() {
  gasCPeak       = 0;
  gasCPrev       = 0;
  gasCDeclineCnt = 0;
  gasCPeakLocked = false;
  clearPending   = false;
}

// ══════════════════════════════════════════════════════
// Peak 추적 갱신 + 해제 판정 (매 loop 호출)
//   반환: true = 해제 확정
// ══════════════════════════════════════════════════════
bool updatePeakAndCheckClearC() {
  int cur = analogRead(GAS_C_PIN);
  bool flameClear = (digitalRead(FLAME_C_PIN) == HIGH);

  // ── 상승 구간: peak 갱신 ──
  if (cur > gasCPeak) {
    gasCPeak       = cur;
    gasCDeclineCnt = 0;
    gasCPeakLocked = false;
    clearPending   = false;          // 아직 상승 중이면 해제 후보도 리셋
  }
  // ── 하락 판정 ──
  else if (cur < gasCPrev) {
    if (gasCDeclineCnt < 1000) gasCDeclineCnt++;   // 오버플로우 방지
    if (gasCDeclineCnt >= DECLINE_CONFIRM) {
      gasCPeakLocked = true;         // 감소 전환 확정
    }
  }
  // ── 같거나 반등 ──
  else {
    // 아직 peakLocked 아니면 카운트 리셋 (출렁임 방지)
    if (!gasCPeakLocked) {
      gasCDeclineCnt = 0;
    }
    // peakLocked 상태에서 다시 peak 이상 오르면 peak 갱신
    if (gasCPeakLocked && cur > gasCPeak) {
      gasCPeak       = cur;
      gasCDeclineCnt = 0;
      gasCPeakLocked = false;
      clearPending   = false;
    }
  }

  gasCPrev = cur;

  // ── 해제 후보 확인 ──
  // 경로 A: peak 추적 해제 (가스가 실제로 높았던 경우)
  //   감소 전환 확정 + 가스값 peak-DROP 이하 + 불꽃 미감지
  // 경로 B: 가스가 ON 임계값 미만 + 불꽃 미감지
  //   (불꽃만으로 트리거된 경우, 가스값은 낮으므로 peak 추적 무의미)
  bool clearCondition =
    (gasCPeakLocked && cur <= (gasCPeak - PEAK_DROP) && flameClear)   // A
    || (cur < GAS_THRESHOLD && flameClear);                           // B

  if (clearCondition) {
    if (!clearPending) {
      clearPending = true;
      clearStartMs = millis();
      Serial.print("Clear candidate: peak=");
      Serial.print(gasCPeak);
      Serial.print(" cur=");
      Serial.print(cur);
      Serial.println(" → holding 2s…");
    } else if (millis() - clearStartMs >= CLEAR_HOLD_MS) {
      return true;   // 해제 확정!
    }
  } else {
    // 조건 이탈 → 타이머 리셋 (하지만 peakLocked는 유지)
    clearPending = false;
  }

  return false;
}

// ══════════════════════════════════════════════════════
// 화면 갱신
// ══════════════════════════════════════════════════════
void applyDisplay() {
  arrC.clear();

  switch (state) {

    case NORMAL:
      arrC.setPixelColor(ARR_L, C_STANDBY);
      arrC.setPixelColor(ARR_R, C_STANDBY);
      break;

    case FIRE_LEFT:
      arrC.setPixelColor(ARR_L, C_RED);
      arrC.setPixelColor(ARR_R, C_GREEN);
      break;

    case FIRE_CENTER:
      arrC.setPixelColor(ARR_L, C_RED);
      arrC.setPixelColor(ARR_R, C_RED);
      break;

    case FIRE_RIGHT:
      arrC.setPixelColor(ARR_L, C_GREEN);
      arrC.setPixelColor(ARR_R, C_RED);
      break;
  }

  arrC.show();
}

// ══════════════════════════════════════════════════════
// 디버그 출력
// ══════════════════════════════════════════════════════
void printDebug() {
  int  gasC   = analogRead(GAS_C_PIN);
  bool flameC = (digitalRead(FLAME_C_PIN) == LOW);
  bool trigC  = (gasC > GAS_THRESHOLD) || flameCConfirmed;

  Serial.println("──────────────────────────");
  Serial.print("[CENTER]  GAS: ");
  Serial.print(gasC);
  Serial.print(gasC > GAS_THRESHOLD ? " ▲OVER" : "      ");
  Serial.print("  FLAME: ");
  if (flameCConfirmed)    Serial.print("CONF▲");
  else if (flameCPending) Serial.print("pend.");
  else if (flameC)        Serial.print("raw  ");
  else                    Serial.print(" no  ");
  Serial.print("  (D7=");
  Serial.print(digitalRead(FLAME_C_PIN));
  Serial.print(")  ->  ");
  Serial.println(trigC ? "FIRE TRIGGERED" : "normal");

  Serial.print("[STATE: ");
  switch (state) {
    case NORMAL:      Serial.print("NORMAL");      break;
    case FIRE_LEFT:   Serial.print("FIRE_LEFT");   break;
    case FIRE_CENTER: Serial.print("FIRE_CENTER"); break;
    case FIRE_RIGHT:  Serial.print("FIRE_RIGHT");  break;
  }
  Serial.print("]  [LOCAL: ");
  Serial.print(localFire ? "YES" : " no");
  Serial.print("]  [THR: ");
  Serial.print(GAS_THRESHOLD);
  if (needRearmC) Serial.print("]  [REARM: WAIT");
  Serial.println("]");

  // Peak 추적 정보 출력
  if (localFire && state == FIRE_CENTER) {
    Serial.print("[PEAK: ");
    Serial.print(gasCPeak);
    Serial.print("]  [LOCKED: ");
    Serial.print(gasCPeakLocked ? "YES" : " no");
    Serial.print("]  [DECLINE: ");
    Serial.print(gasCDeclineCnt);
    Serial.print("]  [CLR_THR: ");
    Serial.print(gasCPeak - PEAK_DROP);
    Serial.print("]  [PEND: ");
    Serial.print(clearPending ? "YES" : " no");
    Serial.println("]");
  }
}

// ══════════════════════════════════════════════════════
// HC12 통신
// ══════════════════════════════════════════════════════
bool sendWithAck(const String& msg) {
  for (int i = 0; i < ACK_RETRY; i++) {
    hc12.println(msg);
    uint32_t t = millis();
    while (millis() - t < ACK_TIMEOUT_MS) {
      if (hc12.available()) {
        String r = hc12.readStringUntil('\n');
        r.trim();
        if (r == "ACK") {
          Serial.print("ACK OK: "); Serial.println(msg);
          return true;
        }
      }
      // 블로킹 중 display 유지
      if (millis() - dispMs >= DISP_INTERVAL) {
        dispMs = millis();
        applyDisplay();
      }
    }
    Serial.print("Retry "); Serial.print(i + 1);
    Serial.print(": "); Serial.println(msg);
  }
  Serial.println("ACK FAIL (applying locally)");
  return false;
}

void sendAck() { hc12.println("ACK"); }

// ══════════════════════════════════════════════════════
// HC12 수신 처리
// ══════════════════════════════════════════════════════
void handleIncoming() {
  if (!hc12.available()) return;
  String msg = hc12.readStringUntil('\n');
  msg.trim();
  if (msg.length() == 0) return;

  Serial.print("RX: "); Serial.println(msg);

  if (msg == "FIRE:L" && state == NORMAL) {
    sendAck();
    state     = FIRE_LEFT;
    localFire = false;
  }
  else if (msg == "FIRE:R" && state == NORMAL) {
    sendAck();
    state     = FIRE_RIGHT;
    localFire = false;
  }
  else if (msg == "CLEAR" && !localFire) {
    sendAck();
    state = NORMAL;
    needRearmC = (analogRead(GAS_C_PIN) >= GAS_THRESHOLD);
    resetPeakC();
    Serial.print("State: NORMAL (remote clear)");
    if (needRearmC) Serial.print(" [REARM: WAIT]");
    Serial.println();
  }
}

// ══════════════════════════════════════════════════════
// setup / loop
// ══════════════════════════════════════════════════════
void setup() {
  Serial.begin(9600);
  hc12.begin(9600);

  pinMode(FLAME_C_PIN, INPUT_PULLUP);

  arrC.begin();
  arrC.setBrightness(100);
  arrC.show();

  // MQ-2 예열
  Serial.print("MQ-2 preheating... ");
  Serial.print(PREHEAT_SEC); Serial.println("s");
  for (int i = PREHEAT_SEC; i > 0; i--) {
    Serial.print("  remain: "); Serial.print(i); Serial.println("s");
    arrC.setPixelColor(ARR_L, (i % 2 == 0) ? 0x000088UL : 0x000000UL);
    arrC.setPixelColor(ARR_R, (i % 2 == 0) ? 0x000000UL : 0x000088UL);
    arrC.show();
    delay(1000);
  }
  arrC.clear(); arrC.show();

  Serial.println("=== mid_nano ready (peak tracking v3) ===");
}

void loop() {
  // 1. HC12 수신 처리
  handleIncoming();

  // 2. 불꽃 감지 확정 갱신
  updateFlameC();

  // 3. Rearm 처리: 해제 후 가스가 충분히 낮아져야 재감지 허용
  if (needRearmC && analogRead(GAS_C_PIN) < REARM_CLEAR_THR) {
    needRearmC = false;
    Serial.println("CENTER rearm OK");
  }

  // 3. 로컬 센서 감지 (rearm 해제일 때만)
  if (state == NORMAL && !needRearmC && sensorCenter()) {
    Serial.println("FIRE: CENTER");
    state        = FIRE_CENTER;
    localFire    = true;
    // Peak 추적 시작: 현재값으로 초기화
    int cur      = analogRead(GAS_C_PIN);
    gasCPeak     = cur;
    gasCPrev     = cur;
    gasCDeclineCnt = 0;
    gasCPeakLocked = false;
    clearPending = false;
    sendWithAck("FIRE:C");
  }

  // 4. 화재 해제 – Peak 추적 방식
  //    (로컬 감지원일 때만 해제 판단)
  if (localFire && state == FIRE_CENTER) {
    if (updatePeakAndCheckClearC()) {
      Serial.print("Fire cleared (peak=");
      Serial.print(gasCPeak);
      Serial.println(") -> NORMAL");

      // 가스가 아직 임계값 이상이면 rearm 필요
      if (analogRead(GAS_C_PIN) >= GAS_THRESHOLD) needRearmC = true;

      state     = NORMAL;
      localFire = false;
      resetPeakC();
      sendWithAck("CLEAR");
    }
  }

  // 4. 화면 갱신
  if (millis() - dispMs >= DISP_INTERVAL) {
    dispMs = millis();
    applyDisplay();
  }

  // 5. 디버그 출력 (1초 간격)
  if (millis() - debugMs >= 1000) {
    debugMs = millis();
    printDebug();
  }
}