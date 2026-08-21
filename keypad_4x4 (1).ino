/*
 * 4x4 Matrix Keypad - ESP32
 * Requires: "Keypad" library by Mark Stanley & Alexander Brevig
 * (Arduino IDE: Sketch > Include Library > Manage Libraries > search "Keypad")
 *
 * Pin map:
 * Rows: D4, D5, D18, D19
 * Cols: D16, D17, D21, D14
 */

#include <Keypad.h>

const byte ROWS = 4;
const byte COLS = 4;

// Change these characters to match your keypad's labeling if different
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {4, 5, 18, 19};   // R1, R2, R3, R4
byte colPins[COLS] = {16, 17, 21, 14}; // C1, C2, C3, C4

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

void setup() {
  Serial.begin(115200);
  Serial.println("4x4 Keypad Ready");
}

void loop() {
  char key = keypad.getKey();

  if (key) {
    Serial.print("Key Pressed: ");
    Serial.println(key);
  }
}
