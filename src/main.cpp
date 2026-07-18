#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <IRremote.hpp>

#define IR_SEND_PIN    5
#define IR_RECEIVE_PIN 3
#define BUTTON_PIN     10

/* ─── OLED (SSD1306, I2C) ─────────────────────────────────────────── */
#define OLED_SDA   7
#define OLED_SCL   6
#define OLED_ADDR  0x3C
#define SCREEN_W   128
#define SCREEN_H   64

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

/* ─── Thermistor (NTC, Beta equation) ─────────────────────────────────
 * Divider: 3.3V -> 10k -> THERM_PIN -> thermistor -> GND. */
#define THERM_PIN         0
#define ADC_MAX           4095.0f
#define THERM_NOMINAL_R   10000.0f
#define THERM_NOMINAL_T   25.0f
#define THERM_BETA        3950.0f
#define THERM_SERIES_R    10000.0f

#define TEMP_ON_F     75.0f
#define TEMP_OFF_F    73.0f
#define MIN_RUN_TIME  600000UL
#define TX_ANIM_MS    2500UL
#define CODE_GAP_MS   50UL

/* ─── Scan mode ────────────────────────────────────────────────────── */
#define SCAN_DURATION_MS  10000UL
#define MAX_LEARNED       8
#define CODE_FLASH_MS     600UL

#define WOOZOO_CODE  0xFF00DE80UL

static uint32_t learnedCodes[MAX_LEARNED];
static int      learnedCount   = 0;
static bool     scanMode       = false;
static uint32_t scanStart      = 0;
static uint32_t codeFlashUntil = 0;

/* ─── Normal-mode state ────────────────────────────────────────────── */
static bool     deviceActive  = true;   // single-tap toggles this
static uint32_t lastTempCheck = 0;
static bool     fanOn         = false;
static uint32_t fanOnTime     = 0;
static float    lastTempF     = 70.0f;
static uint32_t txAnimUntil   = 0;

/* ─── Button: debounce + single/double-tap detection ──────────────── */
#define DEBOUNCE_MS    50
#define DOUBLE_TAP_MS 300   // max gap between two presses to count as double-tap

static bool     btnStable     = HIGH;
static bool     btnLastRead   = HIGH;
static uint32_t btnDebounce   = 0;
static int      btnPressCount = 0;
static uint32_t btnFirstPress = 0;

enum BtnEvent { BTN_NONE, BTN_SINGLE, BTN_DOUBLE };

/* Returns BTN_DOUBLE immediately on the second press within DOUBLE_TAP_MS,
 * or BTN_SINGLE once the window expires with only one press. */
static BtnEvent checkButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != btnLastRead) btnDebounce = millis();
  btnLastRead = reading;

  BtnEvent event = BTN_NONE;

  if ((millis() - btnDebounce) >= DEBOUNCE_MS && reading != btnStable) {
    btnStable = reading;
    if (btnStable == LOW) {                  // falling edge = press
      btnPressCount++;
      if (btnPressCount == 1) {
        btnFirstPress = millis();
      } else if (btnPressCount >= 2) {
        event = BTN_DOUBLE;
        btnPressCount = 0;
      }
    }
  }

  if (btnPressCount == 1 && (millis() - btnFirstPress) >= DOUBLE_TAP_MS) {
    event = BTN_SINGLE;
    btnPressCount = 0;
  }

  return event;
}

/* ─── Snow ─────────────────────────────────────────────────────────── */
#define N_SNOW 16
struct Snow { float x, y, speed, sway; uint8_t r; };
static Snow snow[N_SNOW];
static float lastFrame = 0;

/* ─── Drawing helpers ──────────────────────────────────────────────── */

static void drawWind(float t, int baseY, int amp, float k, float speed, int dash) {
  int prevX = 0;
  int prevY = baseY + (int)(amp * sinf(-speed * t));
  for (int x = 2; x <= SCREEN_W; x += 2) {
    int y = baseY + (int)(amp * sinf(k * x - speed * t));
    if (((x / 2) % dash) != 0)
      display.drawLine(prevX, prevY, x, y, SSD1306_WHITE);
    prevX = x;
    prevY = y;
  }
}

static void drawSnowflake(int x, int y, int r) {
  if (r <= 0) { display.drawPixel(x, y, SSD1306_WHITE); return; }
  display.drawFastHLine(x - r, y, 2 * r + 1, SSD1306_WHITE);
  display.drawFastVLine(x, y - r, 2 * r + 1, SSD1306_WHITE);
  int d = (r * 7) / 10;
  display.drawLine(x - d, y - d, x + d, y + d, SSD1306_WHITE);
  display.drawLine(x - d, y + d, x + d, y - d, SSD1306_WHITE);
}

static void drawChevron(int x, int y) {
  display.drawLine(x,     y - 3, x + 3, y, SSD1306_WHITE);
  display.drawLine(x + 3, y,     x,     y + 3, SSD1306_WHITE);
  display.drawLine(x + 3, y - 3, x + 6, y, SSD1306_WHITE);
  display.drawLine(x + 6, y,     x + 3, y + 3, SSD1306_WHITE);
}

static void drawBreezeGust(float t) {
  drawWind(t, 6,  9,  0.15f, 6.0f, 2);
  drawWind(t, 20, 10, 0.10f, 7.0f, 2);
  drawWind(t, 44, 10, 0.12f, 6.5f, 2);
  drawWind(t, 58, 9,  0.16f, 7.5f, 2);
  int sweepX = (int)fmodf(t * 60.0f, SCREEN_W + 24) - 24;
  drawChevron(sweepX,      SCREEN_H / 2);
  drawChevron(sweepX - 16, SCREEN_H / 2 - 12);
  drawChevron(sweepX - 16, SCREEN_H / 2 + 12);
}

static void drawScanMode() {
  uint32_t elapsed   = millis() - scanStart;
  uint32_t remaining = (elapsed < SCAN_DURATION_MS) ? (SCAN_DURATION_MS - elapsed) : 0;
  int secsLeft       = (int)((remaining + 999) / 1000);

  display.clearDisplay();

  // Blinking title
  if ((millis() / 400) % 2) {
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(4, 2);
    display.print("SCAN MODE");
  }

  // Countdown in top-right corner
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(SCREEN_W - 24, 2);
  display.print(secsLeft);

  // Code count
  display.setTextSize(1);
  display.setCursor(10, 24);
  display.print("Found: ");
  display.print(learnedCount);
  display.print(" / ");
  display.print(MAX_LEARNED);

  // Inverted flash banner when a new code is captured
  if (millis() < codeFlashUntil) {
    display.fillRect(0, 35, SCREEN_W, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(14, 38);
    display.print("CODE CAPTURED!");
    display.setTextColor(SSD1306_WHITE);
  }

  // Progress bar (fills left->right as time elapses)
  const int barX = 8, barY = 52, barW = SCREEN_W - 16, barH = 8;
  int filled = constrain((int)((float)elapsed / SCAN_DURATION_MS * barW), 0, barW);
  display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  if (filled > 0) display.fillRect(barX, barY, filled, barH, SSD1306_WHITE);

  display.display();
}

/* ─── Thermistor ───────────────────────────────────────────────────── */
static float readThermistorF() {
  int adc      = analogRead(THERM_PIN);
  float rTherm = THERM_SERIES_R * adc / (ADC_MAX - adc);
  float invT   = logf(rTherm / THERM_NOMINAL_R) / THERM_BETA
                 + 1.0f / (THERM_NOMINAL_T + 273.15f);
  float tempC  = 1.0f / invT - 273.15f;
  return tempC * 9.0f / 5.0f + 32.0f;
}

/* ─── IR send ──────────────────────────────────────────────────────── */
/* Sends Woozoo first, then every learned code with a brief gap.
 * RX is off during normal operation, so no restartTimer() needed. */
static void sendPowerToggle() {
  Serial.println("Sending power signal...");
  IrSender.sendNECRaw(WOOZOO_CODE, 0);
  for (int i = 0; i < learnedCount; i++) {
    delay(CODE_GAP_MS);
    IrSender.sendNECRaw(learnedCodes[i], 0);
  }
  txAnimUntil = millis() + TX_ANIM_MS;
}

/* ─── Fan state machine ────────────────────────────────────────────── */
static void updateFanState(float tempF) {
  if (!fanOn && tempF >= TEMP_ON_F) {
    sendPowerToggle();
    fanOn     = true;
    fanOnTime = millis();
  } else if (fanOn && tempF <= TEMP_OFF_F && millis() - fanOnTime >= MIN_RUN_TIME) {
    sendPowerToggle();
    fanOn = false;
  }
}

/* ─── Scan mode entry / exit ───────────────────────────────────────── */
static void enterScanMode() {
  if (scanMode) return;
  scanMode  = true;
  scanStart = millis();
  IrReceiver.start();
  Serial.println("Scan mode: started");
}

static void exitScanMode() {
  scanMode = false;
  IrReceiver.stop();
  Serial.print("Scan mode: ended, learned=");
  Serial.println(learnedCount);
}

/* ═══════════════════════════════════════════════════════════════════ */
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  IrSender.begin(IR_SEND_PIN);
  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
  IrReceiver.stop();   // RX stays off until scan mode is triggered

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
    Serial.println("SSD1306 not found");
  }
  display.setTextWrap(false);

  randomSeed(micros());
  for (int i = 0; i < N_SNOW; i++) {
    snow[i].x     = random(SCREEN_W);
    snow[i].y     = random(SCREEN_H);
    snow[i].speed = 7 + random(18);
    snow[i].sway  = random(628) / 100.0f;
    snow[i].r     = random(3);
  }

  Serial.println("Ready");
}

void loop() {
  // ── Button events ──
  switch (checkButton()) {
    case BTN_SINGLE:
      deviceActive = !deviceActive;
      Serial.println(deviceActive ? "Active" : "Inactive");
      break;
    case BTN_DOUBLE:
      enterScanMode();
      break;
    default: break;
  }

  /* ── Scan mode: owns the display for its duration ── */
  if (scanMode) {
    if (IrReceiver.decode()) {
      uint32_t code = IrReceiver.decodedIRData.decodedRawData;
      IrReceiver.resume();
      if (code != 0 && code != WOOZOO_CODE) {
        bool known = false;
        for (int i = 0; i < learnedCount; i++)
          if (learnedCodes[i] == code) { known = true; break; }
        if (!known && learnedCount < MAX_LEARNED) {
          learnedCodes[learnedCount++] = code;
          codeFlashUntil = millis() + CODE_FLASH_MS;
          Serial.print("Learned: 0x");
          Serial.println(code, HEX);
        }
      }
    }
    if (millis() - scanStart >= SCAN_DURATION_MS) exitScanMode();
    else drawScanMode();
    return;
  }

  /* ── Normal mode ── */

  // Temperature check and fan control only when active
  if (deviceActive && millis() - lastTempCheck >= 5000) {
    lastTempF = readThermistorF();
    Serial.print("Temp: "); Serial.print(lastTempF); Serial.println("F");
    updateFanState(lastTempF);
    lastTempCheck = millis();
  }

  // Animation frame
  float t  = millis() / 1000.0f;
  float dt = t - lastFrame;
  lastFrame = t;
  display.clearDisplay();

  if (millis() < txAnimUntil) {
    drawBreezeGust(t);
  } else {
    drawWind(t, 10, 5, 0.11f, 3.0f, 4);
    drawWind(t, 32, 7, 0.08f, 2.2f, 3);
    drawWind(t, 54, 6, 0.13f, 3.6f, 5);
  }

  for (int i = 0; i < N_SNOW; i++) {
    snow[i].y += snow[i].speed * dt;
    if (snow[i].y > SCREEN_H + 2) {
      snow[i].y = -2;
      snow[i].x = random(SCREEN_W);
    }
    int sx = (int)(snow[i].x + 5.0f * sinf(t * 1.3f + snow[i].sway));
    drawSnowflake(sx, (int)snow[i].y, snow[i].r);
  }

  // Temperature readout badge (bobbing)
  const int bob = (int)(2.0f * sinf(t * 1.5f));
  const int bw  = 100, bh = 42;
  const int bx  = (SCREEN_W - bw) / 2;
  const int by  = (SCREEN_H - bh) / 2 + bob;

  display.fillRoundRect(bx, by, bw, bh, 6, SSD1306_BLACK);
  display.drawRoundRect(bx, by, bw, bh, 6, SSD1306_WHITE);
  if ((millis() / 350) % 2)
    display.drawRoundRect(bx + 3, by + 3, bw - 6, bh - 6, 4, SSD1306_WHITE);

  char tempStr[3];
  snprintf(tempStr, sizeof(tempStr), "%2d", (int)(lastTempF + 0.5f));
  const int numW = 6 * 3 * 2;
  const int numX = bx + (bw - numW) / 2;
  const int numY = by + 5;
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(numX, numY);     display.print(tempStr);
  display.setCursor(numX + 1, numY); display.print(tempStr);

  // Sub-line: thresholds when active, INACTIVE label when paused
  display.setTextSize(1);
  if (deviceActive) {
    char threshStr[16];
    snprintf(threshStr, sizeof(threshStr), "ON:%d OFF:%d", (int)TEMP_ON_F, (int)TEMP_OFF_F);
    const int subW = strlen(threshStr) * 6;
    display.setCursor(bx + (bw - subW) / 2, by + bh - 11);
    display.print(threshStr);
  } else {
    const int subW = 8 * 6;   // "INACTIVE" = 8 chars
    display.setCursor(bx + (bw - subW) / 2, by + bh - 11);
    display.print("INACTIVE");
  }

  // Bottom bar
  display.fillRect(0, 56, SCREEN_W, 8, SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 56);
  display.print("fans:");
  display.print(learnedCount + 1);
  display.print(deviceActive ? " | dbl:scan" : " | PAUSED");

  display.display();
}
