// ESP32 USB-to-Serial Bridge for telOS on RPi 5
//
// Wiring:
//   ESP32 GPIO 16 (RX2) <-- RPi 5 Pin 8  (GPIO 14, TX)
//   ESP32 GPIO 17 (TX2) --> RPi 5 Pin 10 (GPIO 15, RX)
//   ESP32 GND           --- RPi 5 Pin 6  (GND)
//
// Upload this sketch, then open Serial Monitor at 115200 baud.
// Everything you type goes to the RPi, everything it sends comes back.

#define RPI_RX 16
#define RPI_TX 17
#define BAUD 115200

void setup() {
  Serial.begin(BAUD);       // USB serial (to your laptop)
  Serial2.begin(BAUD, SERIAL_8N1, RPI_RX, RPI_TX); // hardware UART (to RPi)
}

void loop() {
  // laptop -> RPi
  while (Serial.available())
    Serial2.write(Serial.read());

  // RPi -> laptop
  while (Serial2.available())
    Serial.write(Serial2.read());
}
