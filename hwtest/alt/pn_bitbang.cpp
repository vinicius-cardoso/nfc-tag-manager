#include <Arduino.h>
// PN532 over bit-banged I2C that waits out clock stretching. SDA=21 SCL=22 IRQ=37.
constexpr int SDA_P = 21, SCL_P = 22, IRQ_P = 37;
constexpr int HALF = 10;  // us, ~40 kHz
inline void rel(int p) { pinMode(p, INPUT_PULLUP); }
inline void low(int p) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
bool sclHigh() {  // release SCL and wait while the slave stretches it
  rel(SCL_P);
  uint32_t t = micros();
  while (!digitalRead(SCL_P)) if (micros() - t > 50000) return false;
  return true;
}
void start() { rel(SDA_P); sclHigh(); delayMicroseconds(HALF); low(SDA_P); delayMicroseconds(HALF); low(SCL_P); }
void stop() { low(SDA_P); delayMicroseconds(HALF); sclHigh(); delayMicroseconds(HALF); rel(SDA_P); delayMicroseconds(HALF); }
bool writeByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    if (b & (1 << i)) rel(SDA_P); else low(SDA_P);
    delayMicroseconds(HALF); sclHigh(); delayMicroseconds(HALF); low(SCL_P);
  }
  rel(SDA_P); delayMicroseconds(HALF); sclHigh(); delayMicroseconds(HALF / 2);
  bool ack = !digitalRead(SDA_P);
  delayMicroseconds(HALF / 2); low(SCL_P);
  return ack;
}
uint8_t readByte(bool ack) {
  uint8_t b = 0;
  rel(SDA_P);
  for (int i = 7; i >= 0; i--) {
    delayMicroseconds(HALF); sclHigh(); delayMicroseconds(HALF / 2);
    if (digitalRead(SDA_P)) b |= 1 << i;
    delayMicroseconds(HALF / 2); low(SCL_P);
  }
  if (ack) low(SDA_P); else rel(SDA_P);
  delayMicroseconds(HALF); sclHigh(); delayMicroseconds(HALF); low(SCL_P); rel(SDA_P);
  return b;
}
bool writeFrame(const uint8_t *d, int n) {
  start();
  bool ok = writeByte(0x48);
  int i = 0;
  for (; ok && i < n; i++) ok = writeByte(d[i]);
  stop();
  Serial.printf("  write: %s (stopped after %d data bytes)\n", ok ? "all ACKed" : "NACK", i);
  return ok;
}
void readN(int n) {
  start();
  bool ok = writeByte(0x49);
  Serial.printf("  read addr %s:", ok ? "ACK" : "NACK");
  if (ok) for (int i = 0; i < n; i++) Serial.printf(" %02X", readByte(i < n - 1));
  stop();
  Serial.printf("   IRQ=%d\n", digitalRead(IRQ_P));
}
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== PN532 bit-bang I2C ===");
  pinMode(IRQ_P, INPUT);
  rel(SDA_P); rel(SCL_P);
  Serial.printf("  idle SDA=%d SCL=%d IRQ=%d\n", digitalRead(SDA_P), digitalRead(SCL_P), digitalRead(IRQ_P));
  const uint8_t getFw[] = {0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00};
  for (int round = 0; round < 3; round++) {
    Serial.printf("round %d\n", round);
    writeFrame(getFw, sizeof getFw);
    delay(20);
    readN(7);    // ACK frame
    delay(30);
    readN(14);   // response
    delay(200);
  }
}
void loop() {}
