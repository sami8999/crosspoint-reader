// Pin / rotate / prune, path rejection and the persistence round-trip, all
// against the in-memory FakeFs shared with test/companion_ble/.

#include <gtest/gtest.h>

#include <memory>

#include "FakePorts.h"
#include "companion/cards/CardManager.h"

using namespace companion;
using namespace companion::proto;
using companion::test::FakeFs;
using companion::test::FakeSys;

namespace {

std::vector<uint8_t> bytes(const char* s) { return std::vector<uint8_t>(s, s + strlen(s)); }

struct Fixture {
  FakeFs fs;
  FakeSys sys;
  // ~3.4 KiB of fixed entry slots: heap, not the test's stack.
  std::unique_ptr<CardManager> cards = std::make_unique<CardManager>(fs, sys);

  Fixture() {
    fs.mkdirs("/.sleep");
    fs.mkdirs("/.companion");
    EXPECT_TRUE(cards->begin());
  }
  void addCard(const char* path, const char* content) { fs.files[path] = bytes(content); }
  // Reloads from the same filesystem, as a reboot would.
  std::unique_ptr<CardManager> reload() {
    auto c = std::make_unique<CardManager>(fs, sys);
    EXPECT_TRUE(c->begin());
    return c;
  }
};

SetCards makeSet(CardsMode mode, std::initializer_list<const char*> paths) {
  SetCards m;
  m.mode = mode;
  m.entryCount = 0;
  for (const char* p : paths) m.entries[m.entryCount++].path = std::string_view(p);
  return m;
}

}  // namespace

TEST(Cards, PinCopiesTheCardToTheRootSleepBmp) {
  Fixture f;
  f.addCard("/.sleep/brief.bmp", "BM-brief");
  f.addCard("/.sleep/clock.bmp", "BM-clock");
  const SetCards m = makeSet(CardsMode::Pin, {"/.sleep/brief.bmp", "/.sleep/clock.bmp"});
  EXPECT_EQ(f.cards->apply(m), CardManager::Result::Ok);
  ASSERT_TRUE(f.fs.files.count("/sleep.bmp"));
  EXPECT_EQ(f.fs.files["/sleep.bmp"], bytes("BM-brief"));
  // The rest of the deck survives a pin: a later rotate still has cards to show.
  EXPECT_TRUE(f.fs.files.count("/.sleep/clock.bmp"));
  EXPECT_STREQ(f.cards->activePath(), "/.sleep/brief.bmp");
  // The cache is invalidated for the file the sleep screen reads.
  EXPECT_NE(std::find(f.fs.replaced.begin(), f.fs.replaced.end(), "/sleep.bmp"), f.fs.replaced.end());
}

TEST(Cards, PinWithNoEntriesIsBadPayload) {
  Fixture f;
  SetCards m;
  m.mode = CardsMode::Pin;
  m.entryCount = 0;
  EXPECT_EQ(f.cards->apply(m), CardManager::Result::BadRequest);
}

TEST(Cards, PinTwiceDoesNotRewriteTheSameCard) {
  Fixture f;
  f.addCard("/.sleep/brief.bmp", "BM-brief");
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"/.sleep/brief.bmp"})), CardManager::Result::Ok);
  const size_t allocs = f.sys.allocs;
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"/.sleep/brief.bmp"})), CardManager::Result::Ok);
  // No copy buffer was taken the second time: the file is already in place.
  EXPECT_EQ(f.sys.allocs, allocs);
  EXPECT_EQ(f.fs.files["/sleep.bmp"], bytes("BM-brief"));
}

TEST(Cards, RotateClearsThePinAndPrunesUnlistedCards) {
  Fixture f;
  f.addCard("/.sleep/a.bmp", "A");
  f.addCard("/.sleep/b.bmp", "B");
  f.addCard("/.sleep/stale.bmp", "OLD");
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"/.sleep/a.bmp"})), CardManager::Result::Ok);
  ASSERT_TRUE(f.fs.files.count("/sleep.bmp"));

  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Rotate, {"/.sleep/a.bmp", "/.sleep/b.bmp"})), CardManager::Result::Ok);
  // /sleep.bmp gone -> SleepActivity falls through to its random /.sleep picker.
  EXPECT_FALSE(f.fs.files.count("/sleep.bmp"));
  EXPECT_TRUE(f.fs.files.count("/.sleep/a.bmp"));
  EXPECT_TRUE(f.fs.files.count("/.sleep/b.bmp"));
  // The phone owns the set, so a card it no longer lists is deleted.
  EXPECT_FALSE(f.fs.files.count("/.sleep/stale.bmp"));
  EXPECT_STREQ(f.cards->activePath(), "");
}

TEST(Cards, RotateWithNoEntriesClearsTheWholeDeck) {
  Fixture f;
  f.addCard("/.sleep/a.bmp", "A");
  f.addCard("/.sleep/b.bmp", "B");
  SetCards m;
  m.mode = CardsMode::Rotate;
  m.entryCount = 0;
  EXPECT_EQ(f.cards->apply(m), CardManager::Result::Ok);
  EXPECT_FALSE(f.fs.files.count("/.sleep/a.bmp"));
  EXPECT_FALSE(f.fs.files.count("/.sleep/b.bmp"));
}

TEST(Cards, PruneLeavesFilesOutsideTheCardDirectoryAlone) {
  Fixture f;
  f.addCard("/.sleep/a.bmp", "A");
  f.addCard("/Brain/Today.epub", "book");
  f.addCard("/.companion/lists/todo.list", "list");
  f.addCard("/mybook.epub", "user");
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Rotate, {"/.sleep/a.bmp"})), CardManager::Result::Ok);
  EXPECT_TRUE(f.fs.files.count("/Brain/Today.epub"));
  EXPECT_TRUE(f.fs.files.count("/.companion/lists/todo.list"));
  EXPECT_TRUE(f.fs.files.count("/mybook.epub"));
}

TEST(Cards, PathsOutsideTheCompanionRootsAreRefused) {
  Fixture f;
  f.addCard("/.crosspoint/settings.json", "{}");
  f.addCard("/mybook.epub", "user");
  // Same allow-list as PushFile/DeleteFile (Transfer::writablePath).
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"/.crosspoint/settings.json"})), CardManager::Result::Denied);
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"/mybook.epub"})), CardManager::Result::Denied);
  EXPECT_FALSE(f.fs.files.count("/sleep.bmp"));
}

TEST(Cards, MalformedPathsAreBadPayload) {
  Fixture f;
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"relative.bmp"})), CardManager::Result::BadRequest);
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"/.sleep/../secret"})), CardManager::Result::BadRequest);
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"/.sleep//a.bmp"})), CardManager::Result::BadRequest);
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"/.sleep/a.bmp/"})), CardManager::Result::BadRequest);
  // Syntax is checked before the allow-list, and neither ever touched the card.
  EXPECT_TRUE(f.fs.replaced.empty());
}

TEST(Cards, AMissingCardIsNotFound) {
  Fixture f;
  f.addCard("/.sleep/a.bmp", "A");
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"/.sleep/a.bmp", "/.sleep/gone.bmp"})),
            CardManager::Result::NotFound);
  // Nothing was applied: the whole request is validated before anything is written.
  EXPECT_FALSE(f.fs.files.count("/sleep.bmp"));
  EXPECT_EQ(f.cards->entryCount(), 0u);
}

TEST(Cards, ARefusedRequestLeavesThePreviousSetIntact) {
  Fixture f;
  f.addCard("/.sleep/a.bmp", "A");
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"/.sleep/a.bmp"})), CardManager::Result::Ok);
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Rotate, {"/nope.bmp"})), CardManager::Result::Denied);
  EXPECT_EQ(f.cards->mode(), CardsMode::Pin);
  EXPECT_EQ(f.cards->entryCount(), 1u);
  EXPECT_TRUE(f.fs.files.count("/sleep.bmp"));
}

TEST(Cards, StoreRoundTripsAcrossAReboot) {
  Fixture f;
  f.addCard("/.sleep/morning.bmp", "M");
  f.addCard("/.sleep/night.bmp", "N");
  f.sys.unix = 1700000000;
  f.cards->setTimezone(-240);
  f.cards->onCardPushed("/.sleep/night.bmp");

  SetCards m;
  m.mode = CardsMode::Schedule;
  m.entryCount = 2;
  m.entries[0].path = "/.sleep/morning.bmp";
  m.entries[0].fromMin = 360;
  m.entries[0].toMin = 1080;
  m.entries[1].path = "/.sleep/night.bmp";
  m.entries[1].fromMin = 1080;
  m.entries[1].toMin = 360;
  EXPECT_EQ(f.cards->apply(m), CardManager::Result::Ok);
  f.cards->armNextWake(f.sys.unix);
  const uint32_t wake = f.cards->nextWakeUnix();
  const std::string active = f.cards->activePath();

  auto reloaded = f.reload();
  EXPECT_EQ(reloaded->mode(), CardsMode::Schedule);
  ASSERT_EQ(reloaded->entryCount(), 2u);
  EXPECT_STREQ(reloaded->entry(0).path, "/.sleep/morning.bmp");
  EXPECT_EQ(reloaded->entry(0).fromMin, 360);
  EXPECT_EQ(reloaded->entry(0).toMin, 1080);
  EXPECT_STREQ(reloaded->entry(1).path, "/.sleep/night.bmp");
  EXPECT_EQ(reloaded->entry(1).fromMin, 1080);
  EXPECT_EQ(reloaded->entry(1).toMin, 360);
  EXPECT_EQ(reloaded->tzOffsetMin(), -240);
  EXPECT_EQ(reloaded->nextWakeUnix(), wake);
  EXPECT_STREQ(reloaded->lastPushedPath(), "/.sleep/night.bmp");
  EXPECT_EQ(std::string(reloaded->activePath()), active);
}

TEST(Cards, AMissingOrCorruptStoreStartsEmptyRatherThanFailing) {
  Fixture f;
  EXPECT_EQ(f.cards->mode(), CardsMode::Rotate);
  EXPECT_EQ(f.cards->entryCount(), 0u);

  f.addCard("/.sleep/a.bmp", "A");
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"/.sleep/a.bmp"})), CardManager::Result::Ok);
  ASSERT_TRUE(f.fs.files.count(CardManager::kStorePath));
  f.fs.files[CardManager::kStorePath][0] = 'X';  // break the magic
  auto reloaded = f.reload();
  EXPECT_EQ(reloaded->mode(), CardsMode::Rotate);
  EXPECT_EQ(reloaded->entryCount(), 0u);
}

TEST(Cards, PushesIntoTheCardDirectoryAreRememberedButOthersAreNot) {
  Fixture f;
  f.cards->onCardPushed("/Brain/Today.epub");
  EXPECT_STREQ(f.cards->lastPushedPath(), "");
  f.cards->onCardPushed("/.companion/lists/todo.list");
  EXPECT_STREQ(f.cards->lastPushedPath(), "");
  f.cards->onCardPushed("/.sleep/fresh.bmp");
  EXPECT_STREQ(f.cards->lastPushedPath(), "/.sleep/fresh.bmp");
  // FAT is case-insensitive, so the directory match is too.
  f.cards->onCardPushed("/.SLEEP/other.bmp");
  EXPECT_STREQ(f.cards->lastPushedPath(), "/.SLEEP/other.bmp");
}

TEST(Cards, AFailingStoreWriteIsAnIoError) {
  Fixture f;
  f.addCard("/.sleep/a.bmp", "A");
  f.fs.failWrites = true;
  EXPECT_EQ(f.cards->apply(makeSet(CardsMode::Pin, {"/.sleep/a.bmp"})), CardManager::Result::IoError);
}
