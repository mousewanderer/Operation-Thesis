// =====================================================================
//  STEP 4a — ESP32-A alone : keypad + LCD. No ESP32-B, no link.
//
//  One board, two parts, nothing else to go wrong.
//
//  And it is not a toy: this is the real data-entry flow from the
//  finished machine. You type the target ratio and the batch mass, and
//  it shows you what you asked for. Later the same screens will send
//  those numbers to ESP32-B. For now they just sit there.
//
//  LIBRARIES:  "Keypad"  and  "LiquidCrystal I2C"
//
//  WIRING
//      Keypad ROW1..4 -> GPIO  4, 16, 17, 13
//      Keypad COL1..4 -> GPIO 18, 19, 21, 14
//      LCD SDA -> GPIO 22        LCD VCC -> 3V3
//      LCD SCL -> GPIO 23        LCD GND -> GND
//
//  Start the LCD at 3.3 V. At 5 V the backpack pulls the two signal
//  wires to 5 V and the ESP32 pins are only rated for 3.3 V.
//
//  BLANK SCREEN, OR A ROW OF WHITE BLOCKS?
//    Turn the little blue screw on the back of the backpack. That is the
//    contrast, and it is almost always the answer. If the Serial Monitor
//    shows no I2C addresses at all, it is the wiring instead.
//
//  KEYS
//      0-9   type a digit
//      *     clear what you typed
//      #     accept and go to the next question
//      A     go back one question
//      D     start over
// =====================================================================
#include <Arduino.h>
#include <Wire.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>

#define PIN_SDA  22
#define PIN_SCL  23

// *** SET THESE TO MATCH YOUR ACTUAL SCREEN ***
// Count the character positions on the glass:
//   16 wide x 2 rows  -> 16 and 2      (the usual one)
//   20 wide x 4 rows  -> 20 and 4      (the bigger "2004" one)
// The example that came with your library used 20 and 4. That is just
// what YWROBOT shipped, not necessarily what you have. Count it.
#define LCD_COLS 16
#define LCD_ROWS 2

// ------------------------------------------------------------ keypad ---
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

// --------------------------------------------------------------- LCD ---
// The address is unknown until we scan, so the object is built in
// setup(). This is the one place allocating memory is fine: once, at
// boot, never inside a loop.
// ---------------------------------------------------- LCD start-up ---
// There are several libraries all called "LiquidCrystal I2C" and they do
// not agree on how you start the screen. Some want lcd.init(), others
// want lcd.begin(16,2), a few want lcd.begin(). Rather than make you
// hunt for the matching one, these three lines let the compiler pick
// whichever your library actually has.
//
// If you would rather keep it simple: delete this block and call the one
// your library has directly.
struct LcdP2 {};
struct LcdP1 : LcdP2 {};
struct LcdP0 : LcdP1 {};
template <class T> auto lcdStart(T* p, LcdP0) -> decltype(p->init(), void())        { p->init(); }
template <class T> auto lcdStart(T* p, LcdP1) -> decltype(p->begin(LCD_COLS, LCD_ROWS), void()) { p->begin(LCD_COLS, LCD_ROWS); }
template <class T> auto lcdStart(T* p, LcdP2) -> decltype(p->begin(), void())       { p->begin(); }

LiquidCrystal_I2C* lcd = nullptr;
bool lcdOk = false;

// ------------------------------------------------------------- state ---
// A tiny state machine. Which question are we on?
enum { ASK_N = 0, ASK_P, ASK_K, ASK_MASS, SHOW_SUMMARY };
uint8_t step = ASK_N;

int   valN = 0, valP = 0, valK = 0, valMass = 0;

char    entry[8];              // the digits typed so far
uint8_t entryN = 0;

char     flashMsg[17] = "";    // a short message, shown for a moment
uint32_t flashUntil = 0;

// ---------------------------------------------------------- I2C scan ---
uint8_t scanI2C() {
  uint8_t found = 0;
  Serial.println("\nscanning I2C...");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device at 0x%02X", a);
      if (a == 0x27 || a == 0x3F) { Serial.print("  <- the LCD"); if (!found) found = a; }
      if (a == 0x68)              Serial.print("  <- DS3231 clock");
      Serial.println();
    }
  }
  if (!found) {
    Serial.println("  nothing found. Check SDA=22, SCL=23, VCC and GND.");
  }
  return found;
}

// -------------------------------------------------------------- draw ---
// Write to the screen only when the text really changed. Rewriting the
// same line many times a second makes it flicker, and I2C is not fast.
char shown[LCD_ROWS][21];

void lcdLine(uint8_t row, const char* text) {
  if (row >= LCD_ROWS) return;
  char padded[21];
  snprintf(padded, sizeof(padded), "%-*s", LCD_COLS, text);
  if (strcmp(padded, shown[row]) == 0) return;      // no change, skip
  strcpy(shown[row], padded);
  if (!lcdOk) return;
  lcd->setCursor(0, row);
  lcd->print(padded);
}

void say(const char* msg) {
  snprintf(flashMsg, sizeof(flashMsg), "%s", msg);
  flashUntil = millis() + 1200;
}

void draw() {
  char top[20], bottom[20];

  switch (step) {
    case ASK_N:    snprintf(top, sizeof(top), "Target N ratio");  break;
    case ASK_P:    snprintf(top, sizeof(top), "Target P ratio");  break;
    case ASK_K:    snprintf(top, sizeof(top), "Target K ratio");  break;
    case ASK_MASS: snprintf(top, sizeof(top), "Batch mass (g)");  break;
    default:       snprintf(top, sizeof(top), "%d-%d-%d", valN, valP, valK); break;
  }
  lcdLine(0, top);

  if (millis() < flashUntil) { lcdLine(1, flashMsg); return; }

  if (step == SHOW_SUMMARY) snprintf(bottom, sizeof(bottom), "%d g   D=again", valMass);
  else if (entryN)          snprintf(bottom, sizeof(bottom), "> %s", entry);
  else                      snprintf(bottom, sizeof(bottom), "> _   #=next");
  lcdLine(1, bottom);

  // A 20x4 screen has room to show your progress and the key help all
  // the time. On a 16x2 these two calls simply do nothing.
  if (LCD_ROWS >= 4) {
    char l3[24];
    snprintf(l3, sizeof(l3), "N%d P%d K%d  %dg", valN, valP, valK, valMass);
    lcdLine(2, l3);
    lcdLine(3, "* clr # next A back");
  }
}

// -------------------------------------------------------------- keys ---
void acceptEntry() {
  int v = atoi(entry);
  switch (step) {
    case ASK_N:    valN = v; step = ASK_P;    break;
    case ASK_P:    valP = v; step = ASK_K;    break;
    case ASK_K:    valK = v; step = ASK_MASS; break;
    case ASK_MASS: valMass = v; step = SHOW_SUMMARY;
                   Serial.printf("ENTERED  %d-%d-%d   %d g\n", valN, valP, valK, valMass);
                   break;
  }
  entryN = 0; entry[0] = 0;
}

void onKey(char k) {
  Serial.printf("key %c\n", k);

  if (k == 'D') {                       // start over
    step = ASK_N; entryN = 0; entry[0] = 0;
    valN = valP = valK = valMass = 0;
    say("start over");
    return;
  }

  if (k == 'A') {                       // back one question
    if (step > ASK_N) step--;
    entryN = 0; entry[0] = 0;
    say("back");
    return;
  }

  if (step == SHOW_SUMMARY) {           // only D and A do anything here
    say("D = again");
    return;
  }

  if (k >= '0' && k <= '9') {
    if (entryN < sizeof(entry) - 1) { entry[entryN++] = k; entry[entryN] = 0; }
    return;
  }

  if (k == '*') { entryN = 0; entry[0] = 0; say("cleared"); return; }

  if (k == '#') {
    if (entryN == 0) { say("type a number"); return; }
    acceptEntry();
    return;
  }
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  keypad.setDebounceTime(40);
  delay(400);

  for (uint8_t r = 0; r < LCD_ROWS; r++) shown[r][0] = 0;
  Wire.begin(PIN_SDA, PIN_SCL);
  uint8_t addr = scanI2C();
  if (addr) {
    lcd = new LiquidCrystal_I2C(addr, LCD_COLS, LCD_ROWS);
    lcdStart(lcd, LcdP0{});
    lcd->backlight();
    lcd->clear();
    lcdOk = true;
    Serial.printf("LCD at 0x%02X\n", addr);
  } else {
    Serial.println("no LCD — the keypad still prints here");
  }

  Serial.println("\nSTEP 4a — keypad + LCD");
  Serial.println("0-9 digits, * clear, # next, A back, D start over");
  say("AgriDose");
}

void loop() {
  // getKey() must be called often, so never put delay() in here
  char k = keypad.getKey();
  if (k) onKey(k);
  draw();
}
