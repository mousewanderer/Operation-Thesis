// =====================================================================
//  STEP 3 — ESP32-B : receive, verify, and ANSWER BACK.
//
//  Your current B sketch is a plain echo test. It works, but it does
//  three things that will bite you later, so this replaces it:
//
//    1. It never answers. A is waiting for a reply to know the link is
//       alive, so A prints "== LINK LOST ==" forever even though the
//       wire is fine. B has to talk back.
//
//    2. readStringUntil() BLOCKS. If half a line arrives it sits there
//       for up to a full second doing nothing. A dosing loop that has
//       to read the scale every 12.5 ms cannot afford that. The reader
//       below never waits — it takes whatever bytes are ready and
//       returns.
//
//    3. It ignores the checksum. A is calculating one and sending it;
//       B is throwing it away. A corrupted "DOSE 250" that arrives as
//       "DOSE 2500" would be accepted.
//
//  *** MOVE THE UART TO 16/17. ***
//      Your B sketch has Serial2 on 26 and 27. Those two pins are
//      feeder motor pins in the pin map — GPIO 26 is feeder 1 reverse,
//      GPIO 27 is feeder 2 forward. Keep the UART there and you have
//      nowhere to plug two of the augers in.
//
//      A already has this right in its own header comment.
//
//  WIRING
//      A GPIO 27 (TX)  ->  B GPIO 16 (RX)
//      A GPIO 26 (RX)  <-  B GPIO 17 (TX)
//      A GND           <-> B GND          <-- not optional
//
//  WHAT YOU SHOULD SEE
//      Every 500 ms:   RX  HB,A,41
//      When you type:  RX  NUM,250      ->  answers ACK,NUM,250
//      A's console flips to "== LINK OK ==" and stays there.
// =====================================================================
#include <Arduino.h>

#define PIN_RX       26       // <-- was 26. 26/27 belong to the feeders.
#define PIN_TX       27       // <-- was 27.
#define HEARTBEAT_MS 500
#define LINK_DEAD_MS 1500

char     rxBuf[64];
uint8_t  rxN     = 0;
bool     inFrame = false;
uint32_t goodCount = 0, badCount = 0, lastRx = 0, lastBeat = 0;
uint32_t hbSeq   = 0;
bool     linkUp  = false;

// ------------------------------------------------------------ send ---
void sendFrame(const char* payload) {
  uint8_t cs = 0;
  for (const char* p = payload; *p; ++p) cs ^= (uint8_t)*p;
  Serial2.printf("#%s*%02X\n", payload, cs);
}

// --------------------------------------------------------- received ---
void handlePayload(char* p) {
  // ---- heartbeat from A: answer it. This is the missing piece. ----
  unsigned long seq;
  char who;
  if (sscanf(p, "HB,%c,%lu", &who, &seq) == 2) {
    char ack[24];
    snprintf(ack, sizeof(ack), "HBK,B,%lu", seq);   // echo A's number back
    sendFrame(ack);
    return;                                          // stay quiet on the console
  }

  // ---- a number the operator typed on A ----
  char num[16];
  if (sscanf(p, "NUM,%15s", num) == 1) {
    Serial.printf("FROM A -- number: %s\n", num);
    char ack[24];
    snprintf(ack, sizeof(ack), "ACK,NUM,%s", num);
    sendFrame(ack);
    return;
  }

  // ---- a single key ----
  char k;
  if (sscanf(p, "KEY,%c", &k) == 1) {
    Serial.printf("FROM A -- key: %c\n", k);
    char ack[16];
    snprintf(ack, sizeof(ack), "ACK,KEY,%c", k);
    sendFrame(ack);
    return;
  }

  Serial.print("FROM A -- unknown: ");
  Serial.println(p);
}

// ------------------------------------------------------------ poll ---
// Never blocks. Same shape as A's reader so both boards behave alike.
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

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, PIN_RX, PIN_TX);   // (baud, config, RX, TX)
  delay(300);
  Serial.println("\nESP32-B — link receiver");
  Serial.println("waiting for ESP32-A...");
  lastRx = millis();
}

void loop() {
  pollLink();

  // B sends its own heartbeat too, so the link is proven in both
  // directions. Note the counter: it goes up by one every single beat.
  if (millis() - lastBeat >= HEARTBEAT_MS) {
    lastBeat = millis();
    char hb[24];
    snprintf(hb, sizeof(hb), "HB,B,%lu", (unsigned long)(++hbSeq));
    sendFrame(hb);
  }

  bool up = (millis() - lastRx) < LINK_DEAD_MS;
  if (up != linkUp) {
    linkUp = up;
    if (up) Serial.printf("== LINK OK ==  (good %lu, bad %lu)\n",
                          (unsigned long)goodCount, (unsigned long)badCount);
    else    Serial.println("== LINK LOST ==  ESP32-A is not answering");
  }
}
