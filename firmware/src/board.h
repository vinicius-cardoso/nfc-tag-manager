// Board layout for the 2.4" ST7789 (240x320 portrait) + EC11 + PN532 (HSU).
// Pins from docs/wiring/tdisplay-pn532-tft.json. The UI reads LY so sizes live
// in one place; `portrait` stays as a flag the drawing code uses for scaling.
#pragma once
#include <stdint.h>

// ---- PN532 over HSU, and the EC11 + buttons ----
constexpr int PN_RX = 21;   // ESP32 Serial2 RX  <- PN532 TXD (labelled SDA)
constexpr int PN_TX = 22;   // ESP32 Serial2 TX  -> PN532 RXD (labelled SCL)
constexpr int PN_IRQ = 37;  // wired, unused in HSU

constexpr int ENC_A = 13;
constexpr int ENC_B = 17;
constexpr int ENC_SW = 15;  // knob press  -> select
constexpr int KEY0 = 38;    // module's extra button (ext. pull-up) -> alt action
constexpr int BTN_BOOT = 0; // ESP32 BOOT button -> back / return

struct Layout {
  int16_t w, h;          // after rotation
  uint8_t rotation;      // ST7789 rotation for an upright portrait
  int16_t initW, initH;  // panel size passed to init()
  bool portrait;
  // display SPI pins (2.4" module on HSPI; BLK tied to 3V3, so no bl pin)
  int8_t sck, mosi, cs, dc, rst, bl;
  bool blActiveHigh;
  bool invert;           // this panel needs inversion OFF (else dark shows white)
};

// 2.4" ST7789 on HSPI: SCK25 MOSI26 CS27 DC33 RST32, BLK on 3V3.
constexpr Layout LY = {
    .w = 240, .h = 320, .rotation = 2, .initW = 240, .initH = 320, .portrait = true,
    .sck = 25, .mosi = 26, .cs = 27, .dc = 33, .rst = 32, .bl = -1,
    .blActiveHigh = true, .invert = false};

#define BOARD_NAME "2.4\" TFT"
#define USE_HSPI 1
