//Receiver esp32 code
void setup() {
  Serial.begin(115200);       // Initialize serial communication for USB
  Serial2.begin(115200, SERIAL_8N1, 26, 27);  // Initialize Serial2 for external communication (TX: 16, RX: 17)
}
void loop() {
  if (Serial.available() > 0) {
    String message = Serial.readStringUntil('\n');  // Read message from USB serial
    Serial2.println("Receiver says: " + message); // Send message to Serial2
  }
  if (Serial2.available() > 0) {
    String message = Serial2.readStringUntil('\n'); // Read mess
    age from Serial2
    Serial.print("Received from Sender: ");
    Serial.println(message);  // Print received message to USB serial
  }
}