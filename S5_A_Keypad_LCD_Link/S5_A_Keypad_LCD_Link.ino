// =====================================================================
//  STEP 5 — ESP32-A : keypad + LCD + link to ESP32-B.
//
//  Your working Step 4a sketch, unchanged, with the link added. Nothing
//  about the keypad or the screen was touched — if 4a worked, this works.
//
//  Now the numbers you type actually go somewhere. Enter N, P, K and the
//  batch mass, then press # on the summary screen and the whole recipe
//  is sent to ESP32-B.
//
//  LIBRARIES:  "Keypad"  and  "LiquidCrystal I2C"
//
//  WIRING — same as before, plus three wires
//      Keypad ROW1..4 -> GPIO  4, 16, 17, 5
//      Keypad COL1..4 -> GPIO 18, 19, 21, 14
//      LCD SDA -> GPIO 22        LCD SCL -> GPIO 23
//
//      A GPIO 27 (TX) -> B GPIO 16 (RX)
//      A GPIO 26 (RX) <- B GPIO 17 (TX)
//      A GND          <-> B GND        <-- do not skip this wire
//
//  A and B use different pins on purpose. Only the wire matters. B has
//  to keep 16/17 because 25/26/27 are its motor pins later.
//
//  ON ESP32-B: flash S1_Link_Framed with  #define IS_BOARD_A  0
//
//  WHAT THE TOP-RIGHT CHARACTER MEANS
//      *   ESP32-B is answering
//      !   nothing heard from B for 1.5 seconds
//
//  KEYS
//      0-9   type a digit
//      *     clear what you typed
//      #     accept and go to the next question
//            (on the summary screen: send the recipe to ESP32-B)
//      A     go back one question
//      D     start over
// =====================================================================
#include <Arduino.h>
#include <Wire.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>

#define PIN_SDA  22
#define PIN_SCL  23

// --- the link ---
#define PIN_LINK_RX   26        // o
#define PIN_LINK_TX   27
#define LINK_BAUD     115200
#define HEARTBEAT_MS  500       // how often we say "still here"
#define LINK_DEAD_MS  1500      // silence longer than this = link is down

// *** SET THESE TO MATCH YOUR ACTUAL SCREEN ***
//   16 wide x 2 rows  -> 16 and 2      (the usual one)
//   20 wide x 4 rows  -> 20 and 4      (the bigger "2004" one)
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
byte rowPins[ROWS] = { 4, 16, 17, 5};
byte colPins[COLS] = {18, 19, 21, 14};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ---------------------------------------------------- LCD start-up ---
// Several libraries are called "LiquidCrystal I2C" and they disagree on
// how you start the screen. These three lines let the compiler pick
// whichever your library actually has.
struct LcdP2 {};
struct LcdP1 : LcdP2 {};
struct LcdP0 : LcdP1 {};
template <class T> auto lcdStart(T* p, LcdP0) -> decltype(p->init(), void())        { p->init(); }
template <class T> auto lcdStart(T* p, LcdP1) -> decltype(p->begin(LCD_COLS, LCD_ROWS), void()) { p->begin(LCD_COLS, LCD_ROWS); }
template <class T> auto lcdStart(T* p, LcdP2) -> decltype(p->begin(), void())       { p->begin(); }

LiquidCrystal_I2C* lcd = nullptr;
bool lcdOk = false;

// ------------------------------------------------------------- state ---
enum { ASK_N = 0, ASK_P, ASK_K, ASK_MASS, SHOW_SUMMARY };
uint8_t step = ASK_N;

int   valN = 0, valP = 0, valK = 0, valMass = 0;

char    entry[8];
uint8_t entryN = 0;

char     flashMsg[21] = "";
uint32_t flashUntil = 0;

// -------------------------------------------------------- link state ---
char     rxBuf[64];
uint8_t  rxN = 0;
bool     inFrame = false;
uint32_t goodCount = 0, badCount = 0;
uint32_t lastRx = 0, lastBeat = 0;
bool     linkUp = false;

// =====================================================================
//  LINK
//  Every message goes out as  #PAYLOAD*XX  where XX is all the payload
//  characters XOR-ed together. If noise flips one character the sum will
//  not match and we throw the message away instead of obeying it.
//  A garbled "DOSE,0,200" that arrives as "DOSE,0,2000" is ten times too
//  much feedstock on the floor.
// =====================================================================
void sendFrame(const char* payload) {
  uint8_t cs = 0;
  for (const char* p = payload; *p; ++p) cs ^= (uint8_t)*p;
  Serial2.printf("#%s*%02X\n", payload, cs);
}

void say(const char* msg);          // forward declaration

void handlePayload(char* p) {
  if (p[0] == 'H' && p[1] == 'B') return;      // heartbeat, nothing to show
  Serial.print("FROM B: ");
  Serial.println(p);
  char note[21];
  snprintf(note, sizeof(note), "B: %.*s", LCD_COLS - 3, p);
  say(note);
}

void pollLink() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();

    if (c == '#') { inFrame = true; rxN = 0; continue; }   // a frame starts
    if (!inFrame) continue;                                 // junk, ignore

    if (c == '\n' || c == '\r') {                           // a frame ends
      inFrame = false;
      if (rxN < 4) continue;
      rxBuf[rxN] = 0;

      char* star = strrchr(rxBuf, '*');
      if (!star) { badCount++; continue; }
      *star = 0;

      uint8_t want = (uint8_t)strtoul(star + 1, NULL, 16);
      uint8_t cs = 0;
      for (char* q = rxBuf; *q; ++q) cs ^= (uint8_t)*q;

      if (cs != want) { badCount++; Serial.println("!! bad checksum, thrown away"); continue; }

      goodCount++;
      lastRx = millis();
      handlePayload(rxBuf);
      continue;
    }

    if (rxN < sizeof(rxBuf) - 1) rxBuf[rxN++] = c;
    else { inFrame = false; rxN = 0; }                      // too long, drop it
  }
}

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
  if (!found) Serial.println("  nothing found. Check SDA=22, SCL=23, VCC and GND.");
  return found;
}

// -------------------------------------------------------------- draw ---
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
  char question[20], top[24], bottom[24];

  switch (step) {
    case ASK_N:    snprintf(question, sizeof(question), "Target N ratio"); break;
    case ASK_P:    snprintf(question, sizeof(question), "Target P ratio"); break;
    case ASK_K:    snprintf(question, sizeof(question), "Target K ratio"); break;
    case ASK_MASS: snprintf(question, sizeof(question), "Batch mass (g)"); break;
    default:       snprintf(question, sizeof(question), "%d-%d-%d", valN, valP, valK); break;
  }
  // last character on the top line is the link light: * alive, ! silent
  snprintf(top, sizeof(top), "%-*.*s%c", LCD_COLS - 1, LCD_COLS - 1, question, linkUp ? '*' : '!');
  lcdLine(0, top);

  if (millis() < flashUntil) { lcdLine(1, flashMsg); return; }

  if (step == SHOW_SUMMARY) snprintf(bottom, sizeof(bottom), "%d g  #=send", valMass);
  else if (entryN)          snprintf(bottom, sizeof(bottom), "> %s", entry);
  else                      snprintf(bottom, sizeof(bottom), "> _   #=next");
  lcdLine(1, bottom);

  if (LCD_ROWS >= 4) {
    char l3[48];
    snprintf(l3, sizeof(l3), "N%d P%d K%d  %dg", valN, valP, valK, valMass);
    lcdLine(2, l3);
    snprintf(l3, sizeof(l3), "link %s ok%lu bad%lu",
             linkUp ? "UP" : "DOWN", (unsigned long)goodCount, (unsigned long)badCount);
    lcdLine(3, l3);
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

void sendRecipe() {
  if (!linkUp) { say("no link to B"); return; }
  char msg[40];
  snprintf(msg, sizeof(msg), "SET,%d,%d,%d,%d", valN, valP, valK, valMass);
  sendFrame(msg);
  Serial.printf("sent to B: %s\n", msg);
  say("sent to B");
}

void onKey(char k) {
  Serial.printf("key %c\n", k);

  if (k == 'D') {
    step = ASK_N; entryN = 0; entry[0] = 0;
    valN = valP = valK = valMass = 0;
    say("start over");
    return;
  }

  if (k == 'A') {
    if (step > ASK_N) step--;
    entryN = 0; entry[0] = 0;
    say("back");
    return;
  }

  if (step == SHOW_SUMMARY) {
    if (k == '#') sendRecipe();          // <-- the new bit
    else          say("# send, D again");
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
  Serial2.begin(LINK_BAUD, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);  // (baud, config, RX, TX)
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

  Serial.println("\nSTEP 5 — keypad + LCD + link");
  Serial.println("0-9 digits, * clear, # next, A back, D start over");
  Serial.println("on the summary screen, # sends the recipe to ESP32-B");
  lastRx = millis();
  say("AgriDose");
}

void loop() {
  // getKey() must be called often, so never put delay() in here
  char k = keypad.getKey();
  if (k) onKey(k);

  pollLink();

  // heartbeat: say "still here" on a timer, even with nothing to say
  if (millis() - lastBeat >= HEARTBEAT_MS) {
    lastBeat = millis();
    char hb[24];
    snprintf(hb, sizeof(hb), "HB,A,%lu", (unsigned long)(millis() / 1000));
    sendFrame(hb);
  }

  // watchdog: notice when B stops answering
  bool up = (millis() - lastRx) < LINK_DEAD_MS;
  if (up != linkUp) {
    linkUp = up;
    Serial.printf(up ? "== LINK OK ==  (good %lu, bad %lu)\n" : "== LINK LOST ==\n",
                  (unsigned long)goodCount, (unsigned long)badCount);
  }

  draw();
}
