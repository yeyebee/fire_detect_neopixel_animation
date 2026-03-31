// =====================================================
// side_nano.ino  ─  좌/우측 아두이노 나노
// =====================================================

#include <Adafruit_NeoPixel.h>
#include <SoftwareSerial.h>

// ── 핀 정의 ───────────────────────────────────────────
#define HC12_RX_PIN   2
#define HC12_TX_PIN   3
#define PIN_ARR_L     4
#define PIN_STRIP     5
#define PIN_ARR_R     6
#define FLAME_L_PIN   7
#define FLAME_R_PIN   8
#define BUZZER_PIN    9
#define GAS_L_PIN     A0
#define GAS_R_PIN     A1

// ── NeoPixel 설정 ─────────────────────────────────────
#define NUM_ARR    2
#define NUM_STRIP  30
#define ZONE_SZ    10

#define ARR_L 1
#define ARR_R 0

Adafruit_NeoPixel arrL  = Adafruit_NeoPixel(NUM_ARR,   PIN_ARR_L, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel arrR  = Adafruit_NeoPixel(NUM_ARR,   PIN_ARR_R, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUM_STRIP, PIN_STRIP,  NEO_GRB + NEO_KHZ800);

// ── HC12 ──────────────────────────────────────────────
SoftwareSerial hc12(HC12_RX_PIN, HC12_TX_PIN);

// ── 색상 상수 ─────────────────────────────────────────
const uint32_t C_RED     = 0xFF0000UL;
const uint32_t C_GREEN   = 0x00FF00UL;
const uint32_t C_GRN_MID = 0x007800UL;
const uint32_t C_GRN_DIM = 0x002800UL;
const uint32_t C_STANDBY = 0x646464UL;
const uint32_t C_OFF     = 0x000000UL;

// ── 임계값 / 타이밍 ───────────────────────────────────
#define GAS_THRESHOLD      450   // 화재 감지 진입 기준 (ON 임계값)
#define PEAK_DROP           50   // peak 대비 이만큼 떨어지면 해제 후보
#define DECLINE_CONFIRM      2   // 연속 하락 N회로 감소 전환 확정
#define REARM_CLEAR_THR    400   // rearm 해제 기준 (임계값보다 충분히 아래)
#define ACK_TIMEOUT_MS     500
#define ACK_RETRY            5
#define ANIM_INTERVAL       60
#define PREHEAT_SEC          5

// ── 상태 정의 ─────────────────────────────────────────
enum FireState { NORMAL, FIRE_LEFT, FIRE_CENTER, FIRE_RIGHT };
FireState state     = NORMAL;
bool      localFire = false;
bool      needRearmL = false;  // true: 좌측 가스가 임계값 아래로 내려가야 재감지 허용
bool      needRearmR = false;  // true: 우측 가스가 임계값 아래로 내려가야 재감지 허용

// ── 타이머 ────────────────────────────────────────────
uint32_t animMs  = 0;
uint32_t debugMs = 0;
uint32_t dispMs  = 0;
int      animOff = 0;

// ── Peak 추적 구조체 ─────────────────────────────────
struct GasPeakTracker {
  int      peak;          // 기록된 최고값
  int      prev;          // 직전 읽기값
  uint16_t declineCnt;    // 연속 하락 카운트
  bool     peakLocked;    // 감소 전환 확정 플래그
  bool     clearPending;  // 해제 후보 진행 중
  uint32_t clearStartMs;  // 해제 후보 시작 시각
};

GasPeakTracker trkL = { 0, 0, 0, false, false, 0 };
GasPeakTracker trkR = { 0, 0, 0, false, false, 0 };

// ── 해제 지연 ────────────────────────────────────────
#define CLEAR_HOLD_MS   2000

// ── 불꽃 감지 확정 (0.5초 연속 감지) ───────────────────
#define FLAME_HOLD_MS   500

bool     flameLPending   = false;
uint32_t flameLStartMs   = 0;
bool     flameLConfirmed = false;

bool     flameRPending   = false;
uint32_t flameRStartMs   = 0;
bool     flameRConfirmed = false;

// ── 부저 패턴 (삐빕-삐빕-...) ────────────────────────
const uint16_t BUZZ_PATTERN[]  = { 100, 80, 200, 600 };
const bool     BUZZ_STATE[]    = { true, false, true, false };
const uint8_t  BUZZ_STEPS      = 4;

uint8_t  buzzStep  = 0;
uint32_t buzzMs    = 0;

// ══════════════════════════════════════════════════════
// 불꽃 감지 확정 갱신 (매 loop 호출)
// ══════════════════════════════════════════════════════
void updateFlameL() {
  if (digitalRead(FLAME_L_PIN) == LOW) {
    if (!flameLPending) {
      flameLPending = true;
      flameLStartMs = millis();
    } else if (!flameLConfirmed && millis() - flameLStartMs >= FLAME_HOLD_MS) {
      flameLConfirmed = true;
      Serial.println("FLAME L confirmed (2s hold)");
    }
  } else {
    flameLPending   = false;
    flameLConfirmed = false;
  }
}

void updateFlameR() {
  if (digitalRead(FLAME_R_PIN) == LOW) {
    if (!flameRPending) {
      flameRPending = true;
      flameRStartMs = millis();
    } else if (!flameRConfirmed && millis() - flameRStartMs >= FLAME_HOLD_MS) {
      flameRConfirmed = true;
      Serial.println("FLAME R confirmed (2s hold)");
    }
  } else {
    flameRPending   = false;
    flameRConfirmed = false;
  }
}

// ══════════════════════════════════════════════════════
// 센서 읽기 – 감지 진입용
//   가스: 즉시 트리거 / 불꽃: 2초 확정 후 트리거
// ══════════════════════════════════════════════════════
bool sensorLeft() {
  return (analogRead(GAS_L_PIN) > GAS_THRESHOLD)
      || flameLConfirmed;
}
bool sensorRight() {
  return (analogRead(GAS_R_PIN) > GAS_THRESHOLD)
      || flameRConfirmed;
}

// ══════════════════════════════════════════════════════
// Peak 추적 초기화
// ══════════════════════════════════════════════════════
void resetPeak(GasPeakTracker &t) {
  t.peak         = 0;
  t.prev         = 0;
  t.declineCnt   = 0;
  t.peakLocked   = false;
  t.clearPending = false;
}

void initPeak(GasPeakTracker &t, int curVal) {
  t.peak         = curVal;
  t.prev         = curVal;
  t.declineCnt   = 0;
  t.peakLocked   = false;
  t.clearPending = false;
}

// ══════════════════════════════════════════════════════
// Peak 추적 갱신 + 해제 판정
//   gasPin   : 아날로그 핀
//   flamePin : 불꽃 디지털 핀
//   t        : 해당 채널 트래커
//   반환: true = 해제 확정
// ══════════════════════════════════════════════════════
bool updatePeakAndCheckClear(int gasPin, int flamePin, GasPeakTracker &t) {
  int  cur        = analogRead(gasPin);
  bool flameClear = (digitalRead(flamePin) == HIGH);

  // ── 상승 구간: peak 갱신 ──
  if (cur > t.peak) {
    t.peak         = cur;
    t.declineCnt   = 0;
    t.peakLocked   = false;
    t.clearPending = false;
  }
  // ── 하락 판정 ──
  else if (cur < t.prev) {
    if (t.declineCnt < 1000) t.declineCnt++;   // 오버플로우 방지
    if (t.declineCnt >= DECLINE_CONFIRM) {
      t.peakLocked = true;
    }
  }
  // ── 같거나 반등 ──
  else {
    if (!t.peakLocked) {
      t.declineCnt = 0;
    }
    // peakLocked 상태에서 다시 peak 이상 오르면 peak 갱신
    if (t.peakLocked && cur > t.peak) {
      t.peak         = cur;
      t.declineCnt   = 0;
      t.peakLocked   = false;
      t.clearPending = false;
    }
  }

  t.prev = cur;

  // ── 해제 후보 확인 ──
  // 경로 A: peak 추적 해제 (가스가 실제로 높았던 경우)
  // 경로 B: 가스가 ON 임계값 미만 + 불꽃 미감지 (불꽃만 트리거)
  bool clearCondition =
    (t.peakLocked && cur <= (t.peak - PEAK_DROP) && flameClear)
    || (cur < GAS_THRESHOLD && flameClear);

  if (clearCondition) {
    if (!t.clearPending) {
      t.clearPending = true;
      t.clearStartMs = millis();
      Serial.print("Clear candidate: peak=");
      Serial.print(t.peak);
      Serial.print(" cur=");
      Serial.print(cur);
      Serial.println(" → holding 2s…");
    } else if (millis() - t.clearStartMs >= CLEAR_HOLD_MS) {
      return true;   // 해제 확정!
    }
  } else {
    t.clearPending = false;
  }

  return false;
}

// ══════════════════════════════════════════════════════
// 줄 네오픽셀 유틸
// ══════════════════════════════════════════════════════
void fillZone(int zoneStart, uint32_t color) {
  for (int i = 0; i < ZONE_SZ; i++)
    strip.setPixelColor(zoneStart + i, color);
}

void chaseStrip(int start, int len, int dir, int offset) {
  int head = (dir == 1)
    ? (offset % len)
    : (len - 1 - (offset % len));

  for (int i = 0; i < len; i++) {
    int trail = (dir == 1)
      ? ((head - i + len) % len)
      : ((i - head + len) % len);

    uint32_t c;
    switch (trail) {
      case 0:  c = C_GREEN;   break;
      case 1:  c = C_GRN_MID; break;
      case 2:  c = C_GRN_DIM; break;
      default: c = C_OFF;     break;
    }
    strip.setPixelColor(start + i, c);
  }
}

// ══════════════════════════════════════════════════════
// 부저 갱신 (non-blocking)
// ══════════════════════════════════════════════════════
void updateBuzzer() {
  if (state == NORMAL) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzStep = 0;
    buzzMs   = millis();
    return;
  }

  if (millis() - buzzMs >= BUZZ_PATTERN[buzzStep]) {
    buzzMs   = millis();
    buzzStep = (buzzStep + 1) % BUZZ_STEPS;
    digitalWrite(BUZZER_PIN, BUZZ_STATE[buzzStep] ? HIGH : LOW);
  }
}

// ══════════════════════════════════════════════════════
// 화면 갱신
// ══════════════════════════════════════════════════════
void applyDisplay() {
  arrL.clear(); arrR.clear(); strip.clear();

  switch (state) {

    case NORMAL:
      for (int i = 0; i < NUM_STRIP; i++) strip.setPixelColor(i, C_STANDBY);
      arrL.setPixelColor(ARR_L, C_STANDBY);
      arrL.setPixelColor(ARR_R, C_STANDBY);
      arrR.setPixelColor(ARR_L, C_STANDBY);
      arrR.setPixelColor(ARR_R, C_STANDBY);
      break;

    case FIRE_LEFT:
      arrL.setPixelColor(ARR_L, C_RED);
      arrL.setPixelColor(ARR_R, C_GREEN);
      arrR.setPixelColor(ARR_L, C_RED);
      arrR.setPixelColor(ARR_R, C_GREEN);
      fillZone(0, C_RED);
      chaseStrip(10, 20, 1, animOff);
      break;

    case FIRE_CENTER:
      arrL.setPixelColor(ARR_L, C_GREEN);
      arrL.setPixelColor(ARR_R, C_RED);
      arrR.setPixelColor(ARR_L, C_RED);
      arrR.setPixelColor(ARR_R, C_GREEN);
      chaseStrip( 0, 10, -1, animOff);
      fillZone(10, C_RED);
      chaseStrip(20, 10,  1, animOff);
      break;

    case FIRE_RIGHT:
      arrL.setPixelColor(ARR_L, C_GREEN);
      arrL.setPixelColor(ARR_R, C_RED);
      arrR.setPixelColor(ARR_L, C_GREEN);
      arrR.setPixelColor(ARR_R, C_RED);
      chaseStrip(0, 20, -1, animOff);
      fillZone(20, C_RED);
      break;
  }

  arrL.show(); arrR.show(); strip.show();
}

// ══════════════════════════════════════════════════════
// 디버그 출력
// ══════════════════════════════════════════════════════
void printDebug() {
  int  gasL   = analogRead(GAS_L_PIN);
  int  gasR   = analogRead(GAS_R_PIN);
  bool flameL = (digitalRead(FLAME_L_PIN) == LOW);
  bool flameR = (digitalRead(FLAME_R_PIN) == LOW);
  bool trigL  = (gasL > GAS_THRESHOLD) || flameLConfirmed;
  bool trigR  = (gasR > GAS_THRESHOLD) || flameRConfirmed;

  Serial.println("──────────────────────────");
  Serial.print("[LEFT ]  GAS: ");
  Serial.print(gasL);
  Serial.print(gasL > GAS_THRESHOLD ? " ▲OVER" : "      ");
  Serial.print("  FLAME: ");
  if (flameLConfirmed)    Serial.print("CONF▲");
  else if (flameLPending) Serial.print("pend.");
  else if (flameL)        Serial.print("raw  ");
  else                    Serial.print(" no  ");
  Serial.print("  (D7=");
  Serial.print(digitalRead(FLAME_L_PIN));
  Serial.print(")  ->  ");
  Serial.println(trigL ? "FIRE TRIGGERED" : "normal");

  Serial.print("[RIGHT]  GAS: ");
  Serial.print(gasR);
  Serial.print(gasR > GAS_THRESHOLD ? " ▲OVER" : "      ");
  Serial.print("  FLAME: ");
  if (flameRConfirmed)    Serial.print("CONF▲");
  else if (flameRPending) Serial.print("pend.");
  else if (flameR)        Serial.print("raw  ");
  else                    Serial.print(" no  ");
  Serial.print("  (D8=");
  Serial.print(digitalRead(FLAME_R_PIN));
  Serial.print(")  ->  ");
  Serial.println(trigR ? "FIRE TRIGGERED" : "normal");

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
  if (needRearmL || needRearmR) {
    Serial.print("]  [REARM:");
    if (needRearmL) Serial.print(" L");
    if (needRearmR) Serial.print(" R");
  }
  Serial.println("]");

  // Peak 추적 정보 출력
  if (localFire && state == FIRE_LEFT) {
    Serial.print("[L-PEAK: ");
    Serial.print(trkL.peak);
    Serial.print("]  [LOCKED: ");
    Serial.print(trkL.peakLocked ? "YES" : " no");
    Serial.print("]  [DECLINE: ");
    Serial.print(trkL.declineCnt);
    Serial.print("]  [CLR_THR: ");
    Serial.print(trkL.peak - PEAK_DROP);
    Serial.print("]  [PEND: ");
    Serial.print(trkL.clearPending ? "YES" : " no");
    Serial.println("]");
  }
  if (localFire && state == FIRE_RIGHT) {
    Serial.print("[R-PEAK: ");
    Serial.print(trkR.peak);
    Serial.print("]  [LOCKED: ");
    Serial.print(trkR.peakLocked ? "YES" : " no");
    Serial.print("]  [DECLINE: ");
    Serial.print(trkR.declineCnt);
    Serial.print("]  [CLR_THR: ");
    Serial.print(trkR.peak - PEAK_DROP);
    Serial.print("]  [PEND: ");
    Serial.print(trkR.clearPending ? "YES" : " no");
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
      // 블로킹 중 애니메이션 + 부저 유지
      if (millis() - animMs >= ANIM_INTERVAL) {
        animMs = millis();
        animOff++;
        applyDisplay();
      }
      updateBuzzer();
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

  if (msg == "FIRE:C" && state == NORMAL) {
    sendAck();
    state     = FIRE_CENTER;
    localFire = false;
  }
  else if (msg == "FIRE:L" && state == NORMAL) {
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
    // 가스가 아직 임계값 이상이면 rearm 유지 (즉시 재트리거 방지)
    needRearmL = (analogRead(GAS_L_PIN) >= GAS_THRESHOLD);
    needRearmR = (analogRead(GAS_R_PIN) >= GAS_THRESHOLD);
    resetPeak(trkL);
    resetPeak(trkR);
    Serial.print("State: NORMAL (remote clear)");
    if (needRearmL || needRearmR) {
      Serial.print(" [REARM:");
      if (needRearmL) Serial.print(" L");
      if (needRearmR) Serial.print(" R");
      Serial.print("]");
    }
    Serial.println();
  }
}

// ══════════════════════════════════════════════════════
// setup / loop
// ══════════════════════════════════════════════════════
void setup() {
  Serial.begin(9600);
  hc12.begin(9600);

  pinMode(FLAME_L_PIN, INPUT_PULLUP);
  pinMode(FLAME_R_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN,  OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  arrL.begin();  arrL.setBrightness(100); arrL.show();
  arrR.begin();  arrR.setBrightness(100); arrR.show();
  strip.begin(); strip.setBrightness(80); strip.show();

  // MQ-2 예열
  Serial.print("MQ-2 preheating... ");
  Serial.print(PREHEAT_SEC); Serial.println("s");
  for (int i = PREHEAT_SEC; i > 0; i--) {
    int lit = map(i, PREHEAT_SEC, 0, 0, NUM_STRIP);
    strip.clear();
    for (int p = 0; p < lit; p++) strip.setPixelColor(p, 0x000088);
    strip.show();
    Serial.print("  remain: "); Serial.print(i); Serial.println("s");
    delay(1000);
  }
  strip.clear(); strip.show();

  Serial.println("=== side_nano ready (peak tracking v3) ===");
}

void loop() {
  // 1. HC12 수신 처리
  handleIncoming();

  // 2. 불꽃 감지 확정 갱신
  updateFlameL();
  updateFlameR();

  // 3. Rearm 처리: 해제 후 가스가 충분히 낮아져야 재감지 허용
  if (needRearmL && analogRead(GAS_L_PIN) < REARM_CLEAR_THR) {
    needRearmL = false;
    Serial.println("LEFT rearm OK");
  }
  if (needRearmR && analogRead(GAS_R_PIN) < REARM_CLEAR_THR) {
    needRearmR = false;
    Serial.println("RIGHT rearm OK");
  }

  // 3. 로컬 센서 감지 (정상 상태 + rearm 해제일 때만)
  if (state == NORMAL) {
    if (!needRearmL && sensorLeft()) {
      Serial.println("FIRE: LEFT");
      state     = FIRE_LEFT;
      localFire = true;
      initPeak(trkL, analogRead(GAS_L_PIN));
      sendWithAck("FIRE:L");
    }
    else if (!needRearmR && sensorRight()) {
      Serial.println("FIRE: RIGHT");
      state     = FIRE_RIGHT;
      localFire = true;
      initPeak(trkR, analogRead(GAS_R_PIN));
      sendWithAck("FIRE:R");
    }
  }

  // 4. 화재 해제 – Peak 추적 방식 (로컬 감지원일 때만)
  if (localFire) {
    bool cleared = false;

    if (state == FIRE_LEFT) {
      cleared = updatePeakAndCheckClear(GAS_L_PIN, FLAME_L_PIN, trkL);
    }
    else if (state == FIRE_RIGHT) {
      cleared = updatePeakAndCheckClear(GAS_R_PIN, FLAME_R_PIN, trkR);
    }

    if (cleared) {
      GasPeakTracker &t = (state == FIRE_LEFT) ? trkL : trkR;
      Serial.print("Fire cleared (peak=");
      Serial.print(t.peak);
      Serial.println(") -> NORMAL");

      // 가스가 아직 임계값 이상이면 rearm 필요
      if (state == FIRE_LEFT  && analogRead(GAS_L_PIN) >= GAS_THRESHOLD) needRearmL = true;
      if (state == FIRE_RIGHT && analogRead(GAS_R_PIN) >= GAS_THRESHOLD) needRearmR = true;

      state     = NORMAL;
      localFire = false;
      resetPeak(trkL);
      resetPeak(trkR);

      // 부저 OFF 먼저! (sendWithAck 블로킹 중 울리는 것 방지)
      digitalWrite(BUZZER_PIN, LOW);

      sendWithAck("CLEAR");
    }
  }

  // 4. 애니메이션 + 부저 갱신
  if (millis() - animMs >= ANIM_INTERVAL) {
    animMs = millis();
    animOff++;
    applyDisplay();
  }
  updateBuzzer();

  // 5. 디버그 출력 (1초 간격)
  if (millis() - debugMs >= 1000) {
    debugMs = millis();
    printDebug();
  }
}