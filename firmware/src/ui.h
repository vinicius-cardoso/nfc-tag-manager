// The screens and their navigation, driven entirely by the EC11 (rotate = move,
// press = OK, long-press = OK-long, K0 = Alt, BOOT = Back). Layout-agnostic: it only
// uses Gfx, which adapts to each display.
#pragma once
#include "card.h"
#include "gfx.h"
#include "input.h"

class UI {
 public:
  UI(Gfx &g, PN532 &pn, CardManager &cm, Input &in)
      : g_(g), pn_(pn), cm_(cm), in_(in) {}

  void begin() {
    splash();
    go(Screen::Scan);
  }

  void tick() {
    Input::Event e = in_.poll();
    switch (screen_) {
      case Screen::Scan: scanTick(e); break;
      case Screen::Menu: menuTick(e); break;
      case Screen::Info: infoTick(e); break;
      case Screen::Hex: hexTick(e); break;
      case Screen::Busy: break;  // driven by the running op
      case Screen::Result: resultTick(e); break;
    }
  }

 private:
  enum class Screen : uint8_t { Scan, Menu, Info, Hex, Busy, Result };
  Gfx &g_;
  PN532 &pn_;
  CardManager &cm_;
  Input &in_;
  Screen screen_ = Screen::Scan;

  CardInfo card_;
  Dump dump_;
  bool haveDump_ = false;
  int menuSel_ = 0, menuTop_ = 0;
  int hexBlock_ = 0;
  int infoTop_ = 0;
  String resultTitle_, resultLines_[8];
  int resultN_ = 0;
  uint16_t resultColor_ = C::green;
  uint32_t lastPoll_ = 0;
  bool present_ = false;

  static const char *const MENU[];
  static const int MENU_N;

  void go(Screen s) {
    screen_ = s;
    switch (s) {
      case Screen::Scan: drawScan("Waiting for a tag", false); break;
      case Screen::Menu: drawMenu(); break;
      case Screen::Info: infoTop_ = 0; drawInfo(); break;
      case Screen::Hex: hexBlock_ = 0; drawHex(); break;
      case Screen::Result: drawResult(); break;
      default: break;
    }
  }

  // ---------------- splash ----------------
  void splash() {
    g_.clear();
    g_.d.fillRect(0, 0, g_.W(), g_.H(), C::bg);
    int16_t cy = g_.H() / 2;
    g_.textCenter(g_.W() / 2, cy - 30, "NFC", C::accent, LY.portrait ? 5 : 3);
    g_.textCenter(g_.W() / 2, cy + 8, "TAG MANAGER", C::ink, LY.portrait ? 2 : 1);
    g_.textCenter(g_.W() / 2, cy + 34, BOARD_NAME, C::dim, 1);
    // A/B/X/Y dots, same colors as the header
    uint16_t cc[4] = {C::green, C::red, C::blue, C::yellow};
    int16_t x = g_.W() / 2 - 24;
    for (int i = 0; i < 4; i++) { g_.d.fillCircle(x, cy + 58, 5, cc[i]); x += 16; }
    uint8_t fw[4];
    bool ok = pn_.getFirmware(fw);
    char sub[40];
    if (ok) snprintf(sub, sizeof sub, "PN532 fw %u.%u ready", pn_.verMajor(), pn_.verMinor());
    else strcpy(sub, "PN532 not found!");
    g_.textCenter(g_.W() / 2, g_.H() - 16, sub, ok ? C::green : C::red, 1);
    delay(1400);
  }

  // ---------------- scan ----------------
  void drawScan(const char *msg, bool found) {
    g_.clear();
    g_.header("SCAN");
    g_.banner(found ? "FOUND" : "TAP", found ? C::green : C::accent, msg);
    g_.hints("-", "-", "-");
  }

  void scanTick(Input::Event) {
    if (millis() - lastPoll_ < 150) return;
    lastPoll_ = millis();
    PN532::Target t;
    if (pn_.poll(t)) {
      card_ = CardInfo{};
      card_.t = t;
      cm_.identify(card_);
      card_.magic = PN532::Magic::Unknown;
      haveDump_ = false;
      menuSel_ = 0; menuTop_ = 0;
      // confirmation beep-less flash
      drawScan(card_.uidColon().c_str(), true);
      delay(450);
      go(Screen::Menu);
    }
  }

  // ---------------- menu ----------------
  // Full frame (header + hints), drawn once on entry.
  void drawMenu() {
    g_.clear();
    char title[24];
    snprintf(title, sizeof title, "%s", card_.uidColon().c_str());
    g_.header(title);
    g_.hints("select", "rescan", "back");
    drawMenuBody();
  }
  // Only the rows — repaints the content area, so header/hints never flicker.
  void drawMenuBody() {
    g_.clearContent();
    int rows = g_.rowsPerPage();
    if (menuSel_ < menuTop_) menuTop_ = menuSel_;
    if (menuSel_ >= menuTop_ + rows) menuTop_ = menuSel_ - rows + 1;
    for (int i = 0; i < rows && menuTop_ + i < MENU_N; i++) {
      int idx = menuTop_ + i;
      g_.row(idx, i, MENU[idx], nullptr, idx == menuSel_);
    }
    if (menuTop_ > 0) g_.textCenter(g_.W() - 8, g_.contentTop() + 2, "^", C::accent, 1);
    if (menuTop_ + rows < MENU_N) g_.textCenter(g_.W() - 8, g_.H() - g_.HINT_H() - 10, "v", C::accent, 1);
  }

  void menuTick(Input::Event e) {
    switch (e) {
      case Input::Event::Up: menuSel_ = (menuSel_ - 1 + MENU_N) % MENU_N; drawMenuBody(); break;
      case Input::Event::Down: menuSel_ = (menuSel_ + 1) % MENU_N; drawMenuBody(); break;
      case Input::Event::Alt: go(Screen::Scan); break;
      case Input::Event::Back: go(Screen::Scan); break;
      case Input::Event::Ok:
      case Input::Event::OkLong: runMenu(menuSel_); break;
      default: break;
    }
  }

  void runMenu(int sel) {
    switch (sel) {
      case 0: go(Screen::Info); break;                 // View info
      case 1: doReadFull(); break;                     // Read full
      case 2: if (haveDump_) go(Screen::Hex); else notReadYet(); break;  // Hex view
      case 3: doWriteBlock(); break;                   // Write block
      case 4: doClone(); break;                        // Clone
      case 5: doMagicCheck(); break;                   // Magic check
      case 6: doErase(); break;                        // Erase data
      default: break;
    }
  }

  void notReadYet() {
    result("READ FIRST", C::yellow, {"Run \"Read full\"", "before hex view."});
  }

  // ---------------- info ----------------
  void infoLines(String *out, int &n) {
    n = 0;
    out[n++] = String("UID   ") + card_.uidColon();
    out[n++] = String("Len   ") + card_.t.uidLen + " bytes";
    out[n++] = String("Type  ") + card_.kindName();
    out[n++] = String("Freq  13.56 MHz (HF)");
    out[n++] = String("Proto ISO 14443A");
    char sa[16]; snprintf(sa, sizeof sa, "SAK   0x%02X", card_.t.sak); out[n++] = sa;
    char aq[20]; snprintf(aq, sizeof aq, "ATQA  %02X%02X", card_.t.atqa[0], card_.t.atqa[1]); out[n++] = aq;
    if (card_.blocks) {
      out[n++] = String("Size  ") + (int)card_.bytesTotal + " B";
      out[n++] = String("Sect  ") + (int)card_.sectors + " / " + (int)card_.blocks + " blk";
    }
    out[n++] = String("Magic ") + card_.magicName();
    if (haveDump_) out[n++] = String("Read  ") + dump_.readOk + "/" + dump_.blocks + " blk";
  }

  void drawInfo() {
    g_.clear();
    g_.header("INFO");
    g_.hints("-", "-", "back");
    drawInfoBody();
  }
  void drawInfoBody() {
    g_.clearContent();
    String lines[16];
    int n;
    infoLines(lines, n);
    int rows = g_.contentH() / lineH();
    if (infoTop_ > n - rows) infoTop_ = max(0, n - rows);
    if (infoTop_ < 0) infoTop_ = 0;
    for (int i = 0; i < rows && infoTop_ + i < n; i++) {
      int16_t y = g_.contentTop() + 4 + i * lineH();
      g_.text(8, y, lines[infoTop_ + i].c_str(), i % 2 ? C::dim : C::ink, 1);
    }
  }
  int16_t lineH() const { return LY.portrait ? 24 : 14; }

  void infoTick(Input::Event e) {
    if (e == Input::Event::Back || e == Input::Event::Ok) { go(Screen::Menu); return; }
    if (e != Input::Event::Up && e != Input::Event::Down) return;
    int before = infoTop_;
    infoTop_ += (e == Input::Event::Down) ? 1 : -1;
    // clamp to range; if it didn't move (already at an edge), don't repaint
    String lines[16];
    int n;
    infoLines(lines, n);
    int rows = g_.contentH() / lineH();
    int maxTop = n - rows < 0 ? 0 : n - rows;
    if (infoTop_ < 0) infoTop_ = 0;
    if (infoTop_ > maxTop) infoTop_ = maxTop;
    if (infoTop_ != before) drawInfoBody();
  }

  // ---------------- hex view ----------------
  void drawHex() {
    g_.clear();
    char t[16];
    snprintf(t, sizeof t, "BLK %d", hexBlock_);
    g_.header(t);
    int16_t y = g_.contentTop() + 6;
    if (!dump_.read[hexBlock_]) {
      g_.textCenter(g_.W() / 2, y + 20, "locked", C::red, 2);
      SectorKey &k = dump_.keys[CardManager::sectorOf(hexBlock_)];
      g_.textCenter(g_.W() / 2, y + 50, (k.haveA || k.haveB) ? "key ok, read fail" : "no key found", C::dim, 1);
    } else {
      // 16 bytes as 2 rows of 8, hex + ascii
      for (int r = 0; r < 2; r++) {
        char line[40];
        int p = 0;
        for (int c = 0; c < 8; c++) p += snprintf(line + p, sizeof line - p, "%02X ", dump_.data[hexBlock_][r * 8 + c]);
        g_.text(8, y + r * (LY.portrait ? 26 : 16), line, C::ink, LY.portrait ? 2 : 1);
      }
      char asc[17];
      for (int i = 0; i < 16; i++) { uint8_t b = dump_.data[hexBlock_][i]; asc[i] = (b >= 32 && b < 127) ? b : '.'; }
      asc[16] = 0;
      g_.text(8, y + (LY.portrait ? 60 : 36), asc, C::dim, LY.portrait ? 2 : 1);
      if (CardManager::isTrailer(hexBlock_))
        g_.text(8, y + (LY.portrait ? 92 : 52), "[sector trailer]", C::yellow, 1);
    }
    char pos[20];
    snprintf(pos, sizeof pos, "%d/%d", hexBlock_ + 1, dump_.blocks);
    g_.hints("-", pos, "back");
  }

  void hexTick(Input::Event e) {
    if (e == Input::Event::Down) { hexBlock_ = (hexBlock_ + 1) % dump_.blocks; drawHex(); }
    else if (e == Input::Event::Up) { hexBlock_ = (hexBlock_ - 1 + dump_.blocks) % dump_.blocks; drawHex(); }
    else if (e == Input::Event::Back || e == Input::Event::Ok) go(Screen::Menu);
  }

  // ---------------- operations ----------------
  static UI *self_;
  // Return true to cancel: K0 (Back) during a long op aborts it.
  static bool progressCb(int cur, int total) {
    self_->drawBusy("Reading", cur, total);
    return self_->in_.poll() == Input::Event::Back;
  }
  static bool cloneCb(int cur, int total) {
    self_->drawBusy("Writing", cur, total);
    return self_->in_.poll() == Input::Event::Back;
  }

  void drawBusy(const char *what, int cur, int total) {
    static uint32_t last = 0;
    if (millis() - last < 60 && cur < total) return;
    last = millis();
    g_.clear();
    g_.header("WORKING");
    int16_t cy = g_.contentTop() + g_.contentH() / 2;
    g_.textCenter(g_.W() / 2, cy - 24, what, C::ink, 2);
    g_.progress(16, cy + 4, g_.W() - 32, 14, cur, total);
    char p[16];
    snprintf(p, sizeof p, "%d / %d", cur, total);
    g_.textCenter(g_.W() / 2, cy + 26, p, C::dim, 1);
  }

  bool ensurePresent() {
    pn_.fieldReset();
    PN532::Target t;
    if (pn_.poll(t)) { card_.t = t; return true; }
    result("REMOVED", C::red, {"Tag not in field.", "Hold it still."});
    return false;
  }

  void doReadFull() {
    if (!card_.isClassic()) { result("N/A", C::yellow, {"Full read is for", "MIFARE Classic."}); return; }
    screen_ = Screen::Busy;
    if (!ensurePresent()) return;
    self_ = this;
    int sec = cm_.readFull(card_, dump_, progressCb);
    haveDump_ = true;
    if (cm_.cancelled) { result("CANCELLED", C::yellow, {"Stopped by user.", "Partial dump kept."}); return; }
    char l1[24], l2[24];
    snprintf(l1, sizeof l1, "%d/%d sectors keyed", sec, dump_.sectors);
    snprintf(l2, sizeof l2, "%d/%d blocks read", dump_.readOk, dump_.blocks);
    result(sec == dump_.sectors ? "READ OK" : "PARTIAL", sec == dump_.sectors ? C::green : C::yellow, {l1, l2, "Keys saved to flash."});
  }

  // Guided wizard: SOURCE -> read -> DEST -> write, each step gated by the knob.
  // `step`/`total` drive a small step counter; `want` is the card expected now.
  // Returns false if the user pressed RET/back to cancel.
  enum class WaitResult { Got, Cancelled, Timeout };
  WaitResult cloneStage(const char *tag, int step, int total, const char *line1,
                        const char *line2, PN532::Target &out) {
    // 1) prompt and wait for the knob press to begin (so the user can place the card)
    for (;;) {
      g_.clear();
      g_.header(tag);
      int16_t cy = g_.contentTop() + 24;
      char sc[16];
      snprintf(sc, sizeof sc, "STEP %d/%d", step, total);
      g_.textCenter(g_.W() / 2, cy, sc, C::dim, 1);
      g_.textCenter(g_.W() / 2, cy + 26, line1, C::ink, LY.portrait ? 2 : 1);
      if (line2 && *line2) g_.textCenter(g_.W() / 2, cy + 26 + (LY.portrait ? 24 : 16), line2, C::dim, 1);
      g_.textCenter(g_.W() / 2, g_.H() - g_.HINT_H() - 40, "press SEL when ready", C::accent, 1);
      g_.hints("go", "-", "cancel");
      for (;;) {
        Input::Event e = in_.poll();
        if (e == Input::Event::Ok) break;
        if (e == Input::Event::Back) return WaitResult::Cancelled;
        delay(5);
      }
      // 2) now actually look for a card, a few seconds
      g_.clearContent();
      g_.banner("READING", C::accent, "hold the card still");
      uint32_t t0 = millis();
      while (millis() - t0 < 4000) {
        pn_.fieldReset();
        if (pn_.poll(out)) return WaitResult::Got;
        if (in_.poll() == Input::Event::Back) return WaitResult::Cancelled;
        delay(100);
      }
      // nothing found — show a retry prompt and loop back to the press step
      g_.clearContent();
      g_.banner("NO CARD", C::red, "try again");
      delay(900);
    }
  }

  void doClone() {
    // ---- step 1: source card ----
    PN532::Target src;
    WaitResult w = cloneStage("CLONE 1/2", 1, 2, "Place SOURCE card", "the one to copy", src);
    if (w != WaitResult::Got) { go(Screen::Menu); return; }
    CardInfo srcInfo;
    srcInfo.t = src;
    cm_.identify(srcInfo);
    if (!srcInfo.isClassic()) { result("N/A", C::yellow, {"Clone supports", "MIFARE Classic."}); return; }

    // ---- read the source fully ----
    self_ = this;
    (void)cm_.readFull(srcInfo, dump_, progressCb);
    haveDump_ = true;
    card_ = srcInfo;  // so the menu/info reflect the source afterwards
    if (cm_.cancelled) { go(Screen::Menu); return; }
    if (dump_.readOk == 0) { result("READ FAIL", C::red, {"No block read.", "Unknown keys."}); return; }
    {
      char rl[28];
      snprintf(rl, sizeof rl, "read %d/%d blocks", dump_.readOk, dump_.blocks);
      g_.clear();
      g_.header("CLONE");
      g_.banner("SOURCE OK", C::green, rl);
      g_.hints("next", "-", "cancel");
      for (;;) {
        Input::Event e = in_.poll();
        if (e == Input::Event::Ok) break;
        if (e == Input::Event::Back) { go(Screen::Menu); return; }
        delay(5);
      }
    }

    // ---- step 2: destination card ----
    PN532::Target dst;
    w = cloneStage("CLONE 2/2", 2, 2, "Place TARGET card", "the blank to write", dst);
    if (w != WaitResult::Got) { go(Screen::Menu); return; }
    CardInfo target;
    target.t = dst;
    cm_.identify(target);
    if (!target.isClassic()) { result("N/A", C::yellow, {"Target must be", "MIFARE Classic."}); return; }

    // same card? warn (writing a card onto itself is pointless)
    if (target.uidHex() == srcInfo.uidHex()) {
      if (!confirm("SAME CARD", "Source and target", "look identical. Go on?")) { go(Screen::Menu); return; }
    }

    // ---- write ----
    self_ = this;
    CardManager::CloneReport rep = cm_.clone(dump_, target, cloneCb);
    char l1[28], l2[28], l3[28];
    snprintf(l1, sizeof l1, "%d written, %d skip", rep.blocksWritten, rep.blocksSkipped);
    snprintf(l2, sizeof l2, "%d failed", rep.blocksFailed);
    snprintf(l3, sizeof l3, "UID: %s", rep.uidCopied ? "copied" : (rep.note.length() ? rep.note.c_str() : "kept"));
    bool good = rep.blocksFailed == 0;
    result(good ? "CLONED" : "DONE*", good ? C::green : C::yellow, {l1, l2, l3});
  }

  void doWriteBlock() {
    // Minimal on-device write: fill block 4 with a demo pattern after confirm.
    // (A full byte editor is a lot of UI; this proves the path and is safe.)
    if (!card_.isClassic()) { result("N/A", C::yellow, {"Write is for", "MIFARE Classic."}); return; }
    if (!confirm("WRITE BLK 4", "Fill block 4 with", "DE AD BE EF ...?")) { go(Screen::Menu); return; }
    screen_ = Screen::Busy;
    if (!ensurePresent()) return;
    uint8_t demo[16];
    for (int i = 0; i < 16; i++) demo[i] = (i % 2) ? 0xEF : 0xDE;
    CardManager::WriteResult w = cm_.writeOneBlock(card_, 4, demo, false);
    switch (w) {
      case CardManager::WriteResult::Ok: result("WRITTEN", C::green, {"Block 4 updated."}); break;
      case CardManager::WriteResult::NoKey: result("NO KEY", C::red, {"Sector 1 key", "not found."}); break;
      case CardManager::WriteResult::Denied: result("DENIED", C::yellow, {"Protected block."}); break;
      default: result("FAILED", C::red, {"Write rejected."}); break;
    }
  }

  void doErase() {
    if (!card_.isClassic()) { result("N/A", C::yellow, {"Erase is for", "MIFARE Classic."}); return; }
    if (!confirm("ERASE DATA", "Zero all data blocks", "(keeps UID+keys)?")) { go(Screen::Menu); return; }
    screen_ = Screen::Busy;
    if (!ensurePresent()) return;
    uint8_t zero[16] = {0};
    int ok = 0, fail = 0;
    bool cancel = false;
    for (uint16_t b = 1; b < card_.blocks && !cancel; b++) {
      if (CardManager::isTrailer(b)) continue;
      pn_.fieldReset();
      PN532::Target t;
      if (!pn_.poll(t)) { fail++; continue; }
      card_.t = t;
      if (cm_.writeOneBlock(card_, b, zero, false) == CardManager::WriteResult::Ok) ok++; else fail++;
      drawBusy("Erasing", b, card_.blocks);
      if (in_.poll() == Input::Event::Back) cancel = true;  // K0 cancels
    }
    if (cancel) { result("CANCELLED", C::yellow, {"Erase stopped."}); return; }
    char l[24]; snprintf(l, sizeof l, "%d ok, %d failed", ok, fail);
    result(fail == 0 ? "ERASED" : "PARTIAL", fail == 0 ? C::green : C::yellow, {l});
  }

  void doMagicCheck() {
    screen_ = Screen::Busy;
    if (!ensurePresent()) return;
    g_.clear(); g_.header("MAGIC"); g_.banner("...", C::accent, "probing");
    card_.magic = pn_.detectMagic(card_.t);
    pn_.fieldReset();
    PN532::Target t; pn_.poll(t); card_.t = t;
    uint16_t col = card_.magic == PN532::Magic::No ? C::blue : C::green;
    result("MAGIC?", col, {card_.magicName(), card_.magic == PN532::Magic::No ? "UID not writable" : "UID writable"});
  }

  // ---------------- result + confirm ----------------
  void result(const char *title, uint16_t col, std::initializer_list<const char *> lines) {
    resultTitle_ = title;
    resultColor_ = col;
    resultN_ = 0;
    for (auto l : lines) if (resultN_ < 8) resultLines_[resultN_++] = l;
    go(Screen::Result);
  }
  void drawResult() {
    g_.clear();
    g_.header(resultTitle_.c_str());
    int16_t y = g_.contentTop() + 10;
    g_.textCenter(g_.W() / 2, y, resultTitle_.c_str(), resultColor_, LY.portrait ? 3 : 2);
    y += (LY.portrait ? 40 : 26);
    for (int i = 0; i < resultN_; i++) {
      g_.textCenter(g_.W() / 2, y, resultLines_[i].c_str(), C::ink, 1);
      y += (LY.portrait ? 20 : 12);
    }
    g_.hints("ok", "-", "menu");
  }
  void resultTick(Input::Event e) {
    if (e == Input::Event::Ok || e == Input::Event::Back || e == Input::Event::Alt) go(Screen::Menu);
  }

  bool confirm(const char *title, const char *l1, const char *l2) {
    int sel = 0;  // 0 = No, 1 = Yes (default No, safer)
    while (true) {
      g_.clear();
      g_.header(title);
      int16_t y = g_.contentTop() + 12;
      g_.textCenter(g_.W() / 2, y, l1, C::ink, 1);
      g_.textCenter(g_.W() / 2, y + 16, l2, C::ink, 1);
      int16_t by = y + 48, bw = 70, bh = 30, gap = 16;
      int16_t x0 = g_.W() / 2 - bw - gap / 2, x1 = g_.W() / 2 + gap / 2;
      g_.card(x0, by, bw, bh, sel == 0 ? C::red : C::panel);
      g_.textCenter(x0 + bw / 2, by + 8, "NO", sel == 0 ? C::keyink : C::dim, 2);
      g_.card(x1, by, bw, bh, sel == 1 ? C::green : C::panel);
      g_.textCenter(x1 + bw / 2, by + 8, "YES", sel == 1 ? C::keyink : C::dim, 2);
      g_.hints("confirm", "-", "cancel");
      // modal input loop
      while (true) {
        Input::Event e = in_.poll();
        if (e == Input::Event::Up || e == Input::Event::Down) { sel ^= 1; break; }
        if (e == Input::Event::Back) return false;
        if (e == Input::Event::Ok) return sel == 1;
        delay(5);
      }
    }
  }
};

inline UI *UI::self_ = nullptr;
const char *const UI::MENU[] = {
    "View info", "Read full", "Hex view", "Write block", "Clone card", "Magic check", "Erase data"};
const int UI::MENU_N = sizeof(UI::MENU) / sizeof(UI::MENU[0]);
