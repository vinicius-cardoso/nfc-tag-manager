#include <Arduino.h>
// Is the PN532 in HSU (UART) mode? Its TXD/SDA is on GPIO21, RXD/SCL on GPIO22.
void tryOnce() {
  const uint8_t wake[] = {0x55, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00};
  const uint8_t getFw[] = {0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00};
  while (Serial2.available()) Serial2.read();
  Serial2.write(wake, sizeof wake);
  Serial2.write(getFw, sizeof getFw);
  uint32_t t = millis();
  Serial.print("  reply:");
  int n = 0;
  while (millis() - t < 300) if (Serial2.available()) { Serial.printf(" %02X", Serial2.read()); n++; }
  Serial.printf("   (%d bytes)\n", n);
}
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== PN532 HSU probe ===");
  Serial2.begin(115200, SERIAL_8N1, 21, 22);  // RX=21 (PN532 TXD), TX=22 (PN532 RXD)
  for (int i = 0; i < 3; i++) { tryOnce(); delay(100); }
}
void loop() {}
