// Retro drawing layer (SNES dark). Flat fills, hard 2px offset "key side" shadows,
// a header strip with A/B/X/Y dots, keycap rows. All sizes come from Layout LY, so
// the same calls fit both screens. Uses the built-in GFX font scaled; no gradients.
#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "board.h"
#include "pixelfont.h"
#include "theme.h"

class Gfx {
 public:
  explicit Gfx(Adafruit_ST7789 &tft) : d(tft) {}

  Adafruit_ST7789 &d;
  int16_t W() const { return LY.w; }
  int16_t H() const { return LY.h; }

  // --- primitives ---
  void clear() { d.fillScreen(C::bg); }
  // Repaint only the area between header and hints — no header/hint flicker.
  void clearContent() { d.fillRect(0, contentTop(), LY.w, contentH(), C::bg); }
  void fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) { d.fillRect(x, y, w, h, c); }

  // Framed card: key2 border, panel face, a 1px light highlight on top — the
  // claude-usage-display look. `face` lets the selected row use the accent fill.
  void card(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t face = C::panel, uint16_t = 0) {
    d.fillRect(x, y, w, h, C::key2);                 // border
    d.fillRect(x + 1, y + 1, w - 2, h - 2, face);    // face
    d.fillRect(x + 1, y + 1, w - 2, 1, C::white);    // top highlight
  }

  // Keycap: body with a 2px dark side below, centered label (reference `key()`).
  void keycap(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t body, const char *label,
              uint16_t ink = C::keyink) {
    d.fillRect(x, y + 2, w, h - 2, C::key2);         // side
    d.fillRect(x, y, w, h - 2, body);                // face
    if (label && *label) textCenter(x + w / 2, y + (h - 2 - 8) / 2, label, ink, 1);
  }

  // Press Start 2P text. size = pixel scale (1 => 8x8 glyph, 6px advance base).
  // One glyph is 8x8; we advance 7*size so letters are tight like the reference.
  static constexpr int CW = 7;  // char advance in px at size 1
  void glyph(int16_t x, int16_t y, char c, uint16_t col, uint8_t size) {
    if (c < PF_FIRST || c > PF_LAST) c = '?';
    const uint8_t *g = PS2P_FONT[c - PF_FIRST];
    for (int row = 0; row < 8; row++) {
      uint8_t bits = pgm_read_byte(&g[row]);
      if (!bits) continue;
      for (int col8 = 0; col8 < 8; col8++)
        if (bits & (0x80 >> col8))
          d.fillRect(x + col8 * size, y + row * size, size, size, col);
    }
  }
  void text(int16_t x, int16_t y, const char *s, uint16_t col, uint8_t size = 1) {
    for (; *s; s++) { glyph(x, y, *s, col, size); x += CW * size; }
  }
  int textW(const char *s, uint8_t size) const { return (int)strlen(s) * CW * size; }
  void textCenter(int16_t cx, int16_t y, const char *s, uint16_t col, uint8_t size = 1) {
    text(cx - textW(s, size) / 2, y, s, col, size);
  }
  void textRight(int16_t rx, int16_t y, const char *s, uint16_t col, uint8_t size = 1) {
    text(rx - textW(s, size), y, s, col, size);
  }

  // --- header strip: title left, A/B/X/Y dots right, accent rule under it ---
  static constexpr int HEADER_H() { return LY.portrait ? 34 : 24; }
  static constexpr int DOT_R = 4;
  void header(const char *title) {
    d.fillRect(0, 0, LY.w, HEADER_H(), C::key2);
    d.fillRect(0, HEADER_H(), LY.w, 2, C::accent);
    uint8_t ts = LY.portrait ? 2 : 1;
    int dotsW = 4 * (DOT_R * 2 + 3) + 6;
    char buf[32];
    clip(title, buf, sizeof buf, (LY.w - dotsW - 10) / (CW * ts));
    text(6, (HEADER_H() - 8 * ts) / 2, buf, C::ink, ts);
    abxyDots();
  }
  // SNES face buttons: Y(yellow) X(blue) on the left, B(red) A(green) on the right.
  void abxyDots() {
    uint16_t cc[4] = {C::yellow, C::blue, C::red, C::green};
    int16_t cy = HEADER_H() / 2;
    int16_t x = LY.w - 6 - DOT_R;
    for (int i = 0; i < 4; i++) {
      d.fillCircle(x, cy, DOT_R, cc[i]);
      x -= (DOT_R * 2 + 3);
    }
  }
  // bottom hint bar. Two working buttons: the knob press (SEL) and K0 (RET/back).
  // Pass the OK label and the back label; a "-" hides that cell. (The middle
  // alt slot is kept in the signature for callers but no longer shown.)
  void hints(const char *ok, const char * /*unused*/, const char *back) {
    int16_t y = LY.h - HINT_H();
    d.fillRect(0, y, LY.w, HINT_H(), C::key2);
    d.fillRect(0, y, LY.w, 1, C::accent);
    int half = LY.w / 2;
    if (ok && strcmp(ok, "-")) hintCell(0, y, half, C::accent, "SEL", ok);
    if (back && strcmp(back, "-")) hintCell(half, y, LY.w - half, C::key, "RET", back);
  }
  static constexpr int HINT_H() { return LY.portrait ? 22 : 16; }

  int16_t contentTop() const { return HEADER_H() + 2; }
  int16_t contentH() const { return LY.h - contentTop() - HINT_H(); }

  // --- a selectable list row (keycap look) ---
  void row(int16_t index, int16_t rowIndex, const char *label, const char *value, bool selected) {
    int16_t rh = rowH();
    int16_t y = contentTop() + 4 + rowIndex * (rh + 3);
    uint16_t face = selected ? C::accent : C::panel;
    card(6, y, LY.w - 14, rh, face);
    uint16_t tcol = selected ? C::keyink : C::ink;
    uint8_t ts = LY.portrait ? 2 : 1;
    int16_t ty = y + (rh - 8 * ts) / 2;
    // a small lavender marker on the selected row's left edge
    if (selected) d.fillRect(6 + 1, y + 1, 4, rh - 2, C::lav);
    char buf[26];
    clip(label, buf, sizeof buf, 18);
    text(16, ty, buf, tcol, ts);
    if (value && *value) textRight(LY.w - 18, ty, value, selected ? C::keyink : C::dim, ts >= 2 ? 1 : 1);
  }
  int16_t rowH() const { return LY.portrait ? 30 : 22; }
  // Rows that actually fit between header and hints, so nothing clips on either screen.
  int rowsPerPage() const {
    int per = rowH() + 3;
    int fit = (contentH() - 4) / per;
    return fit < 1 ? 1 : fit;
  }

  // progress bar in block-bar style
  void progress(int16_t x, int16_t y, int16_t w, int16_t h, int cur, int total) {
    d.fillRect(x, y, w, h, C::panel2);
    if (total > 0) {
      int fw = (int)((long)w * cur / total);
      d.fillRect(x, y, fw, h, C::accent);
    }
    d.drawRect(x, y, w, h, C::key2);
  }

  // big centered status glyph area for the scan screen
  void banner(const char *big, uint16_t col, const char *sub = nullptr) {
    int16_t cy = contentTop() + contentH() / 2;
    uint8_t bs = LY.portrait ? 3 : 2;
    textCenter(LY.w / 2, cy - 8 * bs / 2 - (sub ? 10 : 0), big, col, bs);
    if (sub) textCenter(LY.w / 2, cy + 8 * bs, sub, C::dim, 1);
  }

  static void clip(const char *s, char *out, size_t cap, int maxChars) {
    int n = maxChars < (int)cap - 1 ? maxChars : (int)cap - 1;
    if (n < 0) n = 0;
    int len = strlen(s);
    if (len <= n) { strcpy(out, s); return; }
    if (n >= 1) { memcpy(out, s, n - 1); out[n - 1] = '~'; out[n] = 0; }
    else out[0] = 0;
  }

 private:
  void hintCell(int16_t x, int16_t y, int16_t w, uint16_t chipBody, const char *key, const char *label) {
    int chipW = 6 * strlen(key) + 8;
    int16_t ch = HINT_H() - 7;
    keycap(x + 3, y + 3, chipW, ch, chipBody, key, C::keyink);
    int16_t lx = x + 3 + chipW + 6;
    char buf[12];
    clip(label, buf, sizeof buf, (w - (lx - x) - 2) / CW);
    text(lx, y + (HINT_H() - 8) / 2, buf, C::ink, 1);
  }
};
