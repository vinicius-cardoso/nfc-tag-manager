#include <Arduino.h>
// Drain whatever the PN532 holds, then retry a GetFirmwareVersion write.
#include <Wire.h>
void dump(const char *tag, uint8_t n) {
  uint8_t got = Wire.requestFrom((uint8_t)0x24, n);
  Serial.printf("  %s read(%u):", tag, got);
  while (Wire.available()) Serial.printf(" %02X", Wire.read());
  Serial.printf("   IRQ=%d\n", digitalRead(37));
}
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== PN532 drain test ===");
  pinMode(37, INPUT);
  Serial.printf("  IRQ at boot=%d\n", digitalRead(37));
  Wire.begin(21, 22, 100000);
  const uint8_t frame[] = {0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00};
  for (int round = 0; round < 4; round++) {
    dump("before", 16);
    Wire.beginTransmission(0x24);
    Wire.write(frame, sizeof frame);
    uint8_t et = Wire.endTransmission();
    Serial.printf("  round %d write -> %u   IRQ=%d\n", round, et, digitalRead(37));
    delay(50);
    dump("after", 20);
    delay(200);
  }
}
void loop() {}
