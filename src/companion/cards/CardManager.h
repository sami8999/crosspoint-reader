#pragma once
#if CROSSPOINT_COMPANION

// Sleep-card manager (PROTOCOL.md §3.1 SetCards, docs/INTEGRATION.md "Sleep cards").
//
// The phone owns the card set. Cards themselves arrive as ordinary PushFile
// transfers into /.sleep/; SetCards only says *which* of them the reader shows:
//
//   mode 0 pin      - copy entries[0] to /sleep.bmp, which SleepActivity prefers
//                     over everything in /.sleep (renderCustomSleepScreen()).
//   mode 1 rotate   - delete /sleep.bmp so the stock random picker runs over
//                     /.sleep, and prune the cards the phone no longer lists.
//   mode 2 schedule - persist each entry's [fromMin, toMin) minute-of-day window
//                     and, at sleep time, copy the entry whose window contains the
//                     current local minute to /sleep.bmp.
//
// Pure logic over the port interfaces (no Arduino, no display), so the whole
// selection/persistence path runs in test/companion_cards/ on the host. The
// ESP-IDF side of the scheduled wake lives in cards/Wake.h.

#include <cstddef>
#include <cstdint>

#include "../ble/Transfer.h"
#include "../port/Ports.h"
#include "../proto/Messages.h"

namespace companion {

class CardManager {
 public:
  static constexpr size_t kMaxEntries = proto::kMaxCardEntries;  // 16, the SetCards decode cap
  static constexpr size_t kMaxPath = Transfer::kMaxPath;         // 200, the PushFile path cap
  static constexpr uint16_t kMinutesPerDay = 1440;
  static constexpr uint16_t kNoMinute = 0xFFFF;

  static constexpr const char* kStorePath = "/.companion/cards.bin";
  static constexpr const char* kStoreTmp = "/.companion/cards.bin.part";
  static constexpr const char* kStoreDir = "/.companion";
  // SleepActivity's root-priority path and the directory the phone pushes into.
  static constexpr const char* kSleepBmp = "/sleep.bmp";
  static constexpr const char* kSleepTmp = "/sleep.bmp.part";
  static constexpr const char* kCardDir = "/.sleep";

  // "CRD1" as little-endian bytes; version follows separately so the magic never
  // has to change for an additive field.
  static constexpr uint32_t kStoreMagic = 0x31445243u;
  static constexpr uint8_t kStoreVersion = 1;
  static constexpr size_t kHeaderBytes = 16;
  static constexpr size_t kCopyBuf = 4096;
  // A timer wake nearer than this is not worth a boot: the device would come back
  // up seconds after falling asleep. Also stops a window that starts "now" from
  // arming a zero-length timer.
  static constexpr uint32_t kMinWakeLeadS = 30;
  // listDir/remove rounds allowed while pruning; one card is removed per round.
  static constexpr uint8_t kMaxPruneRounds = 64;

  enum class Result : uint8_t { Ok, BadRequest, Denied, NotFound, IoError };

  // Whether the reader wakes on the card schedule at all. Auto is the default:
  // on exactly while a schedule with at least one entry is configured.
  enum class WakeMode : uint8_t { Auto = 0, Off = 1, Always = 2 };

  struct WakeConfig {
    WakeMode mode = WakeMode::Auto;
    uint16_t dailyMin = 6 * 60;  // extra daily wake, local minute-of-day; >= kMinutesPerDay disables
    uint16_t windowS = 120;      // seconds to advertise after a scheduled wake
  };

  struct Entry {
    uint16_t fromMin = kNoMinute;
    uint16_t toMin = kNoMinute;
    char path[kMaxPath + 1] = {};
  };

  CardManager(FsPort& fs, SysPort& sys);

  // Loads the persisted set. A missing or corrupt store is not an error: the
  // manager simply starts empty in Rotate mode (stock behaviour).
  bool begin();
  bool ready() const { return ready_; }

  // --- inbound protocol ---------------------------------------------------
  Result apply(const proto::SetCards& msg);
  // Hello carries the phone's UTC offset; the schedule is expressed in local
  // minutes, so it is persisted with the entries.
  void setTimezone(int32_t offsetMin);
  // A completed PushFile. Cards under /.sleep are remembered as the fallback the
  // schedule falls back to when no window matches.
  void onCardPushed(const char* path);

  // --- sleep / wake -------------------------------------------------------
  // Called from enterDeepSleep() before the sleep screen is rendered. In Schedule
  // mode this is what actually puts the right card at /sleep.bmp.
  bool applyForSleep();
  void setWakeConfig(const WakeConfig& cfg) { wake_ = cfg; }
  const WakeConfig& wakeConfig() const { return wake_; }
  bool wakeEnabled() const;
  // Seconds from `nowUnix` to the next wake: the start of the next schedule
  // window, or the daily wake time, whichever comes first. 0 = do not arm.
  uint32_t secondsUntilNextWake(uint32_t nowUnix) const;
  // secondsUntilNextWake() + persists the resulting absolute unix time.
  uint32_t armNextWake(uint32_t nowUnix);
  uint32_t nextWakeUnix() const { return nextWake_; }

  // --- selection (pure, exposed for tests) --------------------------------
  // Half-open [from, to). from == to means "all day". kNoMinute or >= 1440 in
  // either bound never matches.
  static bool windowContains(uint16_t from, uint16_t to, uint16_t minute);
  // Index of the first entry whose window contains `minute`, or -1.
  int selectForMinute(uint16_t minute) const;
  // Local minute-of-day from the RTC and the stored offset; false when unset.
  bool localMinuteOfDay(uint16_t& out) const;

  // --- accessors ----------------------------------------------------------
  proto::CardsMode mode() const { return mode_; }
  size_t entryCount() const { return count_; }
  const Entry& entry(size_t i) const { return entries_[i]; }
  int32_t tzOffsetMin() const { return tz_; }
  const char* activePath() const { return active_; }
  const char* lastPushedPath() const { return lastPushed_; }

 private:
  Result validate(const proto::SetCards& msg);
  Result commit(const proto::SetCards& msg);
  // Deletes everything in /.sleep the phone no longer lists. Rotate only: see
  // the note in apply() for why a schedule does not own the whole directory.
  Result prune();
  bool listed(const char* path) const;
  // Copies `src` over /sleep.bmp through a .part file. No-op when it is already there.
  bool pin(const char* src);
  bool clearPin();
  bool copyFile(const char* src, const char* dst);
  bool save();
  bool load();
  void reset();
  static void setPath(char* dst, const char* src);

  FsPort& fs_;
  SysPort& sys_;

  bool ready_ = false;
  proto::CardsMode mode_ = proto::CardsMode::Rotate;
  size_t count_ = 0;
  int32_t tz_ = 0;
  uint32_t nextWake_ = 0;
  WakeConfig wake_;

  Entry entries_[kMaxEntries];
  char active_[kMaxPath + 1] = {};      // what is currently copied to /sleep.bmp
  char lastPushed_[kMaxPath + 1] = {};  // newest card pushed into /.sleep
  char scratch_[kMaxPath + 1] = {};     // path assembly; a member so no caller pays 200 B of stack
};

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
