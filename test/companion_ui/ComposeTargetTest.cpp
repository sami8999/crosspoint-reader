// The Compose target grammar shared with the phone's SyncEngine
// (docs/INTEGRATION.md). Parse and format must round-trip byte-for-byte, and
// anything the phone cannot route must be refused on this side.

#include "companion/brain/ComposeTarget.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace companion::brain {
namespace {

std::string roundTrip(const char* text) {
  ComposeTarget t;
  if (!ComposeTarget::parse(text, t)) return "<invalid>";
  char out[ComposeTarget::kMaxText + 1] = {};
  if (!t.format(out, sizeof(out))) return "<unformattable>";
  return out;
}

TEST(ComposeTarget, EveryDocumentedFormRoundTrips) {
  for (const char* text : {"thread:6E9B0C7A-1F2D-4E3B-9A11-0C2D4E6F8A00", "todo:6e9b0c7a-1f2d", "todo:new",
                           "diary:2026-09-04", "note:abc-123", "note:new", "person:sam"}) {
    EXPECT_EQ(std::string(text), roundTrip(text)) << text;
  }
}

TEST(ComposeTarget, KindsDecodeToTheirEnum) {
  ComposeTarget t;
  ASSERT_TRUE(ComposeTarget::parse("thread:x", t));
  EXPECT_EQ(ComposeKind::Thread, t.kind);
  EXPECT_STREQ("x", t.id);
  ASSERT_TRUE(ComposeTarget::parse("diary:2026-01-31", t));
  EXPECT_EQ(ComposeKind::Diary, t.kind);
  ASSERT_TRUE(ComposeTarget::parse("person:p1", t));
  EXPECT_EQ(ComposeKind::Person, t.kind);
}

TEST(ComposeTarget, NewIsOnlyValidForTheKindsThatCanBeCreatedFromTheReader) {
  ComposeTarget t;
  ASSERT_TRUE(ComposeTarget::parse("todo:new", t));
  EXPECT_TRUE(t.isNew());
  ASSERT_TRUE(ComposeTarget::parse("note:new", t));
  EXPECT_TRUE(t.isNew());
  // A reply has to have a thread; there is no "new thread" route on the phone.
  EXPECT_FALSE(ComposeTarget::parse("thread:new", t));
  EXPECT_FALSE(ComposeTarget::parse("person:new", t));
  ASSERT_TRUE(ComposeTarget::parse("todo:notnew", t));
  EXPECT_FALSE(t.isNew());
}

TEST(ComposeTarget, DiaryDemandsAnIsoDate) {
  ComposeTarget t;
  EXPECT_TRUE(ComposeTarget::parse("diary:2026-12-31", t));
  EXPECT_FALSE(ComposeTarget::parse("diary:2026-13-01", t));  // month 13
  EXPECT_FALSE(ComposeTarget::parse("diary:2026-00-01", t));
  EXPECT_FALSE(ComposeTarget::parse("diary:2026-01-32", t));
  EXPECT_FALSE(ComposeTarget::parse("diary:2026-1-1", t));  // not zero-padded
  EXPECT_FALSE(ComposeTarget::parse("diary:today", t));
}

TEST(ComposeTarget, RejectsUnknownKindsAndMalformedText) {
  ComposeTarget t;
  EXPECT_FALSE(ComposeTarget::parse(nullptr, t));
  EXPECT_FALSE(ComposeTarget::parse("", t));
  EXPECT_FALSE(ComposeTarget::parse("todo", t));      // no colon
  EXPECT_FALSE(ComposeTarget::parse("todo:", t));     // empty id
  EXPECT_FALSE(ComposeTarget::parse(":abc", t));      // no kind
  EXPECT_FALSE(ComposeTarget::parse("inbox:abc", t))  // not in the grammar
      << "unknown kinds must not reach the phone";
  EXPECT_FALSE(ComposeTarget::parse("TODO:abc", t));  // case-sensitive
  EXPECT_FALSE(ComposeTarget::parse("todos:abc", t));
}

TEST(ComposeTarget, RejectsIdsWithSpacesControlBytesOrExtraColons) {
  ComposeTarget t;
  EXPECT_FALSE(ComposeTarget::parse("todo:a b", t));
  EXPECT_FALSE(ComposeTarget::parse("todo:a\tb", t));
  EXPECT_FALSE(ComposeTarget::parse("todo:a\nb", t));
  EXPECT_FALSE(ComposeTarget::parse("thread:a:b", t)) << "a second colon makes the split ambiguous";
}

TEST(ComposeTarget, RejectsAnOverLongId) {
  ComposeTarget t;
  const std::string ok = "todo:" + std::string(ComposeTarget::kMaxId, 'a');
  const std::string tooLong = "todo:" + std::string(ComposeTarget::kMaxId + 1, 'a');
  EXPECT_TRUE(ComposeTarget::parse(ok.c_str(), t));
  EXPECT_FALSE(ComposeTarget::parse(tooLong.c_str(), t));
}

TEST(ComposeTarget, FormatRefusesABufferThatIsTooSmall) {
  ComposeTarget t;
  ASSERT_TRUE(ComposeTarget::parse("person:sam", t));
  char tight[11] = {};  // "person:sam" is 10 bytes + NUL
  EXPECT_TRUE(t.format(tight, sizeof(tight)));
  char tooSmall[10] = {};
  EXPECT_FALSE(t.format(tooSmall, sizeof(tooSmall)));
}

TEST(ComposeTarget, AnUnparsedTargetFormatsToNothing) {
  ComposeTarget t;
  char out[ComposeTarget::kMaxText + 1] = {};
  EXPECT_FALSE(t.valid());
  EXPECT_FALSE(t.format(out, sizeof(out)));
}

TEST(ComposeTarget, DerivesTheTargetFromTheListId) {
  ComposeTarget t;
  ASSERT_TRUE(composeTargetForList("inbox", "th-1", t));
  EXPECT_EQ(ComposeKind::Thread, t.kind);
  EXPECT_STREQ("th-1", t.id);
  ASSERT_TRUE(composeTargetForList("todos", "rem-1", t));
  EXPECT_EQ(ComposeKind::Todo, t.kind);
  ASSERT_TRUE(composeTargetForList("diary", "2026-09-04", t));
  EXPECT_EQ(ComposeKind::Diary, t.kind);
  ASSERT_TRUE(composeTargetForList("notes", "n1", t));
  EXPECT_EQ(ComposeKind::Note, t.kind);
  ASSERT_TRUE(composeTargetForList("people", "p1", t));
  EXPECT_EQ(ComposeKind::Person, t.kind);
}

TEST(ComposeTarget, AListIdWithNoComposeRouteIsRefused) {
  ComposeTarget t;
  EXPECT_FALSE(composeTargetForList("stats", "x", t));
  EXPECT_FALSE(composeTargetForList("", "x", t));
  EXPECT_FALSE(composeTargetForList(nullptr, "x", t));
  EXPECT_FALSE(composeTargetForList("todos", nullptr, t));
}

TEST(ComposeTarget, ADerivedTargetGetsTheSameValidationAsAParsedOne) {
  ComposeTarget t;
  // A diary list whose item id is not a date has nowhere to go.
  EXPECT_FALSE(composeTargetForList("diary", "yesterday", t));
  // Item ids are printable-ASCII-no-space by the format; one that is not must
  // not become a target either.
  EXPECT_FALSE(composeTargetForList("todos", "a b", t));
  EXPECT_FALSE(composeTargetForList("inbox", "a:b", t));
}

}  // namespace
}  // namespace companion::brain
