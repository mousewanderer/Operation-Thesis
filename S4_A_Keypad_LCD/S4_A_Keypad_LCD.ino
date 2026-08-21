// =====================================================================
//  STEP 4 — ESP32-A : keypad + link + LCD.
//
//  Step 3 plus a screen. What you type now shows on the LCD instead of
//  only in the Serial Monitor, and the top line tells you whether
//  ESP32-B is alive.
//
//  LIBRARY:  "LiquidCrystal I2C"  (Library Manager, by Frank de Brabander)
//
//  WIRING — the LCD needs only two signal wires
//      LCD SDA -> GPIO 22
//      LCD SCL -> GPIO 23
//      LCD VCC -> 3V3      (read the note below before using 5V)
//      LCD GND -> GND
//
//  The DS3231 clock, when you add it, goes on the SAME two wires.
//  That is what I2C is: one shared pair, many devices, each with its
//  own address. The LCD answers to 0x27 or 0x3F, the clock to 0x68.
//
//  *** POWER: START AT 3.3 V ***
//  Most of these backpacks run at 3.3 V. The screen is a bit dimmer but
//  nothing can be damaged. If you power it from 5 V, the backpack pulls
//  the two signal wires up to 5 V, and the ESP32's pins are only rated
//  for 3.3 V. It usually survives. "Usually" is not a word you want in
//  a machine you have to demonstrate in November. If 3.3 V is too dim
//  even with the contrast turned up, use 5 V with a 2-channel I2C level
//  shifter (about P50).
//
//  *** IF THE SCREEN IS BLANK OR SHOWS ONLY WHITE BLOCKS ***
//    1. Turn the little blue screw on the back of the backpack. That is
//       the contrast. It is almost always this.
//    2. Check the Serial Monitor. This sketch scans the I2C bus at boot
//       and prints every address it finds. No addresses = a wiring
//       problem, not a code problem.
// =====================================================================
#include <Arduino.h>
#include <Wire.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>

// ------------------------------------------------------------- pins ---
#define PIN_SDA   22
#define PIN_SCL   23
#define PIN_RX    26        // link to ESP32-B (B uses 16/17, that is fine)
#define PIN_TX    27

const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = { 4, 16, 17, 13};
byte colPins[COLS] = {18, 19, 21, 14};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// The address is not known until we scan, so the screen object is made
// in setup(). This is the one place where allocating memory is fine —
// it happens once, at boot, never in a loop.
LiquidCrystal_I2C* lcd = nullptr;
bool lcdFound = false;

// ------------------------------------------------------------- link ---
#define HEARTBEAT_MS 500
#define LINK_DEAD_MS 1500
char     rxBuf[64];
uint8_t  rxN = 0;
bool     inFrame = false;
uint32_t goodCount = 0, badCount = 0, lastRx = 0, lastBeat = 0;
bool     linkUp = false;

char     entry[12];
uint8_t  entryN = 0;
char     flash[17] = "";          // short message on line 2
uint32_t flashUntil = 0;

void sendFrame(const char* payload) {
  uint8_t cs = 0;
  for (const char* p = payload; *p; ++p) cs ^= (uint8_t)*p;
  Serial2.printf("#%s*%02X\n", payload, cs);
}

// --------------------------------------------------------- I2C scan ---
// Ask every possible address whether anything is listening.
uint8_t scanI2C() {
  uint8_t lcdAddr = 0;
  Serial.println("\nscanning I2C bus...");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  found a device at 0x%02X", a);
      if (a == 0x27 || a == 0x3F) { Serial.print("   <- looks like the LCD"); if (!lcdAddr) lcdAddr = a; }
      if (a == 0x68)              Serial.print("   <- looks like the DS3231 clock");
      Serial.println();
    }
  }
  if (!lcdAddr) {
    Serial.println("  no LCD found.");
    Serial.println("  check: SDA on 22, SCL on 23, VCC and GND connected.");
  }
  return lcdAddr;
}

// ------------------------------------------------------------ screen ---
// Only write to the screen when the text actually changes. Rewriting a
// line 20 times a second makes it flicker and it is slow.
char shown[2][17] = {"", ""};

void lcdLine(uint8_t row, const char* text) {
  char padded[17];
  snprintf(padded, sizeof(padded), "%-16s", text);
  if (strcmp(padded, shown[row]) == 0) return;      // no change, skip
  strcpy(shown[row], padded);
  if (!lcdFound) return;
  lcd->setCursor(0, row);
  lcd->print(padded);
}

void draw() {
  char top[20];
  snprintf(top, sizeof(top), "AgriDose    %s", linkUp ? "[OK]" : "[--]");
  lcdLine(0, top);

  if (millis() < flashUntil) { lcdLine(1, flash); return; }

  char bottom[20];
  if (entryN) snprintf(bottom, sizeof(bottom), "> %s", entry);
  else        snprintf(bottom, sizeof(bottom), "0-9 * clr  # ok");
  lcdLine(1, bottom);
}

void say(const char* msg) {
  snprintf(flash, sizeof(flash), "%s", msg);
  flashUntil = millis() + 1500;
}

// -------------------------------------------------------------- keys ---
void onKey(char k) {
  Serial.printf("KEY %c\n", k);

  if (k >= '0' && k <= '9') {
    if (entryN < sizeof(entry) - 1) { entry[entryN++] = k; entry[entryN] = 0; }
    return;
  }
  if (k == '*') { entryN = 0; entry[0] = 0; say("cleared"); return; }

  if (k == '#') {
    if (entryN == 0) { say("nothing to send"); return; }
    char msg[24];
    snprintf(msg, sizeof(msg), "NUM,%s", entry);
    sendFrame(msg);
    char note[20];
    snprintf(note, sizeof(note), "sent %s", entry);
    say(note);
    entryN = 0; entry[0] = 0;
    return;
  }

  char msg[16];
  snprintf(msg, sizeof(msg), "KEY,%c", k);
  sendFrame(msg);
  char note[20];
  snprintf(note, sizeof(note), "sent key %c", k);
  say(note);
}

// -------------------------------------------------------------- link ---
void pollLink() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '#') { inFrame = true; rxN = 0; continue; }
    if (!inFrame) continue;
    if (c == '\n' || c == '\r') {
      inFrame = false;
      if (rxN < 4) continue;
      rxBuf[rxN] = 0;
      char* star = strrchr(rxBuf, '*');
      if (!star) { badCount++; continue; }
      *star = 0;
      uint8_t want = (uint8_t)strtoul(star + 1, NULL, 16), cs = 0;
      for (char* q = rxBuf; *q; ++q) cs ^= (uint8_t)*q;
      if (cs != want) { badCount++; continue; }
      goodCount++;
      lastRx = millis();
      if (!(rxBuf[0] == 'H' && rxBuf[1] == 'B')) {
        Serial.print("FROM B: "); Serial.println(rxBuf);
      }
      continue;
    }
    if (rxN < sizeof(rxBuf) - 1) rxBuf[rxN++] = c;
    else { inFrame = false; rxN = 0; }
  }
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, PIN_RX, PIN_TX);
  keypad.setDebounceTime(40);
  delay(400);

  Wire.begin(PIN_SDA, PIN_SCL);
  uint8_t addr = scanI2C();
  if (addr) {
    lcd = new LiquidCrystal_I2C(addr, 16, 2);
    lcd->init();
    lcd->backlight();
    lcd->clear();
    lcdFound = true;
    Serial.printf("using LCD at 0x%02X\n", addr);
  } else {
    Serial.println("running without a screen — keypad still works on serial");
  }

  Serial.println("\nESP32-A: keypad + link + LCD");
  lastRx = millis();
  say("ready");
}

void loop() {
  char k = keypad.getKey();
  if (k) onKey(k);

  pollLink();

  if (millis() - lastBeat >= HEARTBEAT_MS) {
    lastBeat = millis();
    char hb[24];
    snprintf(hb, sizeof(hb), "HB,A,%lu", (unsigned long)(millis() / 1000));
    sendFrame(hb);
  }

  bool up = (millis() - lastRx) < LINK_DEAD_MS;
  if (up != linkUp) {
    linkUp = up;
    Serial.println(up ? "== LINK OK ==" : "== LINK LOST ==");
  }

  draw();      // cheap: it only touches the screen when the text changed
}
