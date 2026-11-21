// EC11 rotary encoder + its push, plus K0 and the BOOT button. Encoder on a full
// quadrature table via interrupts; buttons debounced with press/long-press events.
#pragma once
#include <Arduino.h>
#include "soc/gpio_struct.h"
#include "board.h"

class Input {
 public:
  enum class Event : uint8_t { None, Up, Down, Ok, Back, Alt, OkLong };

  void begin() {
    pinMode(ENC_A, INPUT_PULLUP);
    pinMode(ENC_B, INPUT_PULLUP);
    pinMode(ENC_SW, INPUT_PULLUP);
    pinMode(KEY0, INPUT_PULLUP);  // ext. pull-up on the module; internal as backup
    pinMode(BTN_BOOT, INPUT_PULLUP);
    self_ = this;
    state_ = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
    attachInterrupt(ENC_A, isr, CHANGE);
    attachInterrupt(ENC_B, isr, CHANGE);
  }

  Event poll() {
    // rotation -> one event per detent (4 steps)
    noInterrupts();
    int32_t steps = steps_;
    interrupts();
    int32_t det = steps / 4;
    if (det != lastDet_) {
      bool up = det < lastDet_;  // clockwise = down the list by default; tune by feel
      lastDet_ = det;
      return up ? Event::Up : Event::Down;
    }
    // Buttons. The ESP32 BOOT button (GPIO0) is unreliable on this clone board,
    // so navigation uses the two that work: knob press = OK, K0 = Back.
    Event e = scanButton(ENC_SW, sw_, true);     // knob press -> OK (+ long)
    if (e != Event::None) return e;
    e = scanButton(KEY0, k0_, false);            // K0 -> Back / cancel
    if (e == Event::Ok) return Event::Back;
    return Event::None;
  }

  int32_t detents() {
    noInterrupts();
    int32_t s = steps_;
    interrupts();
    return s / 4;
  }

 private:
  struct Btn {
    bool last = true;       // pulled up = released
    uint32_t changed = 0;
    uint32_t pressedAt = 0;
    bool longSent = false;
  };
  static Input *self_;
  volatile int32_t steps_ = 0;
  volatile uint8_t state_ = 0;
  int32_t lastDet_ = 0;
  Btn sw_, k0_, boot_;

  // IRAM-safe: read the GPIO register directly (ENC_A, ENC_B are both < 32).
  static inline int IRAM_ATTR rd(int pin) { return (GPIO.in >> pin) & 0x1; }
  static void IRAM_ATTR isr();

  // Debounced active-low button. Emits Ok on release (a clean click). With
  // withLong, emits OkLong once at 600ms held and suppresses the release Ok.
  // b.last tracks the last *stable* level (true = released, pulled up).
  Event scanButton(int pin, Btn &b, bool withLong) {
    bool level = digitalRead(pin);  // HIGH = released, LOW = pressed
    uint32_t t = millis();
    if (level != b.last) {
      if (t - b.changed < 25) return Event::None;  // bounce window
      b.changed = t;
      b.last = level;
      if (!level) {                 // just pressed
        b.pressedAt = t;
        b.longSent = false;
        return Event::None;
      } else {                      // just released
        return b.longSent ? Event::None : Event::Ok;
      }
    }
    // held down: fire the long-press once
    if (withLong && !b.last && !b.longSent && t - b.pressedAt > 600) {
      b.longSent = true;
      return Event::OkLong;
    }
    return Event::None;
  }
};
inline Input *Input::self_ = nullptr;

// ISR defined out-of-line with a const quadrature table in flash, so the IRAM
// code holds no literal pool (avoids the l32r "dangerous relocation" on Xtensa).
static const int8_t ENC_TABLE[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
void IRAM_ATTR Input::isr() {
  Input *s = self_;
  uint8_t st = ((s->state_ << 2) | (rd(ENC_A) << 1) | rd(ENC_B)) & 0x0F;
  s->state_ = st;
  s->steps_ += ENC_TABLE[st];
}
