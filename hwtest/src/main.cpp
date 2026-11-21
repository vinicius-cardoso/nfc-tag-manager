// Hardware check for T-Display + PN532 (HSU/UART) + 2.4" ST7789/EC11. Not the real firmware.
// Reports every part on Serial (115200) and on both screens.
// PN532 DIP switches at 0|0 (HSU). Its SDA/TXD pin is on GPIO21, SCL/RXD on GPIO22.
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <esp_system.h>

// ---- pins (docs/wiring/tdisplay-pn532-tft.json) ----
constexpr int PN_RX = 21, PN_TX = 22, PN_IRQ = 37;  // ESP RX <- PN532 TXD, ESP TX -> PN532 RXD
constexpr int TD_MOSI = 19, TD_SCLK = 18, TD_CS = 5, TD_DC = 16, TD_RST = 23, TD_BL = 4;
constexpr int TF_SCK = 25, TF_MOSI = 26, TF_CS = 27, TF_DC = 33, TF_RST = 32;
constexpr int ENC_A = 13, ENC_B = 17, ENC_PUSH = 15, KEY0 = 38, BTN_L = 0, BTN_R = 35;
HardwareSerial &PN = Serial2;

SPIClass hspi(HSPI);
Adafruit_ST7789 td(&SPI, TD_CS, TD_DC, TD_RST);    // T-Display 135x240
Adafruit_ST7789 tf(&hspi, TF_CS, TF_DC, TF_RST);   // 2.4" 240x320

// ---------- PN532 over HSU ----------
bool irqSeen = false;

int pnByte(uint32_t deadline) {
  while (millis() < deadline) {
    if (digitalRead(PN_IRQ) == LOW) irqSeen = true;
    if (PN.available()) return PN.read();
  }
  return -1;
}

// In LowVbat (power-up) mode the PN532 sleeps again after every command until
// SAMConfiguration succeeds, so it needs the wake-up preamble until then.
bool pnAwake = false;

bool pnSend(const uint8_t *cmd, uint8_t len) {
  while (PN.available()) PN.read();
  if (!pnAwake) {  // HSU wake-up: a long 0x55 preamble
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
  int got[6];
  bool ok = true;
  for (uint8_t i = 0; i < sizeof want; i++) {
    got[i] = pnByte(deadline);
    if (got[i] != want[i]) { ok = false; break; }
  }
  static int dumps = 0;
  if (!ok && dumps < 4) {
    dumps++;
    Serial.printf("[dbg] cmd 0x%02X ack bytes:", cmd[0]);
    for (uint8_t i = 0; i < 6; i++) { Serial.printf(" %d", got[i]); if (got[i] != want[i]) break; }
    uint32_t t = millis() + 100;
    Serial.print("  then:");
    int b;
    while ((b = pnByte(t)) >= 0) Serial.printf(" %02X", b);
    Serial.println();
  }
  return ok;
}

// Returns the payload length after D5 <cmd+1>, or -1.
int pnRead(uint8_t cmd, uint8_t *out, uint8_t max, uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  int prev = -1, b;
  while (true) {  // find the 00 FF start code
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
  pnByte(deadline);  // DCS
  pnByte(deadline);  // postamble
  return data < max ? data : max;
}

struct {
  bool ok = false, irqOk = false;
  uint8_t ic = 0, ver = 0, rev = 0;
  String uid = "-";
  uint32_t tags = 0;
} pn;

void pnInit() {
  irqSeen = false;
  const uint8_t getFw[] = {0x02};
  uint8_t r[8];
  for (int attempt = 0; attempt < 3 && !pn.ok; attempt++) {
    if (pnSend(getFw, 1) && pnRead(0x02, r, sizeof r, 200) >= 4) {
      pn.ok = true;
      pn.ic = r[0]; pn.ver = r[1]; pn.rev = r[2];
    }
    delay(50);
  }
  pn.irqOk = irqSeen;
  if (pn.ok) {
    Serial.printf("[PN532] OK over HSU  IC=0x%02X firmware %u.%u\n", pn.ic, pn.ver, pn.rev);
    const uint8_t sam[] = {0x14, 0x01, 0x14, 0x01};
    pnAwake = pnSend(sam, sizeof sam) && pnRead(0x14, r, sizeof r, 100) >= 0;
    Serial.printf("[PN532] SAMConfiguration %s\n", pnAwake ? "OK (normal mode)" : "FAILED");
    const uint8_t retries[] = {0x32, 0x05, 0xFF, 0x01, 0x02};  // give up fast when no tag
    if (pnSend(retries, sizeof retries)) pnRead(0x32, r, sizeof r, 100);
  } else {
    Serial.println("[PN532] FAIL no answer over HSU (DIP 0|0? TXD->21, RXD->22? power-cycled?)");
  }
  Serial.printf("[PN532] IRQ (GPIO37): %s\n",
                pn.irqOk ? "OK, went LOW when a reply was ready"
                         : "never went LOW - check the IRQ wire (not fatal)");
}

void pnPoll() {
  const uint8_t list[] = {0x4A, 0x01, 0x00};  // 1 target, 106 kbps type A
  uint8_t r[32];
  static uint32_t polls = 0, sendFail = 0, readFail = 0, empty = 0, lastReport = 0;
  polls++;
  if (!pnSend(list, sizeof list)) { sendFail++; }
  int n = sendFail && polls == sendFail ? -1 : pnRead(0x4A, r, sizeof r, 300);
  if (n < 0) readFail++; else if (r[0] == 0) empty++;
  if (millis() - lastReport > 3000) {
    lastReport = millis();
    Serial.printf("[dbg] polls %lu  send-fail %lu  read-fail %lu  no-tag %lu\n",
                  (unsigned long)polls, (unsigned long)sendFail, (unsigned long)readFail, (unsigned long)empty);
  }
  if (n < 0) return;
  // r: NbTg Tg SENS_RES(2) SEL_RES IDlen ID...
  if (n >= 6 && r[0] >= 1) {
    uint8_t idLen = r[5];
    String uid;
    for (uint8_t i = 0; i < idLen && 6 + i < n; i++) {
      char h[4];
      snprintf(h, sizeof h, i ? ":%02X" : "%02X", r[6 + i]);
      uid += h;
    }
    if (uid != pn.uid) {
      pn.uid = uid;
      pn.tags++;
      Serial.printf("[PN532] tag UID %s  (SAK 0x%02X)\n", uid.c_str(), r[4]);
    }
  } else if (n >= 1 && r[0] == 0 && pn.uid != "-") {
    pn.uid = "-";
    Serial.println("[PN532] tag removed");
  }
}

// ---------- encoder (interrupt, full quadrature table) ----------
volatile int32_t encSteps = 0;
volatile uint8_t encState = 0;
void IRAM_ATTR encIsr() {
  static const int8_t table[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  encState = ((encState << 2) | (digitalRead(ENC_A) << 1) | digitalRead(ENC_B)) & 0x0F;
  encSteps += table[encState];
}

// ---------- screens ----------
const char *resetName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_EXT: return "reset pin";
    default: return "other";
  }
}

void bars(Adafruit_ST7789 &d, int y, int h) {
  const uint16_t c[] = {ST77XX_RED, ST77XX_GREEN, ST77XX_BLUE, ST77XX_WHITE, ST77XX_YELLOW, ST77XX_CYAN, ST77XX_MAGENTA, ST77XX_BLACK};
  int w = d.width() / 8;
  for (int i = 0; i < 8; i++) d.fillRect(i * w, y, w, h, c[i]);
}

void drawStatus(int32_t detents, bool push, bool key0, bool bl, bool br) {
  char line[64];
  // T-Display
  td.fillRect(0, 26, td.width(), td.height() - 26, ST77XX_BLACK);
  td.setTextSize(2);
  td.setCursor(4, 30);
  td.setTextColor(pn.ok ? ST77XX_GREEN : ST77XX_RED);
  snprintf(line, sizeof line, "PN532 %s", pn.ok ? "OK" : "FAIL");
  td.print(line);
  td.setTextColor(ST77XX_WHITE);
  td.setCursor(4, 52);  td.printf("IRQ %s", pn.irqOk ? "OK" : "--");
  td.setTextSize(1);
  td.setCursor(4, 76);  td.printf("UID %s", pn.uid.c_str());
  td.setCursor(4, 90);  td.printf("knob %ld  push %d  K0 %d", (long)detents, push, key0);
  td.setCursor(4, 104); td.printf("btn L %d  R %d", bl, br);
  td.setCursor(4, 118); td.printf("reset: %s", resetName());

  // 2.4"
  tf.fillRect(0, 60, tf.width(), tf.height() - 60, ST77XX_BLACK);
  tf.setTextSize(2);
  tf.setTextColor(ST77XX_WHITE);
  tf.setCursor(8, 70);  tf.print("2.4in TFT OK if you read this");
  tf.setCursor(8, 100); tf.setTextColor(pn.ok ? ST77XX_GREEN : ST77XX_RED);
  tf.printf("PN532: %s  IRQ: %s", pn.ok ? "OK" : "FAIL", pn.irqOk ? "OK" : "--");
  tf.setTextColor(ST77XX_WHITE);
  tf.setCursor(8, 130); tf.printf("UID: %s", pn.uid.c_str());
  tf.setCursor(8, 160); tf.printf("Knob: %ld", (long)detents);
  tf.setCursor(8, 190); tf.printf("PUSH:%s K0:%s", push ? "DOWN" : "up", key0 ? "DOWN" : "up");
  tf.setCursor(8, 215); tf.printf("L:%s R:%s", bl ? "DOWN" : "up", br ? "DOWN" : "up");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== nfc-rfid hardware check ===  reset reason: %s\n", resetName());

  pinMode(TD_BL, OUTPUT);
  digitalWrite(TD_BL, HIGH);
  SPI.begin(TD_SCLK, -1, TD_MOSI, TD_CS);
  td.init(135, 240);
  td.setRotation(1);
  td.fillScreen(ST77XX_BLACK);
  bars(td, 0, 22);

  hspi.begin(TF_SCK, -1, TF_MOSI, TF_CS);
  tf.init(240, 320);
  tf.setRotation(3);
  tf.fillScreen(ST77XX_BLACK);
  bars(tf, 0, 50);
  Serial.println("[TFT] both screens initialised - look for colour bars:"
                 " red green blue white yellow cyan magenta black");

  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_PUSH, INPUT_PULLUP);
  pinMode(KEY0, INPUT);
  pinMode(BTN_L, INPUT_PULLUP);
  pinMode(BTN_R, INPUT);
  pinMode(PN_IRQ, INPUT);
  encState = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  attachInterrupt(ENC_A, encIsr, CHANGE);
  attachInterrupt(ENC_B, encIsr, CHANGE);
  Serial.printf("[IN] idle levels  A=%d B=%d PUSH=%d K0=%d L=%d R=%d IRQ=%d"
                "  (PUSH, K0, L, R should be 1)\n",
                digitalRead(ENC_A), digitalRead(ENC_B), digitalRead(ENC_PUSH),
                digitalRead(KEY0), digitalRead(BTN_L), digitalRead(BTN_R), digitalRead(PN_IRQ));

  PN.begin(115200, SERIAL_8N1, PN_RX, PN_TX);
  pnInit();
  Serial.println("[READY] turn the knob, press it, press K0 and both T-Display buttons, tap a tag");
  drawStatus(0, false, false, false, false);
}

void loop() {
  static int32_t lastDet = INT32_MIN;
  static uint8_t lastIn = 0xFF;
  static String lastUid = "?";
  static uint32_t lastPoll = 0;

  if (pn.ok && millis() - lastPoll > 250) {
    lastPoll = millis();
    pnPoll();
  }

  noInterrupts();
  int32_t steps = encSteps;
  interrupts();
  int32_t det = steps / 4;
  bool push = !digitalRead(ENC_PUSH), key0 = !digitalRead(KEY0);
  bool bl = !digitalRead(BTN_L), br = !digitalRead(BTN_R);
  uint8_t in = push | key0 << 1 | bl << 2 | br << 3;

  if (det != lastDet) Serial.printf("[ENC] knob %ld (raw %ld)\n", (long)det, (long)steps);
  if (in != lastIn && lastIn != 0xFF) {
    uint8_t changed = in ^ lastIn;
    const char *names[] = {"PUSH", "K0", "BTN L (GPIO0)", "BTN R (GPIO35)"};
    for (int i = 0; i < 4; i++)
      if (changed & (1 << i)) Serial.printf("[IN] %s %s\n", names[i], in & (1 << i) ? "pressed" : "released");
  }
  if (det != lastDet || in != lastIn || pn.uid != lastUid) {
    drawStatus(det, push, key0, bl, br);
    lastDet = det;
    lastIn = in;
    lastUid = pn.uid;
  }
  delay(5);
}
