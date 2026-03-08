// Test version of serial bridge — prints confirmation on startup

#define RPI_RX 16
#define RPI_TX 17
#define BAUD 115200

void setup() {
  Serial.begin(BAUD);
  Serial2.begin(BAUD, SERIAL_8N1, RPI_RX, RPI_TX);

  delay(1000);
  Serial.println("=== ESP32 serial bridge is running ===");
  Serial.println("Waiting for data from RPi on GPIO 16...");
}

void loop() {
  // laptop -> RPi
  while (Serial.available())
    Serial2.write(Serial.read());

  // RPi -> laptop
  if (Serial2.available()) {
    Serial.print("[RX] ");
    while (Serial2.available())
      Serial.write(Serial2.read());
  }
}
