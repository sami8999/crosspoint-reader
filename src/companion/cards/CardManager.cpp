#if CROSSPOINT_COMPANION

#include "CardManager.h"

#include <cstdio>
#include <cstring>

#include "../Log.h"

namespace companion {

using namespace proto;

namespace {

void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }

// FAT/exFAT is case-insensitive, so path identity is too (same rule as
// Transfer::writablePath).
bool samePath(const char* a, const char* b) {
  for (;; ++a, ++b) {
    if (fold(*a) != fold(*b)) return false;
    if (!*a) return true;
  }
}

}  // namespace

CardManager::CardManager(FsPort& fs, SysPort& sys) : fs_(fs), sys_(sys) {}

void CardManager::setPath(char* dst, const char* src) {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  const size_t n = strnlen(src, kMaxPath);
  memcpy(dst, src, n);
  dst[n] = '\0';
}

void CardManager::reset() {
  mode_ = CardsMode::Rotate;
  count_ = 0;
  nextWake_ = 0;
  active_[0] = '\0';
  lastPushed_[0] = '\0';
  for (auto& e : entries_) {
    e.fromMin = kNoMinute;
    e.toMin = kNoMinute;
    e.path[0] = '\0';
  }
}

bool CardManager::begin() {
  reset();
  ready_ = true;
  if (!load()) {
    // No store yet (first boot after the fork) or a store this build cannot read:
    // start empty rather than refusing the feature. The next SetCards rewrites it.
    reset();
    CLOG_INF("cards: no usable store, starting empty");
  } else {
    CLOG_INF("cards: mode %u, %u entries, tz %ld", static_cast<unsigned>(mode_), static_cast<unsigned>(count_),
             static_cast<long>(tz_));
  }
  return true;
}

// ---------------------------------------------------------------- persistence
//
// Compact binary rather than JSON, for three reasons:
//  1. src/companion/ carries no JSON codec. ArduinoJson is device-only, so a JSON
//     store would either pull Arduino into the host suite or need a second parser.
//  2. It is read on the deep-sleep path and on the headless scheduled-wake boot,
//     where there is no display, little heap and a hurry to get back to sleep.
//  3. Records are read straight into their slots: a 16 B header on the stack and
//     one path at a time, so nothing here needs a whole-file buffer or a >256 B
//     stack local (CLAUDE.md Resource Protocol 1).
//
// Layout (little-endian):
//   0  magic u32 "CRD1" | 4 version u8 | 5 mode u8 | 6 count u8 | 7 reserved u8
//   8  tzOffsetMin i32  | 12 nextWake u32 (unix s)
//   16 count x { fromMin u16 | toMin u16 | pathLen u8 | path[pathLen] }
//      lastPushedLen u8 | lastPushed[] | activeLen u8 | active[]

bool CardManager::save() {
  if (!ready_) return false;
  fs_.mkdirs(kStoreDir);
  auto f = fs_.openWrite(kStoreTmp);
  if (!f) {
    CLOG_ERR("cards: open %s failed", kStoreTmp);
    return false;
  }
  uint8_t hdr[kHeaderBytes] = {};
  put32(hdr, kStoreMagic);
  hdr[4] = kStoreVersion;
  hdr[5] = static_cast<uint8_t>(mode_);
  hdr[6] = static_cast<uint8_t>(count_);
  put32(hdr + 8, static_cast<uint32_t>(tz_));
  put32(hdr + 12, nextWake_);
  uint32_t off = 0;
  if (!f->writeAt(off, hdr, sizeof(hdr))) return false;
  off += sizeof(hdr);

  uint8_t rec[5];
  for (size_t i = 0; i < count_; ++i) {
    const Entry& e = entries_[i];
    const size_t n = strnlen(e.path, kMaxPath);
    put16(rec, e.fromMin);
    put16(rec + 2, e.toMin);
    rec[4] = static_cast<uint8_t>(n);
    if (!f->writeAt(off, rec, sizeof(rec))) return false;
    off += sizeof(rec);
    if (n && !f->writeAt(off, reinterpret_cast<const uint8_t*>(e.path), n)) return false;
    off += static_cast<uint32_t>(n);
  }
  for (const char* s : {lastPushed_, active_}) {
    const size_t n = strnlen(s, kMaxPath);
    const uint8_t len = static_cast<uint8_t>(n);
    if (!f->writeAt(off, &len, 1)) return false;
    off += 1;
    if (n && !f->writeAt(off, reinterpret_cast<const uint8_t*>(s), n)) return false;
    off += static_cast<uint32_t>(n);
  }
  if (!f->flush()) return false;
  f.reset();  // close before rename
  if (fs_.exists(kStorePath) && !fs_.remove(kStorePath)) return false;
  if (!fs_.rename(kStoreTmp, kStorePath)) {
    CLOG_ERR("cards: rename %s failed", kStoreTmp);
    return false;
  }
  return true;
}

bool CardManager::load() {
  auto f = fs_.openRead(kStorePath);
  if (!f) return false;
  uint8_t hdr[kHeaderBytes];
  size_t got = 0;
  if (!f->readAt(0, hdr, sizeof(hdr), got) || got != sizeof(hdr)) return false;
  if (get32(hdr) != kStoreMagic || hdr[4] != kStoreVersion) return false;
  const uint8_t rawMode = hdr[5];
  if (rawMode > static_cast<uint8_t>(CardsMode::Schedule)) return false;
  const uint8_t rawCount = hdr[6];
  if (rawCount > kMaxEntries) return false;

  mode_ = static_cast<CardsMode>(rawMode);
  tz_ = static_cast<int32_t>(get32(hdr + 8));
  nextWake_ = get32(hdr + 12);

  uint32_t off = sizeof(hdr);
  uint8_t rec[5];
  for (uint8_t i = 0; i < rawCount; ++i) {
    if (!f->readAt(off, rec, sizeof(rec), got) || got != sizeof(rec)) return false;
    off += sizeof(rec);
    const uint8_t n = rec[4];
    if (n > kMaxPath) return false;
    Entry& e = entries_[i];
    e.fromMin = get16(rec);
    e.toMin = get16(rec + 2);
    if (n && (!f->readAt(off, reinterpret_cast<uint8_t*>(e.path), n, got) || got != n)) return false;
    e.path[n] = '\0';
    off += n;
  }
  count_ = rawCount;

  for (char* dst : {lastPushed_, active_}) {
    uint8_t len = 0;
    if (!f->readAt(off, &len, 1, got) || got != 1) return false;
    off += 1;
    if (len > kMaxPath) return false;
    if (len && (!f->readAt(off, reinterpret_cast<uint8_t*>(dst), len, got) || got != len)) return false;
    dst[len] = '\0';
    off += len;
  }
  return true;
}

// ---------------------------------------------------------------- SetCards

CardManager::Result CardManager::validate(const SetCards& msg) {
  if (msg.entryCount > kMaxEntries) return Result::BadRequest;
  // Pin names the card to show, so it needs one. Rotate and Schedule accept an
  // empty list: that is how the phone clears the deck.
  if (msg.mode == CardsMode::Pin && msg.entryCount == 0) return Result::BadRequest;
  if (msg.mode != CardsMode::Pin && msg.mode != CardsMode::Rotate && msg.mode != CardsMode::Schedule) {
    return Result::BadRequest;
  }
  for (size_t i = 0; i < msg.entryCount; ++i) {
    const CardEntry& e = msg.entries[i];
    if (!Transfer::validPath(e.path.data(), e.path.size())) return Result::BadRequest;
    // Same allow-list as PushFile/DeleteFile: /.companion, /.sleep, /Brain. A card
    // outside them is refused exactly as a write to it would be.
    if (!Transfer::writablePath(e.path.data(), e.path.size())) {
      CLOG_ERR("cards: %.*s is outside the companion roots", static_cast<int>(e.path.size()), e.path.data());
      return Result::Denied;
    }
    if (msg.mode == CardsMode::Schedule) {
      if (!e.fromMin || !e.toMin) return Result::BadRequest;
      if (*e.fromMin >= kMinutesPerDay || *e.toMin >= kMinutesPerDay) return Result::BadRequest;
    }
  }
  // Existence is checked only after every path passed the cheap syntactic tests,
  // so a malformed request never touches the card.
  for (size_t i = 0; i < msg.entryCount; ++i) {
    const CardEntry& e = msg.entries[i];
    memcpy(scratch_, e.path.data(), e.path.size());
    scratch_[e.path.size()] = '\0';
    if (!fs_.exists(scratch_)) {
      CLOG_ERR("cards: %s not on the card", scratch_);
      return Result::NotFound;
    }
  }
  return Result::Ok;
}

CardManager::Result CardManager::commit(const SetCards& msg) {
  mode_ = msg.mode;
  count_ = msg.entryCount;
  for (size_t i = 0; i < count_; ++i) {
    const CardEntry& e = msg.entries[i];
    Entry& dst = entries_[i];
    memcpy(dst.path, e.path.data(), e.path.size());
    dst.path[e.path.size()] = '\0';
    dst.fromMin = e.fromMin ? static_cast<uint16_t>(*e.fromMin) : kNoMinute;
    dst.toMin = e.toMin ? static_cast<uint16_t>(*e.toMin) : kNoMinute;
  }
  for (size_t i = count_; i < kMaxEntries; ++i) {
    entries_[i].fromMin = entries_[i].toMin = kNoMinute;
    entries_[i].path[0] = '\0';
  }
  return Result::Ok;
}

CardManager::Result CardManager::apply(const SetCards& msg) {
  if (!ready_) return Result::IoError;
  const Result v = validate(msg);
  if (v != Result::Ok) return v;
  commit(msg);

  Result r = Result::Ok;
  switch (mode_) {
    case CardsMode::Pin:
      // The pinned card wins over /.sleep because SleepActivity checks the root
      // path first (renderCustomSleepScreen()). The rest of the deck is kept, so
      // a later `rotate` still has something to rotate through.
      if (!pin(entries_[0].path)) r = Result::IoError;
      break;
    case CardsMode::Rotate:
      // Nothing pinned: the stock random picker owns /.sleep again.
      if (!clearPin()) r = Result::IoError;
      if (r == Result::Ok) r = prune();
      break;
    case CardsMode::Schedule:
      // Deliberately no prune here. A schedule lists the cards that have a *time*;
      // one pushed without a window is still the freshest thing the phone sent and
      // is what applyForSleep() falls back to when no window matches, so deleting
      // it would throw away the fallback. Only `rotate` owns the whole directory.
      //
      // Apply the current window immediately as well as at sleep time, so a card
      // set while the reader is awake is already in place if power is lost.
      if (!applyForSleep()) r = Result::IoError;
      break;
  }
  if (!save()) {
    CLOG_ERR("cards: store write failed");
    return Result::IoError;
  }
  CLOG_INF("cards: mode %u, %u entries applied", static_cast<unsigned>(mode_), static_cast<unsigned>(count_));
  return r;
}

void CardManager::setTimezone(int32_t offsetMin) {
  if (!ready_ || tz_ == offsetMin) return;
  tz_ = offsetMin;
  save();
}

void CardManager::onCardPushed(const char* path) {
  if (!ready_ || !path || !*path) return;
  const size_t n = strnlen(path, kMaxPath + 1);
  if (n > kMaxPath) return;
  // Only cards count; list files and Brain books share the transfer path.
  static constexpr char kDirPrefix[] = "/.sleep/";
  for (size_t i = 0; i < sizeof(kDirPrefix) - 1; ++i) {
    if (i >= n || fold(path[i]) != kDirPrefix[i]) return;
  }
  if (samePath(lastPushed_, path)) return;
  setPath(lastPushed_, path);
  save();
}

// ---------------------------------------------------------------- pruning

bool CardManager::listed(const char* path) const {
  for (size_t i = 0; i < count_; ++i) {
    if (samePath(entries_[i].path, path)) return true;
  }
  return false;
}

// One unlisted card per listDir pass. Removing entries while a directory is being
// walked is not something FsPort promises to survive (nor SdFat), so each pass
// stops at the first victim, deletes it, and starts again. Card decks are ~10
// files, so the extra listings are cheap and the code has no iterator to invalidate.
CardManager::Result CardManager::prune() {
  for (uint8_t round = 0; round < kMaxPruneRounds; ++round) {
    scratch_[0] = '\0';
    struct Ctx {
      CardManager* self;
      char* out;
    } ctx{this, scratch_};
    const bool opened = fs_.listDir(kCardDir, [](void* user, const DirEntry& e) {
      auto* c = static_cast<Ctx*>(user);
      if (e.isDir) return true;
      char* out = c->out;
      const size_t dirLen = strlen(kCardDir);
      const size_t nameLen = strlen(e.name);
      if (dirLen + 1 + nameLen > kMaxPath) return true;  // cannot be one of ours: paths are capped
      memcpy(out, kCardDir, dirLen);
      out[dirLen] = '/';
      memcpy(out + dirLen + 1, e.name, nameLen + 1);
      if (c->self->listed(out)) {
        out[0] = '\0';
        return true;
      }
      return false;  // stop the walk; this one goes
    }, &ctx);
    if (!opened) return Result::Ok;  // no /.sleep yet: nothing to prune
    if (!scratch_[0]) return Result::Ok;
    fs_.onFileReplaced(scratch_);
    if (!fs_.remove(scratch_)) {
      CLOG_ERR("cards: prune %s failed", scratch_);
      return Result::IoError;
    }
    CLOG_INF("cards: pruned %s", scratch_);
  }
  CLOG_ERR("cards: prune round limit hit");
  return Result::IoError;
}

// ---------------------------------------------------------------- pinning

bool CardManager::copyFile(const char* src, const char* dst) {
  auto in = fs_.openRead(src);
  if (!in) return false;
  auto out = fs_.openWrite(dst);
  if (!out) return false;
  auto* buf = static_cast<uint8_t*>(sys_.allocBig(kCopyBuf));
  if (!buf) {
    CLOG_ERR("cards: OOM copy buffer");
    return false;
  }
  bool ok = true;
  uint32_t off = 0;
  for (;;) {
    size_t got = 0;
    if (!in->readAt(off, buf, kCopyBuf, got)) {
      ok = false;
      break;
    }
    if (got == 0) break;
    if (!out->writeAt(off, buf, got)) {
      ok = false;
      break;
    }
    off += static_cast<uint32_t>(got);
  }
  sys_.freeBig(buf);
  return ok && out->flush();
}

bool CardManager::pin(const char* src) {
  if (!src || !*src) return clearPin();
  // Already the pinned card and the file is still there: nothing to rewrite. This
  // is what keeps a schedule from copying ~48 KB to SD on every single sleep.
  if (samePath(active_, src) && fs_.exists(kSleepBmp)) return true;
  if (!fs_.exists(src)) {
    CLOG_ERR("cards: pin %s missing", src);
    return false;
  }
  if (!copyFile(src, kSleepTmp)) {
    CLOG_ERR("cards: copy %s failed", src);
    if (fs_.exists(kSleepTmp)) fs_.remove(kSleepTmp);
    return false;
  }
  if (fs_.exists(kSleepBmp) && !fs_.remove(kSleepBmp)) return false;
  if (!fs_.rename(kSleepTmp, kSleepBmp)) {
    CLOG_ERR("cards: rename to %s failed", kSleepBmp);
    return false;
  }
  fs_.onFileReplaced(kSleepBmp);
  setPath(active_, src);
  CLOG_INF("cards: pinned %s", src);
  return true;
}

bool CardManager::clearPin() {
  if (fs_.exists(kSleepBmp)) {
    fs_.onFileReplaced(kSleepBmp);
    if (!fs_.remove(kSleepBmp)) return false;
  }
  active_[0] = '\0';
  return true;
}

// ---------------------------------------------------------------- schedule

bool CardManager::windowContains(uint16_t from, uint16_t to, uint16_t minute) {
  if (from >= kMinutesPerDay || to >= kMinutesPerDay || minute >= kMinutesPerDay) return false;
  if (from == to) return true;                          // degenerate window = all day
  if (from < to) return minute >= from && minute < to;  // [from, to)
  return minute >= from || minute < to;                 // wraps midnight, e.g. 1320 -> 360
}

int CardManager::selectForMinute(uint16_t minute) const {
  for (size_t i = 0; i < count_; ++i) {
    if (windowContains(entries_[i].fromMin, entries_[i].toMin, minute)) return static_cast<int>(i);
  }
  return -1;
}

bool CardManager::localMinuteOfDay(uint16_t& out) const {
  const uint32_t now = sys_.unixTime();
  if (!now) return false;  // RTC never set
  const int64_t local = static_cast<int64_t>(now) + static_cast<int64_t>(tz_) * 60;
  const int64_t day = ((local % 86400) + 86400) % 86400;
  out = static_cast<uint16_t>(day / 60);
  return true;
}

bool CardManager::applyForSleep() {
  if (!ready_) return false;
  // Pin and Rotate did their work when SetCards arrived; only the schedule
  // depends on what time it is now.
  if (mode_ != CardsMode::Schedule) return true;

  const char* pick = nullptr;
  uint16_t minute = 0;
  if (localMinuteOfDay(minute)) {
    const int idx = selectForMinute(minute);
    if (idx >= 0) pick = entries_[idx].path;
  }
  if (!pick) {
    // No window matched (or the clock is unset): the most recently pushed card is
    // the phone's freshest intent.
    if (lastPushed_[0] && fs_.exists(lastPushed_)) {
      pick = lastPushed_;
    } else if (count_) {
      pick = entries_[count_ - 1].path;
    }
  }
  const bool ok = pick ? pin(pick) : clearPin();
  if (ok) save();
  return ok;
}

bool CardManager::wakeEnabled() const {
  switch (wake_.mode) {
    case WakeMode::Off: return false;
    case WakeMode::Always: return true;
    case WakeMode::Auto: break;
  }
  return mode_ == CardsMode::Schedule && count_ > 0;
}

uint32_t CardManager::secondsUntilNextWake(uint32_t nowUnix) const {
  if (!wakeEnabled() || !nowUnix) return 0;  // an unset RTC cannot place a local time
  const int64_t local = static_cast<int64_t>(nowUnix) + static_cast<int64_t>(tz_) * 60;
  const int32_t daySec = static_cast<int32_t>(((local % 86400) + 86400) % 86400);

  uint32_t best = 0;
  const auto consider = [&](uint16_t minute) {
    if (minute >= kMinutesPerDay) return;
    int32_t d = static_cast<int32_t>(minute) * 60 - daySec;
    if (d <= 0) d += 86400;
    uint32_t s = static_cast<uint32_t>(d);
    if (s < kMinWakeLeadS) s = kMinWakeLeadS;
    if (!best || s < best) best = s;
  };
  if (mode_ == CardsMode::Schedule) {
    for (size_t i = 0; i < count_; ++i) consider(entries_[i].fromMin);
  }
  consider(wake_.dailyMin);
  return best;
}

uint32_t CardManager::armNextWake(uint32_t nowUnix) {
  const uint32_t s = secondsUntilNextWake(nowUnix);
  const uint32_t target = s ? nowUnix + s : 0;
  if (target != nextWake_) {
    nextWake_ = target;
    save();
  }
  return s;
}

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
