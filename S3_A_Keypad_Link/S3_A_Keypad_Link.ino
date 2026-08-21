// =====================================================================
//  STEP 3 — ESP32-A : keypad + link, working together.
//
//  This is your keypad sketch and your link sketch joined into one.
//  Press a key on A and it appears on B's screen. That is the whole goal.
//
//  *** MOVE ONE WIRE. THAT IS ALL. ***
//  Your keypad stays exactly where it is, except row 4:
//
//      GPIO 5  ->  GPIO 13
//
//  Why: GPIO 5 is a "strapping" pin. The chip reads its level while
//  booting to decide how to start. A key held down at power-on could
//  drag it and stop the board booting. GPIO 13 is plain and safe.
//
//  Everything else on the board moves around the keypad instead:
//  the SD card and the LCD/clock go on the pins that are left over.
//
//  WIRING
//      ROW1 -> GPIO  4        COL1 -> GPIO 18
//      ROW2 -> GPIO 16        COL2 -> GPIO 19
//      ROW3 -> GPIO 17        COL3 -> GPIO 21
//      ROW4 -> GPIO 13        COL4 -> GPIO 14
//
//      A GPIO27 (TX) -> B GPIO16 (RX)
//      A GPIO26 (RX) <- B GPIO17 (TX)
//      A GND         <-> B GND
//
//  A and B use different UART pins on purpose. Only the wire matters.
//  B has to keep 16/17 because 25/26/27 are its feeder motor pins.
//
//  ESP32-B keeps running the Step 1 sketch (with IS_BOARD_A set to 0).
//
//  WHAT THE KEYS DO HERE
//      0-9   build up a number, shown as you type
//      *     clear the number
//      #     send the number to ESP32-B
//      A-D   sent straight across as a single key press
//
//  This is a small rehearsal of the real thing, where you will type a
//  target ratio and a batch mass the same way.
// =====================================================================
#include <Arduino.h>
#include <Keypad.h>

// ---------------------------------------------------------- keypad ---
const byte ROWS = 4, COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = { 4, 16, 17, 13};   // read  (internal pull-ups)
byte colPins[COLS] = {18, 19, 21, 14};   // driven

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ------------------------------------------------------------ link ---
#define PIN_RX       26        // on ESP32-A. B uses 16/17 — that is fine.
#define PIN_TX       27
#define HEARTBEAT_MS 500
#define LINK_DEAD_MS 1500

char     rxBuf[64];
uint8_t  rxN     = 0;
bool     inFrame = false;
uint32_t goodCount = 0, badCount = 0, lastRx = 0, lastBeat = 0;
bool     linkUp = false;

// what the operator is typing
char     entry[12];
uint8_t  entryN = 0;

void sendFrame(const char* payload) {
  uint8_t cs = 0;
  for (const char* p = payload; *p; ++p) cs ^= (uint8_t)*p;
  Serial2.printf("#%s*%02X\n", payload, cs);
}

void handlePayload(char* p) {
  if (p[0] == 'H' && p[1] == 'B') return;     // heartbeat, stay quiet
  Serial.print("FROM B: ");
  Serial.println(p);
}

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
      if (cs != want) { badCount++; Serial.println("!! bad checksum, thrown away"); continue; }
      goodCount++;
      lastRx = millis();
      handlePayload(rxBuf);
      continue;
    }
    if (rxN < sizeof(rxBuf) - 1) rxBuf[rxN++] = c;
    else { inFrame = false; rxN = 0; }
  }
}

// ------------------------------------------------------------- keys ---
void showEntry() {
  Serial.print("  typing: ");
  Serial.println(entryN ? entry : "(empty)");
}

void onKey(char k) {
  Serial.printf("KEY %c\n", k);

  if (k >= '0' && k <= '9') {
    if (entryN < sizeof(entry) - 1) { entry[entryN++] = k; entry[entryN] = 0; }
    showEntry();
    return;
  }

  if (k == '*') {                       // clear
    entryN = 0; entry[0] = 0;
    Serial.println("  cleared");
    return;
  }

  if (k == '#') {                       // send the number
    if (entryN == 0) { Serial.println("  nothing to send"); return; }
    char msg[24];
    snprintf(msg, sizeof(msg), "NUM,%s", entry);
    sendFrame(msg);
    Serial.printf("  sent to B: %s\n", msg);
    entryN = 0; entry[0] = 0;
    return;
  }

  // A B C D go across on their own
  char msg[16];
  snprintf(msg, sizeof(msg), "KEY,%c", k);
  sendFrame(msg);
  Serial.printf("  sent to B: %s\n", msg);
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, PIN_RX, PIN_TX);   // (baud, config, RX, TX)
  keypad.setDebounceTime(40);
  delay(300);
  Serial.println("\nESP32-A — keypad + link");
  Serial.println("0-9 type a number, * clear, # send, A-D send at once");
  lastRx = millis();
}

void loop() {
  // 1 --- the keypad. getKey() must be called often, so keep loop() fast
  //       and never put delay() in here.
  char k = keypad.getKey();
  if (k) onKey(k);

  // 2 --- anything arriving from ESP32-B
  pollLink();

  // 3 --- heartbeat
  if (millis() - lastBeat >= HEARTBEAT_MS) {
    lastBeat = millis();
    char hb[24];
    snprintf(hb, sizeof(hb), "HB,A,%lu", (unsigned long)(millis() / 1000));
    sendFrame(hb);
  }

  // 4 --- notice if B goes quiet
  bool up = (millis() - lastRx) < LINK_DEAD_MS;
  if (up != linkUp) {
    linkUp = up;
    if (up) Serial.printf("== LINK OK ==  (good %lu, bad %lu)\n",
                          (unsigned long)goodCount, (unsigned long)badCount);
    else    Serial.println("== LINK LOST ==  ESP32-B is not answering");
  }
}
