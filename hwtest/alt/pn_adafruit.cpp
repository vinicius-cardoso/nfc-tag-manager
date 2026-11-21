// Minimal PN532 check through Adafruit's library (I2C, IRQ on 37, unused GPIO 2 as reset).
#include <Wire.h>
#include <Adafruit_PN532.h>
Adafruit_PN532 *nfc;
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Adafruit PN532 check ===");
  Wire.begin(21, 22, 100000);
  nfc = new Adafruit_PN532(37, 2, &Wire);
  bool b = nfc->begin();
  Serial.printf("begin()=%d\n", b);
  uint32_t v = nfc->getFirmwareVersion();
  Serial.printf("getFirmwareVersion()=0x%08lX\n", (unsigned long)v);
  if (v) { nfc->SAMConfig(); Serial.println("SAMConfig done, tap a tag"); }
}
void loop() {
  uint8_t uid[7], len;
  if (nfc->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 500)) {
    Serial.print("UID");
    for (int i = 0; i < len; i++) Serial.printf(" %02X", uid[i]);
    Serial.println();
  }
}
