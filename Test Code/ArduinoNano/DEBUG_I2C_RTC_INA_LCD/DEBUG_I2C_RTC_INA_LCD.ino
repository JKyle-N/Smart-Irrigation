/* =============================================================================
 *  ARDUINO NANO  --  I2C BENCH DEBUG  (RTC + INA226 + LCD on A4/A5)
 * -----------------------------------------------------------------------------
 *  Exercises the three I2C devices used by ESP32 #1, at the SAME addresses, but
 *  here driven from an Arduino Nano over its hardware I2C pins:
 *       A4 = SDA      A5 = SCL        (fixed by the ATmega328 hardware)
 *
 *       LCD 1602/2004 (PCF8574 backpack) .... 0x27
 *       INA226 power monitor ................ 0x40
 *       DS3231 RTC .......................... 0x68
 *
 *  DISPLAY CYCLE (repeats forever):
 *    1. 5 s  -> status screen: "RTC is ready" + "INA226 is ready" (with live
 *               time + bus voltage so you can see the devices actually respond).
 *    2. 10 s -> a running cat animation scrolling across the LCD.
 *
 *  ---- ARDUINO IDE BUILD ----------------------------------------------------
 *    Board:  Arduino AVR > Arduino Nano   (Processor: ATmega328P)
 *    Libraries (Tools > Manage Libraries -> install):
 *      - "LiquidCrystal I2C"  by Frank de Brabander / marcoschwartz
 *      - "RTClib"             by Adafruit
 *      - "INA226"             by Rob Tillaart
 *    Wire.h ships with the AVR core.
 *  Set LCD_COLS / LCD_ROWS below to match your panel (20x4 default, 16x2 common).
 * ========================================================================== */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <INA226.h>

/* ---- EDIT: panel + device addresses -------------------------------------- */
#define LCD_COLS   20            // set 16 for a 1602 panel
#define LCD_ROWS    4            // set  2 for a 1602 panel
#define LCD_ADDR  0x27
#define INA_ADDR  0x40
// DS3231 is fixed at 0x68 inside RTClib.

/* ---- INA226 calibration (match ESP1) ------------------------------------- */
const float INA_SHUNT_OHMS    = 0.002f;   // shunt resistor value      [MEASURE]
const float INA_MAX_CURRENT_A = 10.0f;    // expected max current      [MEASURE]

/* ---- Phase timing -------------------------------------------------------- */
const unsigned long STATUS_MS = 5000;     // status screen duration
const unsigned long CAT_MS    = 10000;    // running-cat duration
const unsigned long STATUS_REFRESH_MS = 500;
const unsigned long CAT_FRAME_MS      = 180;

/* ---- Objects ------------------------------------------------------------- */
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
RTC_DS3231        rtc;
INA226            ina(INA_ADDR);

bool rtcReady = false;
bool inaReady = false;

void drawStatus();
void drawCat();

enum Phase { PHASE_STATUS, PHASE_CAT };
Phase phase = PHASE_STATUS;
unsigned long phaseStart = 0;
unsigned long lastFrame  = 0;
int  catX        = -8;          // cat entry position (off the left edge)
bool catBlink    = false;

/* ---- The cat: backslash-free on purpose. The Arduino IDE preprocessor can
 *      mis-parse a backslash inside a string literal, so the classic cat face
 *      is replaced with an equals/caret face that needs no escapes. ---------- */
const char *CAT_FACE  = "~(=^.^=)";    // tail + face, eyes open
const char *CAT_BLINK = "~(=-.-=)";    // blink frame
const char *CAT_FEET1 = "  u   u ";    // running legs, frame A
const char *CAT_FEET2 = "   u u  ";    // running legs, frame B

/* =============================================================================
 *  HELPERS
 * ========================================================================== */
// Print a string onto `row` at horizontal offset `x`, clipped to the panel and
// padded with spaces so the previous frame is fully overwritten (no ghosting).
void putAt(uint8_t row, const char *s, int x) {
  char line[LCD_COLS + 1];
  for (uint8_t i = 0; i < LCD_COLS; i++) line[i] = ' ';
  line[LCD_COLS] = '\0';
  int len = (int)strlen(s);
  for (int i = 0; i < len; i++) {
    int c = x + i;
    if (c >= 0 && c < LCD_COLS) line[c] = s[i];
  }
  lcd.setCursor(0, row);
  lcd.print(line);
}

// Print a left-justified, space-padded line (clean overwrite of a whole row).
void printRow(uint8_t row, const String &text) {
  String t = text;
  while (t.length() < LCD_COLS) t += ' ';
  lcd.setCursor(0, row);
  lcd.print(t.substring(0, LCD_COLS));
}

String two(int v) { return (v < 10) ? "0" + String(v) : String(v); }

/* =============================================================================
 *  SETUP
 * ========================================================================== */
void setup() {
  Serial.begin(9600);
  Wire.begin();                 // Nano I2C master on A4 (SDA) / A5 (SCL)

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("I2C debug boot...");

  // ---- RTC ----
  rtcReady = rtc.begin();
  if (rtcReady) {
    Serial.println(F("RTC DS3231 @0x68: READY"));
    if (rtc.lostPower()) {
      Serial.println(F("  WARN: RTC lost power -- time not set (replace CR2032)."));
      // seed a sane time so the display isn't 2000-00-00 while bench testing
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  } else {
    Serial.println(F("RTC DS3231 @0x68: NOT FOUND"));
  }

  // ---- INA226 ----
  inaReady = ina.begin();
  if (inaReady) {
    ina.setMaxCurrentShunt(INA_MAX_CURRENT_A, INA_SHUNT_OHMS);
    Serial.println(F("INA226 @0x40: READY"));
  } else {
    Serial.println(F("INA226 @0x40: NOT FOUND"));
  }

  phase      = PHASE_STATUS;
  phaseStart = millis();
  lastFrame  = 0;
  lcd.clear();
}

/* =============================================================================
 *  LOOP  --  STATUS (5 s)  ->  CAT (10 s)  ->  repeat
 * ========================================================================== */
void loop() {
  unsigned long now = millis();

  if (phase == PHASE_STATUS) {
    if (now - lastFrame >= STATUS_REFRESH_MS || lastFrame == 0) {
      lastFrame = now;
      drawStatus();
    }
    if (now - phaseStart >= STATUS_MS) {            // -> switch to cat
      phase = PHASE_CAT;
      phaseStart = now;
      lastFrame = 0;
      catX = -8;
      lcd.clear();
      Serial.println(F("---> running cat"));
    }
  }

  else { // PHASE_CAT
    if (now - lastFrame >= CAT_FRAME_MS || lastFrame == 0) {
      lastFrame = now;
      drawCat();
    }
    if (now - phaseStart >= CAT_MS) {               // -> back to status
      phase = PHASE_STATUS;
      phaseStart = now;
      lastFrame = 0;
      lcd.clear();
      Serial.println(F("---> status"));
    }
  }
}

/* =============================================================================
 *  STATUS SCREEN  (live RTC time + INA226 bus voltage)
 * ========================================================================== */
void drawStatus() {
  // Row 0: RTC
  if (rtcReady) {
    DateTime t = rtc.now();
    printRow(0, "RTC is ready " + two(t.hour()) + ":" + two(t.minute()) + ":" + two(t.second()));
  } else {
    printRow(0, "RTC: NOT FOUND");
  }

  // Row 1: INA226
  if (inaReady) {
    float v = ina.getBusVoltage();            // volts
    printRow(1, "INA226 is ready " + String(v, 2) + "V");
  } else {
    printRow(1, "INA226: NOT FOUND");
  }

  if (LCD_ROWS >= 4) {
    float i = inaReady ? ina.getCurrent() : 0.0f;   // amps
    printRow(2, inaReady ? ("I=" + String(i, 3) + "A") : String(""));
    printRow(3, "LCD0x27 INA0x40 RTC68");
  }

  // mirror to serial
  Serial.print(F("[STATUS] RTC="));
  Serial.print(rtcReady ? "ready" : "--");
  Serial.print(F("  INA="));
  Serial.println(inaReady ? "ready" : "--");
}

/* =============================================================================
 *  RUNNING CAT  (scrolls left -> right, wraps, blinks)
 * ========================================================================== */
void drawCat() {
  bool    four  = (LCD_ROWS >= 4);
  uint8_t rFace = four ? 1 : 0;     // face row
  uint8_t rFeet = four ? 2 : 1;     // legs row (shares the 2nd row on a 1602)

  // face (with trailing tail) and alternating running legs -- one putAt per row
  putAt(rFace, catBlink ? CAT_BLINK : CAT_FACE, catX);
  putAt(rFeet, ((catX & 1) == 0) ? CAT_FEET1 : CAT_FEET2, catX);

  if (four) {
    printRow(0, "  ...running cat...");
    printRow(3, "RTC0x68 INA0x40 LCD27");
  }

  // advance across the panel, wrap, blink occasionally
  catX++;
  if (catX > LCD_COLS) catX = -8;   // re-enter from the left
  static uint8_t fc = 0;
  if (++fc % 6 == 0) catBlink = !catBlink;
}
