// PN532 over HSU (UART). Verified working on this board in HSU mode (DIP 0|0);
// I2C never answered — see the pn532-hsu-only memory. Minimal, no external lib.
#pragma once
#include <Arduino.h>
#include "board.h"

class PN532 {
 public:
  struct Target {
    uint8_t uid[10] = {0};
    uint8_t uidLen = 0;
    uint8_t sak = 0;
    uint8_t atqa[2] = {0, 0};
  };
  enum class Magic : uint8_t { Unknown, No, Gen1a, Gen2, Gen4 };

  void begin() {
    PN.begin(115200, SERIAL_8N1, PN_RX, PN_TX);
    uint8_t r[16];
    getFirmware(r);                        // first talk; may return the version
    const uint8_t sam[] = {0x14, 0x01, 0x14, 0x01};
    awake_ = cmd(sam, sizeof sam, r, sizeof r) >= 0;  // normal mode
    const uint8_t retries[] = {0x32, 0x05, 0xFF, 0x01, 0x02};  // give up fast, no tag
    cmd(retries, sizeof retries, r, sizeof r);
  }

  bool getFirmware(uint8_t out[4]) {
    const uint8_t c[] = {0x02};
    uint8_t r[8];
    int n = cmd(c, 1, r, sizeof r, 200);
    if (n >= 4) { memcpy(out, r, 4); ic_ = r[0]; ver_ = r[1]; rev_ = r[2]; return true; }
    return false;
  }
  uint8_t ic() const { return ic_; }
  uint8_t verMajor() const { return ver_; }
  uint8_t verMinor() const { return rev_; }

  // One 106 kbps ISO14443A target in the field, or false.
  bool poll(Target &t) {
    const uint8_t list[] = {0x4A, 0x01, 0x00};
    uint8_t r[32];
    int n = cmd(list, sizeof list, r, sizeof r, 120);
    if (n < 6 || r[0] < 1) return false;
    t.atqa[0] = r[2]; t.atqa[1] = r[3]; t.sak = r[4]; t.uidLen = r[5];
    if (t.uidLen > sizeof t.uid) t.uidLen = sizeof t.uid;
    memcpy(t.uid, r + 6, t.uidLen);
    return true;
  }

  void fieldReset() {
    uint8_t r[4];
    const uint8_t off[] = {0x32, 0x01, 0x00}, on[] = {0x32, 0x01, 0x01};
    cmd(off, sizeof off, r, sizeof r);
    delay(20);
    cmd(on, sizeof on, r, sizeof r);
    delay(20);
  }

  // MIFARE Classic: auth a block with keyA(0)/keyB(1). The UID's last 4 bytes are used.
  bool authClassic(const Target &t, uint8_t block, const uint8_t key[6], bool keyB) {
    uint8_t c[14] = {0x40, 0x01, (uint8_t)(keyB ? 0x61 : 0x60), block};
    memcpy(c + 4, key, 6);
    memcpy(c + 10, t.uid + (t.uidLen >= 4 ? t.uidLen - 4 : 0), 4);
    uint8_t r[8];
    int n = cmd(c, sizeof c, r, sizeof r, 250);
    return n >= 1 && r[0] == 0x00;
  }

  bool readBlock(uint8_t block, uint8_t out[16]) {
    const uint8_t c[] = {0x40, 0x01, 0x30, block};
    uint8_t r[24];
    int n = cmd(c, sizeof c, r, sizeof r, 250);
    if (n >= 17 && r[0] == 0x00) { memcpy(out, r + 1, 16); return true; }
    return false;
  }

  // Normal MIFARE write (0xA0). Fails on block 0 of a genuine card.
  bool writeBlock(uint8_t block, const uint8_t in[16]) {
    uint8_t c[20] = {0x40, 0x01, 0xA0, block};
    memcpy(c + 4, in, 16);
    uint8_t r[8];
    int n = cmd(c, sizeof c, r, sizeof r, 500);
    return n >= 1 && (r[0] & 0x3F) == 0;
  }

  // Gen1a backdoor write of block 0 (unlock, then 0xA0 without auth).
  bool gen1aWriteBlock0(const uint8_t in[16]) {
    if (!gen1aUnlock()) return false;
    crc(false);
    uint8_t out[18];
    int ol;
    const uint8_t w0[] = {0xA0, 0x00};
    if (thru(w0, 2, out, ol) != 0) { crc(true); return false; }
    uint8_t data[18];
    memcpy(data, in, 16);
    appendCrcA(data, 16);
    int st = thru(data, 18, out, ol);
    crc(true);
    return st == 0;
  }
  // Gen2/CUID write of block 0 is just a normal write after keyA auth — use writeBlock(0,...).

  // Read-only magic detection (the method the magic probe used).
  Magic detectMagic(Target t) {
    fieldReset();
    if (!poll(t)) return Magic::Unknown;
    // Gen4 GTU config read
    crc(true);
    uint8_t out[48];
    int ol;
    const uint8_t cfg[] = {0xCF, 0x00, 0x00, 0x00, 0x00, 0xC6};
    if (thru(cfg, sizeof cfg, out, ol) == 0 && ol >= 30) return Magic::Gen4;
    // Gen1a backdoor
    if (gen1aUnlock()) { crc(true); return Magic::Gen1a; }
    crc(true);
    return Magic::No;  // Gen2 needs a write test; done separately with consent
  }

 private:
  HardwareSerial &PN = Serial2;
  bool awake_ = false;
  uint8_t ic_ = 0, ver_ = 0, rev_ = 0;

  int rxByte(uint32_t deadline) {
    while ((int32_t)(millis() - deadline) < 0)
      if (PN.available()) return PN.read();
    return -1;
  }

  bool send(const uint8_t *c, uint8_t len) {
    while (PN.available()) PN.read();
    if (!awake_) {
      const uint8_t wake[] = {0x55, 0x55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
      PN.write(wake, sizeof wake);
    }
    uint8_t lenTot = len + 1, sum = 0xD4;
    for (uint8_t i = 0; i < len; i++) sum += c[i];
    const uint8_t head[] = {0x00, 0x00, 0xFF, lenTot, (uint8_t)(~lenTot + 1), 0xD4};
    PN.write(head, sizeof head);
    PN.write(c, len);
    PN.write((uint8_t)(~sum + 1));
    PN.write((uint8_t)0x00);
    const uint8_t want[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    uint32_t dl = millis() + 60;
    for (uint8_t i = 0; i < sizeof want; i++)
      if (rxByte(dl) != want[i]) return false;
    return true;
  }

  // -> payload length after D5 <cmd+1>, or -1.
  int readResp(uint8_t cmd0, uint8_t *out, uint8_t max, uint32_t timeoutMs) {
    uint32_t dl = millis() + timeoutMs;
    int prev = -1, b;
    while (true) {
      b = rxByte(dl);
      if (b < 0) return -1;
      if (prev == 0x00 && b == 0xFF) break;
      prev = b;
    }
    int len = rxByte(dl), lcs = rxByte(dl);
    if (len < 2 || ((len + lcs) & 0xFF) != 0) return -1;
    if (rxByte(dl) != 0xD5 || rxByte(dl) != cmd0 + 1) return -1;
    int data = len - 2;
    for (int i = 0; i < data; i++) {
      b = rxByte(dl);
      if (b < 0) return -1;
      if (i < max) out[i] = b;
    }
    rxByte(dl);  // DCS
    rxByte(dl);  // postamble
    return data < max ? data : max;
  }

  int cmd(const uint8_t *c, uint8_t len, uint8_t *out, uint8_t max, uint32_t timeoutMs = 300) {
    if (!send(c, len)) return -1;
    return readResp(c[0], out, max, timeoutMs);
  }

  void writeReg(uint16_t addr, uint8_t val) {
    const uint8_t c[] = {0x08, (uint8_t)(addr >> 8), (uint8_t)addr, val};
    uint8_t r[4];
    cmd(c, sizeof c, r, sizeof r);
  }
  void crc(bool on) {
    writeReg(0x6338, 0x00);                    // Crypto1 off
    writeReg(0x6302, on ? 0x80 : 0x00);        // TxCRCEn
    writeReg(0x6303, on ? 0x80 : 0x00);        // RxCRCEn
  }
  // InCommunicateThru — raw frame, returns status (0 ok).
  int thru(const uint8_t *data, uint8_t len, uint8_t *out, int &outLen) {
    uint8_t c[44] = {0x42};
    memcpy(c + 1, data, len);
    uint8_t r[52];
    int n = cmd(c, len + 1, r, sizeof r, 200);
    outLen = 0;
    if (n < 1) return -1;
    outLen = n - 1;
    memcpy(out, r + 1, outLen);
    return r[0] & 0x3F;
  }
  bool gen1aUnlock() {
    fieldReset();
    Target t;
    if (!poll(t)) return false;
    crc(false);
    uint8_t out[16];
    int ol;
    const uint8_t halt[] = {0x50, 0x00, 0x57, 0xCD};
    thru(halt, sizeof halt, out, ol);
    writeReg(0x633D, 0x07);  // 7-bit frame
    const uint8_t b40[] = {0x40};
    int s1 = thru(b40, 1, out, ol);
    uint8_t r1 = ol ? out[0] : 0xFF;
    writeReg(0x633D, 0x00);
    const uint8_t b43[] = {0x43};
    int s2 = thru(b43, 1, out, ol);
    uint8_t r2 = ol ? out[0] : 0xFF;
    return s1 == 0 && (r1 & 0x0F) == 0x0A && s2 == 0 && (r2 & 0x0F) == 0x0A;
  }
  static uint16_t crcA(const uint8_t *d, int n) {
    uint16_t w = 0x6363;
    for (int i = 0; i < n; i++) {
      uint8_t b = d[i] ^ (w & 0xFF);
      b ^= b << 4;
      w = (w >> 8) ^ (b << 8) ^ (b << 3) ^ (b >> 4);
    }
    return w;
  }
  static void appendCrcA(uint8_t *d, int n) {
    uint16_t c = crcA(d, n);
    d[n] = c & 0xFF;
    d[n + 1] = c >> 8;
  }
};
