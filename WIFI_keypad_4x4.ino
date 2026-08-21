/*
 * 4x4 Matrix Keypad + I2C LCD + WiFi - ESP32
 *
 * Libraries required (Library Manager):
 *  - "Keypad" by Mark Stanley & Alexander Brevig
 *  - "LiquidCrystal I2C" by Frank de Brabander
 *
 * Pin map:
 * Keypad Rows: D4, D16, D17, D5
 * Keypad Cols: D18, D19, D21, D14
 * LCD  I2C:    SDA=D32, SCL=D33
 *
 * WiFi does NOT affect any of these pins (they're not ADC2),
 * so keypad + LCD keep working normally with WiFi on.
 */

#include <Keypad.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>

// ---- WiFi credentials ----
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// ---- Keypad setup ----
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {4, 16, 17, 5};   // R1, R2, R3, R4
byte colPins[COLS] = {18, 19, 21, 14}; // C1, C2, C3, C4

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ---- LCD setup (0x27 is the common default address; use 0x3F if blank) ----
LiquidCrystal_I2C lcd(0x27, 16, 2);

String inputBuffer = "";

void setup() {
  Serial.begin(115200);

  // I2C on custom pins
  Wire.begin(32, 33); // SDA, SCL

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi Connected");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP());
  delay(2000);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Enter:");
}

void loop() {
  char key = keypad.getKey();

  if (key) {
    Serial.print("Key Pressed: ");
    Serial.println(key);

    if (key == '#') {
      // Confirm entry - do something with inputBuffer here
      Serial.print("Entered: ");
      Serial.println(inputBuffer);
      inputBuffer = "";
      lcd.setCursor(0, 1);
      lcd.print("                "); // clear line
    } else if (key == '*') {
      // Clear entry
      inputBuffer = "";
      lcd.setCursor(0, 1);
      lcd.print("                "); // clear line
    } else {
      inputBuffer += key;
    }

    lcd.setCursor(0, 1);
    lcd.print(inputBuffer);
  }
}
