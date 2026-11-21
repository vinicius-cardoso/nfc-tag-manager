// nfc-rfid — retro NFC/RFID tag manager. PN532 over HSU + EC11, on either display.
// Build per board with the platformio envs (tft24 / tdisplay).
#include <Arduino.h>
#include <SPI.h>
#include "board.h"
#include "card.h"
#include "gfx.h"
#include "input.h"
#include "pn532.h"
#include "ui.h"

#if USE_HSPI
SPIClass spiBus(HSPI);
#else
SPIClass &spiBus = SPI;
#endif

Adafruit_ST7789 tft(&spiBus, LY.cs, LY.dc, LY.rst);
Gfx gfx(tft);
PN532 pn;
KeyStore keys;
CardManager cards;
Input input;
UI ui(gfx, pn, cards, input);

void setup() {
  Serial.begin(115200);

  if (LY.bl >= 0) {
    pinMode(LY.bl, OUTPUT);
    digitalWrite(LY.bl, LY.blActiveHigh ? HIGH : LOW);
  }
  spiBus.begin(LY.sck, -1, LY.mosi, LY.cs);
  tft.init(LY.initW, LY.initH);
  // This panel renders with inversion OFF (the reference CYD uses TFT_INVERSION_OFF);
  // the Adafruit default leaves dark colors looking washed-out white.
  tft.invertDisplay(LY.invert);
  tft.setRotation(LY.rotation);
  tft.fillScreen(C::bg);

  keys.begin();
  cards.begin(&pn, &keys);
  pn.begin();
  input.begin();
  ui.begin();

  Serial.printf("nfc-rfid ready on %s (%dx%d)\n", BOARD_NAME, LY.w, LY.h);
}

void loop() {
  ui.tick();
  delay(2);
}
