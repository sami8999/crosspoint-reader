// The row store behind BrainListActivity: what a loaded list holds, how a row's
// state becomes something you can see on a 1-bit screen, and what a tap changes
// before the phone has confirmed anything.

#include "companion/brain/ListStore.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Goldens.h"
#include "companion_ble/FakePorts.h"

namespace companion::brain {
namespace {

using companion::test::fromHex;

// U+0336 COMBINING LONG STROKE OVERLAY - the strike-through mark.
constexpr const char* kStrike = "\xCC\xB6";

struct Fixture {
  companion::test::FakeFs fs;
  companion::test::FakeSys sys;
  ListStore store{sys};

  ParseError load(const char* hex, const char* path = "/.companion/lists/todos.list") {
    const std::vector<uint8_t> bytes = fromHex(hex);
    fs.mkdirs("/.companion/lists");
    fs.writeAll(path, bytes.data(), bytes.size());
    return store.load(fs, path, static_cast<uint32_t>(bytes.size()));
  }
  std::string display(uint32_t i) {
    const char* s = store.str(store.row(i).display);
    return s ? s : "";
  }
};

TEST(ListStore, LoadsHeaderAndRows) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenTodos));
  EXPECT_STREQ("todos", f.store.header().listId);
  EXPECT_EQ(2u, f.store.count());
  EXPECT_STREQ("rem-1", f.store.str(f.store.row(0).id));
  EXPECT_STREQ("Call Sam", f.store.str(f.store.row(0).primary));
  EXPECT_STREQ("About the print run", f.store.str(f.store.row(0).secondary));
  EXPECT_STREQ("17:00", f.store.str(f.store.row(0).meta));
  EXPECT_STREQ("!", f.store.str(f.store.row(0).badge));
  // Absent optional strings are null, never a dangling pointer into the arena.
  EXPECT_EQ(nullptr, f.store.str(f.store.row(1).secondary));
  EXPECT_EQ(nullptr, f.store.str(f.store.row(1).badge));
}

TEST(ListStore, AnEmptyListNeedsNoStorageAndStillHasItsHeader) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenEmpty, "/.companion/lists/empty.list"));
  EXPECT_STREQ("empty", f.store.header().listId);
  EXPECT_STREQ("Nothing", f.store.header().title);
  EXPECT_EQ(0u, f.store.count());
}

TEST(ListStore, DoneRowsAreStruckThrough) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenTodos));
  // rem-2 carries flags = done.
  const std::string struck = f.display(1);
  EXPECT_NE(std::string::npos, struck.find(kStrike));
  // One mark per non-space character of "Push firmware" (13 chars, one space).
  EXPECT_EQ(std::string("Push firmware").size() + 12 * 2, struck.size());
  // Spaces are left alone so the strike does not read as a solid bar.
  EXPECT_EQ(std::string::npos, struck.find(std::string(" ") + kStrike));
}

TEST(ListStore, PinnedRowsCarryAMarker) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenTodos));
  // rem-1 is pinned, not done.
  const std::string display = f.display(0);
  EXPECT_NE(std::string::npos, display.find("Call Sam"));
  EXPECT_NE("Call Sam", display) << "a pinned row must be distinguishable";
  EXPECT_EQ(std::string::npos, display.find(kStrike));
}

TEST(ListStore, UnreadRowsCarryADot) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenInbox, "/.companion/lists/inbox.list"));
  // th-1 is unread, th-2 plain, th-3 done.
  EXPECT_NE("Mum", f.display(0));
  EXPECT_NE(std::string::npos, f.display(0).find("Mum"));
  EXPECT_EQ("Standup moved", f.display(1)) << "a plain row is drawn as written";
  EXPECT_NE(std::string::npos, f.display(2).find(kStrike));
}

TEST(ListStore, ResolvesActionsAgainstTheListDefault) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenTodos));
  EXPECT_EQ(0x206u, f.store.actionsFor(0));  // the row's own mask
  EXPECT_EQ(0x106u, f.store.actionsFor(1));
}

TEST(ListStore, CompleteStrikesTheRowThroughLocally) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenTodos));
  ASSERT_EQ(std::string::npos, f.display(0).find(kStrike));
  EXPECT_TRUE(f.store.applyOptimistic(0, /*complete=*/1));
  EXPECT_NE(std::string::npos, f.display(0).find(kStrike));
  EXPECT_TRUE(f.store.row(0).flags & flags::kDone);
}

TEST(ListStore, ArchiveAndDeleteAlsoFinishARow) {
  for (uint8_t action : {uint8_t{4}, uint8_t{8}}) {
    Fixture f;
    ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenInbox, "/.companion/lists/inbox.list"));
    EXPECT_TRUE(f.store.applyOptimistic(0, action)) << "action " << int(action);
    EXPECT_TRUE(f.store.row(0).flags & flags::kDone);
    EXPECT_FALSE(f.store.row(0).flags & flags::kUnread);
  }
}

TEST(ListStore, OpenAndReplyOnlyClearUnread) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenInbox, "/.companion/lists/inbox.list"));
  ASSERT_TRUE(f.store.row(0).flags & flags::kUnread);
  EXPECT_TRUE(f.store.applyOptimistic(0, /*reply=*/5));
  EXPECT_FALSE(f.store.row(0).flags & flags::kUnread);
  EXPECT_FALSE(f.store.row(0).flags & flags::kDone) << "a reply does not finish a thread";
  // Nothing left to change: the screen must not be asked to repaint again.
  EXPECT_FALSE(f.store.applyOptimistic(0, 5));
}

TEST(ListStore, AcceptAndDeclineDoNotClaimSuccess) {
  // EventKit cannot RSVP (docs/INTEGRATION.md), so the reader must not draw an
  // invitation as settled when the user picks accept or decline.
  Fixture f;
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenInbox, "/.companion/lists/inbox.list"));
  f.store.applyOptimistic(1, /*accept=*/6);
  EXPECT_FALSE(f.store.row(1).flags & flags::kDone);
  f.store.applyOptimistic(1, /*decline=*/7);
  EXPECT_FALSE(f.store.row(1).flags & flags::kDone);
}

TEST(ListStore, MarkingOutOfRangeIsRefused) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenTodos));
  EXPECT_FALSE(f.store.applyOptimistic(99, 1));
}

TEST(ListStore, AMalformedFileLeavesNothingBehind) {
  Fixture f;
  std::vector<uint8_t> bytes = fromHex(companion::test::kGoldenTodos);
  bytes[2] = 0x09;  // version 9
  f.fs.mkdirs("/.companion/lists");
  f.fs.writeAll("/.companion/lists/bad.list", bytes.data(), bytes.size());
  EXPECT_EQ(ParseError::BadVersion, f.store.load(f.fs, "/.companion/lists/bad.list", bytes.size()));
  EXPECT_EQ(0u, f.store.count());
  EXPECT_EQ(0u, f.sys.live) << "a failed load must not leak its arena";
}

TEST(ListStore, AMissingFileIsAnIoError) {
  Fixture f;
  EXPECT_EQ(ParseError::Io, f.store.load(f.fs, "/.companion/lists/nope.list", 0));
  EXPECT_EQ(0u, f.store.count());
}

TEST(ListStore, ReleaseGivesEveryBlockBack) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenInbox, "/.companion/lists/inbox.list"));
  EXPECT_GT(f.sys.live, 0u);
  f.store.release();
  EXPECT_EQ(0u, f.sys.live);
  EXPECT_EQ(0u, f.store.count());
}

TEST(ListStore, LoadingASecondListReplacesTheFirst) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenTodos));
  ASSERT_EQ(2u, f.store.count());
  ASSERT_EQ(ParseError::None, f.load(companion::test::kGoldenInbox, "/.companion/lists/inbox.list"));
  EXPECT_EQ(3u, f.store.count());
  EXPECT_STREQ("inbox", f.store.header().listId);
}

}  // namespace
}  // namespace companion::brain
