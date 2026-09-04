// Schedule-window selection (including windows that wrap midnight), the no-match
// fallback, and the next-wake arithmetic.

#include <gtest/gtest.h>

#include <memory>

#include "FakePorts.h"
#include "companion/cards/CardManager.h"

using namespace companion;
using namespace companion::proto;
using companion::test::FakeFs;
using companion::test::FakeSys;

namespace {

constexpr uint32_t kMidnightUtc = 1700006400;  // 2023-11-15 00:00:00 UTC

struct Fixture {
  FakeFs fs;
  FakeSys sys;
  std::unique_ptr<CardManager> cards = std::make_unique<CardManager>(fs, sys);

  Fixture() {
    fs.mkdirs("/.sleep");
    EXPECT_TRUE(cards->begin());
  }
  void addCard(const char* path) { fs.files[path] = std::vector<uint8_t>{static_cast<uint8_t>(path[8]), 'M'}; }
  // Puts the RTC at `minute` local, with the manager's stored UTC offset applied.
  void setLocalMinute(uint16_t minute) { setLocalSecond(minute * 60u); }
  void setLocalSecond(uint32_t secondOfDay) {
    sys.unix = kMidnightUtc + secondOfDay - static_cast<uint32_t>(cards->tzOffsetMin() * 60);
  }
};

// from, to, entries are (path, fromMin, toMin).
struct Win {
  const char* path;
  uint32_t from;
  uint32_t to;
};

SetCards schedule(std::initializer_list<Win> wins) {
  SetCards m;
  m.mode = CardsMode::Schedule;
  m.entryCount = 0;
  for (const Win& w : wins) {
    CardEntry& e = m.entries[m.entryCount++];
    e.path = std::string_view(w.path);
    e.fromMin = w.from;
    e.toMin = w.to;
  }
  return m;
}

}  // namespace

TEST(Schedule, WindowContainsIsHalfOpen) {
  EXPECT_TRUE(CardManager::windowContains(360, 1080, 360));   // inclusive start
  EXPECT_TRUE(CardManager::windowContains(360, 1080, 1079));
  EXPECT_FALSE(CardManager::windowContains(360, 1080, 1080));  // exclusive end
  EXPECT_FALSE(CardManager::windowContains(360, 1080, 359));
  EXPECT_FALSE(CardManager::windowContains(360, 1080, 0));
}

TEST(Schedule, WindowContainsWrapsAroundMidnight) {
  // 22:00 -> 06:00
  EXPECT_TRUE(CardManager::windowContains(1320, 360, 1320));
  EXPECT_TRUE(CardManager::windowContains(1320, 360, 1439));
  EXPECT_TRUE(CardManager::windowContains(1320, 360, 0));
  EXPECT_TRUE(CardManager::windowContains(1320, 360, 359));
  EXPECT_FALSE(CardManager::windowContains(1320, 360, 360));
  EXPECT_FALSE(CardManager::windowContains(1320, 360, 1319));
  EXPECT_FALSE(CardManager::windowContains(1320, 360, 720));
}

TEST(Schedule, DegenerateWindowIsAllDayAndOutOfRangeNeverMatches) {
  EXPECT_TRUE(CardManager::windowContains(0, 0, 0));
  EXPECT_TRUE(CardManager::windowContains(600, 600, 1439));
  EXPECT_FALSE(CardManager::windowContains(CardManager::kNoMinute, 60, 30));
  EXPECT_FALSE(CardManager::windowContains(60, CardManager::kNoMinute, 30));
  EXPECT_FALSE(CardManager::windowContains(60, 120, 1440));
}

TEST(Schedule, SelectionPicksTheFirstMatchingWindow) {
  Fixture f;
  f.addCard("/.sleep/morning.bmp");
  f.addCard("/.sleep/day.bmp");
  f.addCard("/.sleep/night.bmp");
  ASSERT_EQ(f.cards->apply(schedule({{"/.sleep/morning.bmp", 360, 720},
                                     {"/.sleep/day.bmp", 720, 1320},
                                     {"/.sleep/night.bmp", 1320, 360}})),
            CardManager::Result::Ok);
  EXPECT_EQ(f.cards->selectForMinute(360), 0);
  EXPECT_EQ(f.cards->selectForMinute(700), 0);
  EXPECT_EQ(f.cards->selectForMinute(720), 1);
  EXPECT_EQ(f.cards->selectForMinute(1319), 1);
  EXPECT_EQ(f.cards->selectForMinute(1320), 2);
  EXPECT_EQ(f.cards->selectForMinute(30), 2);   // after midnight, still the night window
  EXPECT_EQ(f.cards->selectForMinute(359), 2);
}

TEST(Schedule, SleepPinsTheCardForTheCurrentWindow) {
  Fixture f;
  f.cards->setTimezone(-300);  // UTC-5
  f.addCard("/.sleep/morning.bmp");
  f.addCard("/.sleep/night.bmp");
  f.setLocalMinute(480);  // 08:00 local
  ASSERT_EQ(f.cards->apply(schedule({{"/.sleep/morning.bmp", 360, 1320}, {"/.sleep/night.bmp", 1320, 360}})),
            CardManager::Result::Ok);
  EXPECT_STREQ(f.cards->activePath(), "/.sleep/morning.bmp");

  // 23:00 local: the wrap-around window takes over on the next sleep.
  f.setLocalMinute(1380);
  EXPECT_TRUE(f.cards->applyForSleep());
  EXPECT_STREQ(f.cards->activePath(), "/.sleep/night.bmp");
  EXPECT_EQ(f.fs.files["/sleep.bmp"], f.fs.files["/.sleep/night.bmp"]);

  // 02:00 local, i.e. the previous UTC day: still the night window.
  f.setLocalMinute(120);
  EXPECT_TRUE(f.cards->applyForSleep());
  EXPECT_STREQ(f.cards->activePath(), "/.sleep/night.bmp");
}

TEST(Schedule, NoMatchingWindowFallsBackToTheMostRecentlyPushedCard) {
  Fixture f;
  f.addCard("/.sleep/a.bmp");
  f.addCard("/.sleep/b.bmp");
  f.addCard("/.sleep/fresh.bmp");
  f.cards->onCardPushed("/.sleep/fresh.bmp");
  f.setLocalMinute(700);  // 11:40, outside both windows below
  ASSERT_EQ(f.cards->apply(schedule({{"/.sleep/a.bmp", 0, 60}, {"/.sleep/b.bmp", 1380, 1440 - 1}})),
            CardManager::Result::Ok);
  EXPECT_STREQ(f.cards->activePath(), "/.sleep/fresh.bmp");
  EXPECT_EQ(f.fs.files["/sleep.bmp"], f.fs.files["/.sleep/fresh.bmp"]);
}

TEST(Schedule, AnUnsetClockAlsoFallsBackToTheMostRecentlyPushedCard) {
  Fixture f;
  f.addCard("/.sleep/a.bmp");
  f.addCard("/.sleep/fresh.bmp");
  f.cards->onCardPushed("/.sleep/fresh.bmp");
  f.sys.unix = 0;  // RTC never set
  ASSERT_EQ(f.cards->apply(schedule({{"/.sleep/a.bmp", 0, 60}})), CardManager::Result::Ok);
  EXPECT_STREQ(f.cards->activePath(), "/.sleep/fresh.bmp");
}

TEST(Schedule, WithNothingPushedTheFallbackIsTheLastListedEntry) {
  Fixture f;
  f.addCard("/.sleep/a.bmp");
  f.addCard("/.sleep/z.bmp");
  f.sys.unix = 0;
  ASSERT_EQ(f.cards->apply(schedule({{"/.sleep/a.bmp", 0, 60}, {"/.sleep/z.bmp", 60, 120}})),
            CardManager::Result::Ok);
  EXPECT_STREQ(f.cards->activePath(), "/.sleep/z.bmp");
}

TEST(Schedule, AFallbackCardThatIsNoLongerOnTheCardIsSkipped) {
  Fixture f;
  f.addCard("/.sleep/a.bmp");
  f.addCard("/.sleep/fresh.bmp");
  f.cards->onCardPushed("/.sleep/fresh.bmp");
  f.sys.unix = 0;
  ASSERT_EQ(f.cards->apply(schedule({{"/.sleep/a.bmp", 0, 60}})), CardManager::Result::Ok);
  ASSERT_STREQ(f.cards->activePath(), "/.sleep/fresh.bmp");
  // A DeleteFile (or a later rotate) took the fallback away.
  f.fs.files.erase("/.sleep/fresh.bmp");
  EXPECT_TRUE(f.cards->applyForSleep());
  EXPECT_STREQ(f.cards->activePath(), "/.sleep/a.bmp");
}

TEST(Schedule, AScheduleDoesNotPruneUnlistedCards) {
  Fixture f;
  f.addCard("/.sleep/a.bmp");
  f.addCard("/.sleep/spare.bmp");
  f.sys.unix = 0;
  ASSERT_EQ(f.cards->apply(schedule({{"/.sleep/a.bmp", 0, 60}})), CardManager::Result::Ok);
  // Only `rotate` owns the whole directory; a schedule keeps the fallback around.
  EXPECT_TRUE(f.fs.files.count("/.sleep/spare.bmp"));
}

TEST(Schedule, ScheduleEntriesMustCarryBothBoundsInRange) {
  Fixture f;
  f.addCard("/.sleep/a.bmp");
  SetCards m;
  m.mode = CardsMode::Schedule;
  m.entryCount = 1;
  m.entries[0].path = "/.sleep/a.bmp";
  EXPECT_EQ(f.cards->apply(m), CardManager::Result::BadRequest);  // no bounds
  m.entries[0].fromMin = 60;
  EXPECT_EQ(f.cards->apply(m), CardManager::Result::BadRequest);  // only one bound
  m.entries[0].toMin = 1440;
  EXPECT_EQ(f.cards->apply(m), CardManager::Result::BadRequest);  // out of range
  m.entries[0].toMin = 1439;
  EXPECT_EQ(f.cards->apply(m), CardManager::Result::Ok);
}

TEST(Schedule, ModesOtherThanScheduleDoNoWorkAtSleepTime) {
  Fixture f;
  f.addCard("/.sleep/a.bmp");
  ASSERT_EQ(f.cards->apply(schedule({})), CardManager::Result::Ok);  // empty schedule
  f.fs.files["/sleep.bmp"] = std::vector<uint8_t>{'X'};
  SetCards m;
  m.mode = CardsMode::Pin;
  m.entryCount = 1;
  m.entries[0].path = "/.sleep/a.bmp";
  ASSERT_EQ(f.cards->apply(m), CardManager::Result::Ok);
  const auto pinned = f.fs.files["/sleep.bmp"];
  EXPECT_TRUE(f.cards->applyForSleep());
  EXPECT_EQ(f.fs.files["/sleep.bmp"], pinned);  // unchanged: a pin is not time-dependent
}

// ---------------------------------------------------------------- next wake

TEST(Wake, AutoIsOnOnlyWhileAScheduleExists) {
  Fixture f;
  f.addCard("/.sleep/a.bmp");
  EXPECT_FALSE(f.cards->wakeEnabled());  // rotate, no entries
  ASSERT_EQ(f.cards->apply(schedule({{"/.sleep/a.bmp", 360, 720}})), CardManager::Result::Ok);
  EXPECT_TRUE(f.cards->wakeEnabled());

  CardManager::WakeConfig cfg;
  cfg.mode = CardManager::WakeMode::Off;
  f.cards->setWakeConfig(cfg);
  EXPECT_FALSE(f.cards->wakeEnabled());
  EXPECT_EQ(f.cards->secondsUntilNextWake(f.sys.unix), 0u);

  cfg.mode = CardManager::WakeMode::Always;
  f.cards->setWakeConfig(cfg);
  SetCards rot;
  rot.mode = CardsMode::Rotate;
  rot.entryCount = 0;
  ASSERT_EQ(f.cards->apply(rot), CardManager::Result::Ok);
  EXPECT_TRUE(f.cards->wakeEnabled());  // forced on even with no schedule
}

TEST(Wake, TheNextWakeIsTheEarlierOfAWindowStartAndTheDailyTime) {
  Fixture f;
  f.addCard("/.sleep/a.bmp");
  f.addCard("/.sleep/b.bmp");
  ASSERT_EQ(f.cards->apply(schedule({{"/.sleep/a.bmp", 480, 1080}, {"/.sleep/b.bmp", 1080, 480}})),
            CardManager::Result::Ok);
  // Default daily wake is 06:00 (360).
  f.setLocalMinute(300);  // 05:00 -> the 06:00 daily wake is an hour away
  EXPECT_EQ(f.cards->secondsUntilNextWake(f.sys.unix), 60u * 60u);
  f.setLocalMinute(400);  // 06:40 -> the 08:00 window start is next
  EXPECT_EQ(f.cards->secondsUntilNextWake(f.sys.unix), 80u * 60u);
  f.setLocalMinute(1100);  // 18:20 -> nothing else today; 06:00 tomorrow
  EXPECT_EQ(f.cards->secondsUntilNextWake(f.sys.unix), (1440u - 1100u + 360u) * 60u);
}

TEST(Wake, ADailyTimeOutOfRangeDisablesJustThatWake) {
  Fixture f;
  f.addCard("/.sleep/a.bmp");
  ASSERT_EQ(f.cards->apply(schedule({{"/.sleep/a.bmp", 480, 1080}})), CardManager::Result::Ok);
  CardManager::WakeConfig cfg;
  cfg.dailyMin = CardManager::kMinutesPerDay;  // 1440 = off
  f.cards->setWakeConfig(cfg);
  f.setLocalMinute(300);
  EXPECT_EQ(f.cards->secondsUntilNextWake(f.sys.unix), 180u * 60u);  // only the 08:00 window
}

TEST(Wake, AWakeThatWouldFireImmediatelyIsHeldOffAndArmingPersists) {
  Fixture f;
  f.addCard("/.sleep/a.bmp");
  ASSERT_EQ(f.cards->apply(schedule({{"/.sleep/a.bmp", 480, 1080}})), CardManager::Result::Ok);
  // A window start already reached needs no wake at all - the device is awake -
  // so it rolls to tomorrow and the 06:00 daily wake wins.
  f.setLocalMinute(480);
  EXPECT_EQ(f.cards->secondsUntilNextWake(f.sys.unix), (1440u - 480u + 360u) * 60u);
  // Ten seconds before it, though, the timer would fire almost immediately.
  f.setLocalSecond(480u * 60u - 10u);
  EXPECT_EQ(f.cards->secondsUntilNextWake(f.sys.unix), CardManager::kMinWakeLeadS);

  f.setLocalMinute(300);
  const uint32_t armed = f.cards->armNextWake(f.sys.unix);
  EXPECT_EQ(armed, 60u * 60u);
  EXPECT_EQ(f.cards->nextWakeUnix(), f.sys.unix + armed);
}

TEST(Wake, AnUnsetClockArmsNothing) {
  Fixture f;
  f.addCard("/.sleep/a.bmp");
  ASSERT_EQ(f.cards->apply(schedule({{"/.sleep/a.bmp", 480, 1080}})), CardManager::Result::Ok);
  EXPECT_EQ(f.cards->secondsUntilNextWake(0), 0u);
  EXPECT_EQ(f.cards->armNextWake(0), 0u);
  EXPECT_EQ(f.cards->nextWakeUnix(), 0u);
}
