#include <Arduino.h>
// "Is this a magic card?" probe (read-only unless built with -DGEN2_WRITE_TEST) for the PN532 in HSU mode (TXD->21, RXD->22).
// Without GEN2_WRITE_TEST nothing is written to a tag. Checks: Gen1a backdoor (7-bit 0x40 / 0x43), Gen4 GTU
// config read (CF 00000000 C6), and block 0 read with common default keys.
HardwareSerial &PN = Serial2;
constexpr int PN_RX = 21, PN_TX = 22;

// ---------- HSU transport ----------
bool pnAwake = false;

int pnByte(uint32_t deadline) {
  while (millis() < deadline)
    if (PN.available()) return PN.read();
  return -1;
}

bool pnSend(const uint8_t *cmd, uint8_t len) {
  while (PN.available()) PN.read();
  if (!pnAwake) {
    const uint8_t wake[] = {0x55, 0x55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    PN.write(wake, sizeof wake);
  }
  uint8_t lenTot = len + 1, sum = 0xD4;
  for (uint8_t i = 0; i < len; i++) sum += cmd[i];
  const uint8_t head[] = {0x00, 0x00, 0xFF, lenTot, (uint8_t)(~lenTot + 1), 0xD4};
  PN.write(head, sizeof head);
  PN.write(cmd, len);
  PN.write((uint8_t)(~sum + 1));
  PN.write((uint8_t)0x00);
  const uint8_t want[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
  uint32_t deadline = millis() + 50;
  for (uint8_t i = 0; i < sizeof want; i++)
    if (pnByte(deadline) != want[i]) return false;
  return true;
}

int pnRead(uint8_t cmd, uint8_t *out, uint8_t max, uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  int prev = -1, b;
  while (true) {
    b = pnByte(deadline);
    if (b < 0) return -1;
    if (prev == 0x00 && b == 0xFF) break;
    prev = b;
  }
  int len = pnByte(deadline), lcs = pnByte(deadline);
  if (len < 2 || ((len + lcs) & 0xFF) != 0) return -1;
  if (pnByte(deadline) != 0xD5 || pnByte(deadline) != cmd + 1) return -1;
  int data = len - 2;
  for (int i = 0; i < data; i++) {
    b = pnByte(deadline);
    if (b < 0) return -1;
    if (i < max) out[i] = b;
  }
  pnByte(deadline);
  pnByte(deadline);
  return data < max ? data : max;
}

int pnCmd(const uint8_t *cmd, uint8_t len, uint8_t *out, uint8_t max, uint32_t timeoutMs = 300) {
  if (!pnSend(cmd, len)) return -1;
  return pnRead(cmd[0], out, max, timeoutMs);
}

void writeReg(uint16_t addr, uint8_t val) {
  const uint8_t c[] = {0x08, (uint8_t)(addr >> 8), (uint8_t)addr, val};
  uint8_t r[4];
  pnCmd(c, sizeof c, r, sizeof r);
}

// ---------- card helpers ----------
struct Card {
  uint8_t uid[10], uidLen = 0, sak = 0, atqa[2] = {0, 0};
};

bool select(Card &c) {
  const uint8_t list[] = {0x4A, 0x01, 0x00};
  uint8_t r[32];
  int n = pnCmd(list, sizeof list, r, sizeof r);
  if (n < 6 || r[0] < 1) return false;
  c.atqa[0] = r[2]; c.atqa[1] = r[3]; c.sak = r[4]; c.uidLen = r[5];
  memcpy(c.uid, r + 6, c.uidLen);
  return true;
}

void fieldReset() {
  uint8_t r[4];
  const uint8_t off[] = {0x32, 0x01, 0x00}, on[] = {0x32, 0x01, 0x01};
  pnCmd(off, sizeof off, r, sizeof r);
  delay(30);
  pnCmd(on, sizeof on, r, sizeof r);
  delay(30);
}

void crcAndCrypto(bool crcOn) {
  writeReg(0x6338, 0x00);              // CIU_Status2: Crypto1 off
  writeReg(0x6302, crcOn ? 0x80 : 0x00);  // CIU_TxMode: TxCRCEn
  writeReg(0x6303, crcOn ? 0x80 : 0x00);  // CIU_RxMode: RxCRCEn
}

// InCommunicateThru; returns status byte (0 = ok) or -1, data in out/outLen.
int thru(const uint8_t *data, uint8_t len, uint8_t *out, int &outLen) {
  uint8_t c[40] = {0x42};
  memcpy(c + 1, data, len);
  uint8_t r[48];
  int n = pnCmd(c, len + 1, r, sizeof r, 200);
  outLen = 0;
  if (n < 1) return -1;
  outLen = n - 1;
  memcpy(out, r + 1, outLen);
  return r[0] & 0x3F;
}

void hex(const uint8_t *b, int n, const char *sep = " ") {
  for (int i = 0; i < n; i++) Serial.printf("%s%02X", i ? sep : "", b[i]);
}

String uidStr(const Card &c) {
  String s;
  char h[4];
  for (int i = 0; i < c.uidLen; i++) { snprintf(h, sizeof h, i ? ":%02X" : "%02X", c.uid[i]); s += h; }
  return s;
}

const char *typeName(uint8_t sak) {
  switch (sak) {
    case 0x08: return "MIFARE Classic 1K";
    case 0x18: return "MIFARE Classic 4K";
    case 0x09: return "MIFARE Mini";
    case 0x00: return "MIFARE Ultralight / NTAG";
    case 0x20: return "ISO 14443-4 (DESFire / smart card)";
    case 0x28: return "Classic 1K + 14443-4 (SmartMX)";
    default: return "unknown";
  }
}

// ---------- the tests ----------
bool readBlock0(Card &c, uint8_t *block, String &keyName) {
  const uint8_t keys[][6] = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},
                             {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
  const char *names[] = {"FFFFFFFFFFFF", "A0A1A2A3A4A5", "D3F7D3F7D3F7", "000000000000"};
  for (uint8_t k = 0; k < 4; k++) {
    for (uint8_t ab = 0; ab < 2; ab++) {
      fieldReset();
      if (!select(c)) return false;
      uint8_t auth[14] = {0x40, 0x01, (uint8_t)(ab ? 0x61 : 0x60), 0x00};
      memcpy(auth + 4, keys[k], 6);
      memcpy(auth + 10, c.uid + (c.uidLen - 4), 4);
      uint8_t r[24];
      int n = pnCmd(auth, sizeof auth, r, sizeof r);
      if (n < 1 || r[0] != 0x00) continue;
      const uint8_t rd[] = {0x40, 0x01, 0x30, 0x00};
      n = pnCmd(rd, sizeof rd, r, sizeof r);
      if (n >= 17 && r[0] == 0x00) {
        memcpy(block, r + 1, 16);
        keyName = String(ab ? "key B " : "key A ") + names[k];
        return true;
      }
    }
  }
  return false;
}

bool gen4(Card &c, uint8_t *cfg, int &cfgLen) {
  fieldReset();
  if (!select(c)) return false;
  crcAndCrypto(true);
  const uint8_t cmd[] = {0xCF, 0x00, 0x00, 0x00, 0x00, 0xC6};
  int st = thru(cmd, sizeof cmd, cfg, cfgLen);
  return st == 0 && cfgLen >= 30;
}

bool gen1a(Card &c, String &detail) {
  fieldReset();
  if (!select(c)) { detail = "reselect failed"; return false; }
  crcAndCrypto(false);
  uint8_t out[16];
  int outLen;
  const uint8_t halt[] = {0x50, 0x00, 0x57, 0xCD};  // HLTA + CRC_A
  thru(halt, sizeof halt, out, outLen);             // no answer expected
  writeReg(0x633D, 0x07);                            // CIU_BitFraming: send 7 bits
  const uint8_t b40[] = {0x40};
  int s1 = thru(b40, 1, out, outLen);
  uint8_t r1 = outLen ? out[0] : 0xFF;
  writeReg(0x633D, 0x00);
  const uint8_t b43[] = {0x43};
  int s2 = thru(b43, 1, out, outLen);
  uint8_t r2 = outLen ? out[0] : 0xFF;
  crcAndCrypto(true);
  char buf[96];
  snprintf(buf, sizeof buf, "0x40 -> status %d reply %02X, 0x43 -> status %d reply %02X", s1, r1, s2, r2);
  detail = buf;
  return s1 == 0 && (r1 & 0x0F) == 0x0A && s2 == 0 && (r2 & 0x0F) == 0x0A;
}

// Gen2/CUID: write block 0 back with its own bytes. A genuine card refuses; Gen2 accepts.
// (FUID/UFUID variants lock block 0 after this write.) Only built with -DGEN2_WRITE_TEST.
int gen2(Card &c, const uint8_t *block0, String &detail) {
  fieldReset();
  if (!select(c)) { detail = "reselect failed"; return -1; }
  uint8_t auth[14] = {0x40, 0x01, 0x60, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  memcpy(auth + 10, c.uid + (c.uidLen - 4), 4);
  uint8_t r[24];
  if (pnCmd(auth, sizeof auth, r, sizeof r) < 1 || r[0] != 0x00) { detail = "auth failed"; return -1; }
  // Control: block 1 (an ordinary data block) rewritten with its own bytes must succeed.
  const uint8_t rd1[] = {0x40, 0x01, 0x30, 0x01};
  int n = pnCmd(rd1, sizeof rd1, r, sizeof r);
  int ctl = -1;
  if (n >= 17 && r[0] == 0) {
    uint8_t w1[20] = {0x40, 0x01, 0xA0, 0x01};
    memcpy(w1 + 4, r + 1, 16);
    n = pnCmd(w1, sizeof w1, r, sizeof r, 500);
    ctl = n >= 1 ? r[0] & 0x3F : -1;
  }
  uint8_t wr[20] = {0x40, 0x01, 0xA0, 0x00};
  memcpy(wr + 4, block0, 16);
  n = pnCmd(wr, sizeof wr, r, sizeof r, 500);
  int st = n >= 1 ? r[0] & 0x3F : -1;
  char buf[80];
  snprintf(buf, sizeof buf, "control block 1 write status %d, block 0 write status %d", ctl, st);
  detail = buf;
  if (ctl != 0) { detail += " - control failed, result not trusted"; return -1; }
  if (st != 0) return 0;
  // read back to confirm block 0 is unchanged
  fieldReset();
  if (!select(c)) { detail += ", reselect after write failed"; return 1; }
  if (pnCmd(auth, sizeof auth, r, sizeof r) < 1 || r[0] != 0x00) { detail += ", re-auth failed"; return 1; }
  const uint8_t rd[] = {0x40, 0x01, 0x30, 0x00};
  n = pnCmd(rd, sizeof rd, r, sizeof r);
  detail += (n >= 17 && r[0] == 0 && memcmp(r + 1, block0, 16) == 0) ? ", read-back identical" : ", READ-BACK DIFFERS";
  return 1;
}

void probe(Card &c) {
  Serial.printf("\n==================== tag %s ====================\n", uidStr(c).c_str());
  Serial.printf("ATQA %02X%02X  SAK %02X  -> %s, %u-byte UID\n", c.atqa[0], c.atqa[1], c.sak, typeName(c.sak), c.uidLen);

  bool classic = c.sak == 0x08 || c.sak == 0x18 || c.sak == 0x09 || c.sak == 0x28;
  uint8_t block0[16];
  String key;
  bool b0 = classic && readBlock0(c, block0, key);
  if (classic) {
    if (b0) {
      Serial.printf("Block 0 (%s): ", key.c_str());
      hex(block0, 16);
      Serial.println();
      if (c.uidLen == 4) {
        uint8_t bcc = block0[0] ^ block0[1] ^ block0[2] ^ block0[3];
        Serial.printf("  BCC %s (stored %02X, computed %02X)\n", bcc == block0[4] ? "valid" : "INVALID", block0[4], bcc);
        Serial.printf("  block-0 SAK/ATQA bytes: %02X / %02X%02X\n", block0[5], block0[6], block0[7]);
        static const uint8_t clone[] = {0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69};
        if (memcmp(block0 + 8, clone, 8) == 0)
          Serial.println("  manufacturer bytes 62..69: typical of Chinese clone / magic cards");
      }
    } else {
      Serial.println("Block 0: not readable with the 4 default keys (custom keys)");
    }
  }

  uint8_t cfg[40];
  int cfgLen = 0;
  bool g4 = gen4(c, cfg, cfgLen);
  Serial.printf("Gen4 GTU config read: %s", g4 ? "ANSWERED -> Gen4 / Ultimate magic card\n  config: " : "no answer\n");
  if (g4) { hex(cfg, cfgLen); Serial.println(); }

  String detail;
  bool g1 = gen1a(c, detail);
  Serial.printf("Gen1a backdoor: %s (%s)\n", g1 ? "ANSWERED -> Gen1a magic card" : "no answer", detail.c_str());

  int g2 = -1;
#ifdef GEN2_WRITE_TEST
  if (b0 && !g1 && !g4) {
    String d2;
    g2 = gen2(c, block0, d2);
    Serial.printf("Gen2 block-0 rewrite: %s (%s)\n",
                  g2 == 1 ? "ACCEPTED -> Gen2 / CUID magic card" : g2 == 0 ? "refused" : "not run", d2.c_str());
  }
#endif

  Serial.print("VERDICT: ");
  if (g1) Serial.println("MAGIC Gen1a (UID changeable via backdoor)");
  else if (g4) Serial.println("MAGIC Gen4 / Ultimate");
  else if (g2 == 1) Serial.println("MAGIC Gen2 / CUID (block 0 writable)");
  else if (g2 == 0) Serial.println("NOT magic (genuine-behaving card: block 0 read-only)");
  else if (classic) Serial.println("not Gen1a/Gen4; Gen2/CUID can't be ruled out without a block-0 write test");
  else Serial.println("not Gen4; Gen1a test only applies to Classic-style cards");
  fieldReset();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  #ifdef GEN2_WRITE_TEST
  Serial.println("\n=== PN532 magic card probe (with Gen2 block-0 rewrite test) ===");
#else
  Serial.println("\n=== PN532 magic card probe (read-only) ===");
#endif
  PN.begin(115200, SERIAL_8N1, PN_RX, PN_TX);
  uint8_t r[16];
  const uint8_t fw[] = {0x02};
  int n = pnCmd(fw, 1, r, sizeof r, 200);
  const uint8_t sam[] = {0x14, 0x01, 0x14, 0x01};
  pnAwake = pnCmd(sam, sizeof sam, r, sizeof r) >= 0;
  const uint8_t retries[] = {0x32, 0x05, 0xFF, 0x01, 0x02};
  pnCmd(retries, sizeof retries, r, sizeof r);
  Serial.printf("PN532 %s  SAM %s\n", n >= 4 ? "OK" : "FAIL", pnAwake ? "OK" : "FAIL");
  Serial.println("Hold a tag on the reader...");
}

void loop() {
  static String done[8];
  static int doneN = 0;
  Card c;
  if (select(c)) {
    String u = uidStr(c);
    bool seen = false;
    for (int i = 0; i < doneN; i++) seen |= done[i] == u;
    if (!seen) {
      probe(c);
      if (doneN < 8) done[doneN++] = u;
      Serial.println("Done - remove this tag and hold the next one.");
    }
  }
  delay(200);
}
