// A scanned card: identity (UID, type, frequency) and, for MIFARE Classic, a full
// dump with the keys found per sector. The high-level read/write/clone operations.
#pragma once
#include <Arduino.h>
#include "pn532.h"
#include "keystore.h"

constexpr int MAX_SECTORS = 40;   // 4K has 40 sectors
constexpr int MAX_BLOCKS = 256;   // 4K: 255 blocks; keep room

enum class CardKind : uint8_t { Unknown, Classic1K, Classic4K, ClassicMini, UltralightNTAG, DESFire, SmartMX };

struct CardInfo {
  PN532::Target t;
  CardKind kind = CardKind::Unknown;
  PN532::Magic magic = PN532::Magic::Unknown;
  uint16_t sectors = 0;    // Classic
  uint16_t blocks = 0;     // total blocks (Classic)
  uint32_t bytesTotal = 0;

  String uidHex() const {
    String s;
    char h[3];
    for (int i = 0; i < t.uidLen; i++) { snprintf(h, sizeof h, "%02X", t.uid[i]); s += h; }
    return s;
  }
  String uidColon() const {
    String s;
    char h[4];
    for (int i = 0; i < t.uidLen; i++) { snprintf(h, sizeof h, i ? ":%02X" : "%02X", t.uid[i]); s += h; }
    return s;
  }
  const char *kindName() const {
    switch (kind) {
      case CardKind::Classic1K: return "MIFARE Classic 1K";
      case CardKind::Classic4K: return "MIFARE Classic 4K";
      case CardKind::ClassicMini: return "MIFARE Mini";
      case CardKind::UltralightNTAG: return "Ultralight / NTAG";
      case CardKind::DESFire: return "DESFire / ISO14443-4";
      case CardKind::SmartMX: return "SmartMX (Classic+4)";
      default: return "Unknown ISO14443A";
    }
  }
  const char *magicName() const {
    switch (magic) {
      case PN532::Magic::Gen1a: return "Magic Gen1a";
      case PN532::Magic::Gen2: return "Magic Gen2/CUID";
      case PN532::Magic::Gen4: return "Magic Gen4/UMC";
      case PN532::Magic::No: return "Normal (locked UID)";
      default: return "not checked";
    }
  }
  bool isClassic() const {
    return kind == CardKind::Classic1K || kind == CardKind::Classic4K ||
           kind == CardKind::ClassicMini || kind == CardKind::SmartMX;
  }
};

// Full dump buffer + per-sector key/read state.
struct Dump {
  uint8_t data[MAX_BLOCKS][16];
  bool read[MAX_BLOCKS] = {false};
  SectorKey keys[MAX_SECTORS];
  uint16_t blocks = 0;
  uint16_t sectors = 0;
  uint16_t readOk = 0;

  static uint8_t firstBlockOf(uint16_t sector) {
    return sector < 32 ? sector * 4 : 128 + (sector - 32) * 16;
  }
  static uint8_t blocksInSector(uint16_t sector) { return sector < 32 ? 4 : 16; }
};

class CardManager {
 public:
  void begin(PN532 *pn, KeyStore *ks) { pn_ = pn; ks_ = ks; }

  // Identify kind/size/frequency from SAK + ATQA.
  void identify(CardInfo &c) {
    switch (c.t.sak) {
      case 0x08: c.kind = CardKind::Classic1K; c.sectors = 16; c.blocks = 64; break;
      case 0x18: c.kind = CardKind::Classic4K; c.sectors = 40; c.blocks = 256; break;
      case 0x09: c.kind = CardKind::ClassicMini; c.sectors = 5; c.blocks = 20; break;
      case 0x28: c.kind = CardKind::SmartMX; c.sectors = 16; c.blocks = 64; break;
      case 0x00: c.kind = CardKind::UltralightNTAG; break;
      case 0x20: c.kind = CardKind::DESFire; break;
      default: c.kind = CardKind::Unknown; break;
    }
    c.bytesTotal = (uint32_t)c.blocks * 16;
  }

  // progress(cur,total) returns true to request cancellation; the loop then stops.
  using Progress = bool (*)(int, int);
  bool cancelled = false;

  // Read the whole Classic card. Returns sectors fully read. Fills dump keys/data.
  int readFull(CardInfo &c, Dump &d, Progress progress = nullptr) {
    d.blocks = c.blocks;
    d.sectors = c.sectors;
    d.readOk = 0;
    SectorKey stored[MAX_SECTORS];
    int nStored = ks_->load(c.uidHex(), stored, MAX_SECTORS);
    int done = 0;
    cancelled = false;
    for (uint16_t s = 0; s < c.sectors; s++) {
      SectorKey sk = s < nStored ? stored[s] : SectorKey{};
      bool okA = tryKeys(c, Dump::firstBlockOf(s), false, sk.a, sk.haveA);
      bool okB = tryKeys(c, Dump::firstBlockOf(s), true, sk.b, sk.haveB);
      sk.haveA = okA;
      sk.haveB = okB;
      d.keys[s] = sk;
      // read every block with whichever key worked
      uint8_t nb = Dump::blocksInSector(s);
      for (uint8_t bi = 0; bi < nb; bi++) {
        uint8_t blk = Dump::firstBlockOf(s) + bi;
        bool got = false;
        if (okA && authRead(c, blk, sk.a, false, d.data[blk])) got = true;
        else if (okB && authRead(c, blk, sk.b, true, d.data[blk])) got = true;
        d.read[blk] = got;
        if (got) d.readOk++;
      }
      if (okA || okB) done++;
      if (progress && progress(s + 1, c.sectors)) { cancelled = true; break; }
    }
    // persist whatever keys we found (even on cancel, so partial work isn't lost)
    ks_->save(c.uidHex(), d.keys, c.sectors);
    return done;
  }

  // Single block read with the dictionary (used by the "read block" screen).
  bool readOneBlock(CardInfo &c, uint8_t block, uint8_t out[16], SectorKey &usedKey) {
    uint16_t s = sectorOf(block);
    SectorKey stored[MAX_SECTORS];
    int nStored = ks_->load(c.uidHex(), stored, MAX_SECTORS);
    SectorKey sk = s < nStored ? stored[s] : SectorKey{};
    uint8_t base = Dump::firstBlockOf(s);
    if (tryKeys(c, base, false, sk.a, sk.haveA) && authRead(c, block, sk.a, false, out)) { sk.haveA = true; usedKey = sk; return true; }
    if (tryKeys(c, base, true, sk.b, sk.haveB) && authRead(c, block, sk.b, true, out)) { sk.haveB = true; usedKey = sk; return true; }
    return false;
  }

  // Write 16 bytes to one block. Refuses sector trailers and block 0 unless allowTrailer/UID path.
  enum class WriteResult { Ok, NoKey, Denied, Failed };
  WriteResult writeOneBlock(CardInfo &c, uint8_t block, const uint8_t in[16], bool allowTrailer) {
    if (block == 0) return WriteResult::Denied;                 // use clone path for UID
    if (isTrailer(block) && !allowTrailer) return WriteResult::Denied;
    uint16_t s = sectorOf(block);
    SectorKey stored[MAX_SECTORS];
    int nStored = ks_->load(c.uidHex(), stored, MAX_SECTORS);
    SectorKey sk = s < nStored ? stored[s] : SectorKey{};
    uint8_t base = Dump::firstBlockOf(s);
    if (tryKeys(c, base, false, sk.a, sk.haveA)) {
      if (pn_->authClassic(c.t, block, sk.a, false) && pn_->writeBlock(block, in)) return WriteResult::Ok;
    }
    if (tryKeys(c, base, true, sk.b, sk.haveB)) {
      if (pn_->authClassic(c.t, block, sk.b, true) && pn_->writeBlock(block, in)) return WriteResult::Ok;
    }
    return (sk.haveA || sk.haveB) ? WriteResult::Failed : WriteResult::NoKey;
  }

  // Clone a stored dump onto the card now in the field. Smart: adapts to target magic.
  struct CloneReport {
    int blocksWritten = 0, blocksSkipped = 0, blocksFailed = 0;
    bool uidCopied = false;
    bool targetMagic = false;
    String note;
  };
  CloneReport clone(const Dump &src, CardInfo &target, Progress progress = nullptr) {
    CloneReport rep;
    cancelled = false;
    target.magic = pn_->detectMagic(target.t);
    rep.targetMagic = target.magic == PN532::Magic::Gen1a || target.magic == PN532::Magic::Gen4;

    // block 0 (UID) first, only onto a magic card
    if (src.read[0]) {
      if (target.magic == PN532::Magic::Gen1a) {
        pn_->fieldReset();
        PN532::Target tt;
        pn_->poll(tt);
        rep.uidCopied = pn_->gen1aWriteBlock0(src.data[0]);
      } else if (target.magic == PN532::Magic::Gen4 || tryGen2Block0(target, src.data[0])) {
        rep.uidCopied = true;
      }
    }
    // data blocks (skip trailers to avoid locking sectors; skip block 0)
    for (uint16_t b = 1; b < src.blocks; b++) {
      if (!src.read[b]) { rep.blocksSkipped++; continue; }
      if (isTrailer(b)) { rep.blocksSkipped++; continue; }
      pn_->fieldReset();
      PN532::Target tt;
      if (!pn_->poll(tt)) { rep.blocksFailed++; continue; }
      target.t = tt;
      WriteResult w = writeOneBlock(target, b, src.data[b], false);
      if (w == WriteResult::Ok) rep.blocksWritten++;
      else rep.blocksFailed++;
      if (progress && progress(b, src.blocks)) { cancelled = true; break; }
    }
    if (cancelled) rep.note = "cancelled";
    else if (!rep.targetMagic && target.magic == PN532::Magic::No)
      rep.note = "UID unchanged (target not magic)";
    return rep;
  }

  static uint16_t sectorOf(uint8_t block) {
    return block < 128 ? block / 4 : 32 + (block - 128) / 16;
  }
  static bool isTrailer(uint8_t block) {
    uint16_t s = sectorOf(block);
    return block == Dump::firstBlockOf(s) + Dump::blocksInSector(s) - 1;
  }

 private:
  PN532 *pn_ = nullptr;
  KeyStore *ks_ = nullptr;

  bool authRead(CardInfo &c, uint8_t block, const uint8_t key[6], bool keyB, uint8_t out[16]) {
    if (!pn_->authClassic(c.t, block, key, keyB)) return false;
    return pn_->readBlock(block, out);
  }
  // Try a sector's key: the one already found (if valid), then the dictionary. Auth
  // on the sector's first block. On success writes the winning key into `key`.
  bool tryKeys(CardInfo &c, uint8_t baseBlock, bool keyB, uint8_t key[6], bool alreadyHave) {
    if (alreadyHave && pn_->authClassic(c.t, baseBlock, key, keyB)) return true;
    for (int i = 0; i < KEY_DICT_N; i++) {
      pn_->fieldReset();
      PN532::Target tt;
      if (!pn_->poll(tt)) continue;
      c.t = tt;
      if (pn_->authClassic(c.t, baseBlock, KEY_DICT[i], keyB)) {
        memcpy(key, KEY_DICT[i], 6);
        return true;
      }
    }
    return false;
  }
  bool tryGen2Block0(CardInfo &target, const uint8_t block0[16]) {
    pn_->fieldReset();
    PN532::Target tt;
    if (!pn_->poll(tt)) return false;
    target.t = tt;
    const uint8_t fkey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (!pn_->authClassic(target.t, 0, fkey, false)) return false;
    return pn_->writeBlock(0, block0);
  }
};
