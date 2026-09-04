// Streaming list-file reader (src/companion/brain/ListFile.*) against files the
// phone's own ListFileBuilder produced, plus the caps, limits and malformed
// input the format has to survive on an SD card the reader does not control.

#include "companion/brain/ListFile.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Goldens.h"
#include "companion_ble/FakePorts.h"

namespace companion::brain {
namespace {

using companion::test::fromHex;

// A ByteSource over a memory buffer that hands out at most `chunk` bytes per
// call, so the window-refill path is exercised at every boundary.
class MemSource final : public ByteSource {
 public:
  MemSource(std::vector<uint8_t> data, size_t chunk = 64) : data_(std::move(data)), chunk_(chunk) {}
  size_t read(uint8_t* out, size_t len) override {
    const size_t take = std::min({len, chunk_, data_.size() - pos_});
    memcpy(out, data_.data() + pos_, take);
    pos_ += take;
    return take;
  }
  size_t pos() const { return pos_; }

 private:
  std::vector<uint8_t> data_;
  size_t chunk_;
  size_t pos_ = 0;
};

struct Collected {
  std::vector<ListRow> rows;
  std::vector<uint32_t> indices;
  uint32_t stopAfter = 0;  // 0 = never stop
};

bool collect(void* user, uint32_t index, const ListRow& row) {
  auto* c = static_cast<Collected*>(user);
  c->rows.push_back(row);
  c->indices.push_back(index);
  return c->stopAfter == 0 || c->rows.size() < c->stopAfter;
}

struct Fixture {
  ListParser parser;
  ListHeader header;
  ListRow scratch;
  Collected out;

  ParseError run(const std::string& hex, size_t chunk = 64) {
    MemSource src(fromHex(hex), chunk);
    return parser.parse(src, header, scratch, collect, &out);
  }
  ParseError runBytes(const std::vector<uint8_t>& bytes, size_t chunk = 64) {
    MemSource src(bytes, chunk);
    return parser.parse(src, header, scratch, collect, &out);
  }
};

// ------------------------------------------------------------------ goldens

TEST(ListFile, ParsesTheWorkedExampleFromTheSpec) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.run(companion::test::kGoldenTodos));
  EXPECT_STREQ("todos", f.header.listId);
  EXPECT_STREQ("Todos", f.header.title);
  EXPECT_EQ(1788000000u, f.header.generatedAt);
  EXPECT_EQ(2u, f.header.itemCount);
  EXPECT_EQ(kActionBit(3), f.header.defaultActions);  // open
  ASSERT_EQ(1u, f.header.sectionCount);
  EXPECT_STREQ("Today", f.header.sections[0].title);
  EXPECT_EQ(0u, f.header.sections[0].start);
  EXPECT_EQ(2u, f.header.sections[0].count);

  ASSERT_EQ(2u, f.out.rows.size());
  EXPECT_EQ(0u, f.out.indices[0]);
  EXPECT_STREQ("rem-1", f.out.rows[0].id);
  EXPECT_STREQ("Call Sam", f.out.rows[0].primary);
  EXPECT_STREQ("About the print run", f.out.rows[0].secondary);
  EXPECT_STREQ("17:00", f.out.rows[0].meta);
  EXPECT_STREQ("!", f.out.rows[0].badge);
  EXPECT_EQ(0x206u, f.out.rows[0].actions);
  EXPECT_EQ(flags::kPinned, f.out.rows[0].flags);

  EXPECT_STREQ("rem-2", f.out.rows[1].id);
  EXPECT_STREQ("Push firmware", f.out.rows[1].primary);
  EXPECT_STREQ("", f.out.rows[1].secondary);  // key 3 omitted, never null
  EXPECT_STREQ("", f.out.rows[1].badge);
  EXPECT_EQ(0x106u, f.out.rows[1].actions);
  EXPECT_EQ(flags::kDone, f.out.rows[1].flags);
}

TEST(ListFile, SurvivesEveryWindowBoundary) {
  // A one-byte-at-a-time source refills the window inside heads, strings and
  // array bodies alike; the result must be identical.
  for (size_t chunk : {size_t{1}, size_t{2}, size_t{3}, size_t{7}, size_t{64}, size_t{4096}}) {
    Fixture f;
    ASSERT_EQ(ParseError::None, f.run(companion::test::kGoldenInbox, chunk)) << "chunk " << chunk;
    ASSERT_EQ(3u, f.out.rows.size());
    EXPECT_STREQ("Are you coming on Sunday?", f.out.rows[0].secondary) << "chunk " << chunk;
    EXPECT_STREQ("Newsletter", f.out.rows[2].primary) << "chunk " << chunk;
  }
}

TEST(ListFile, EmptyListHasNoRowsAndNoDefaultActions) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.run(companion::test::kGoldenEmpty));
  EXPECT_STREQ("empty", f.header.listId);
  EXPECT_EQ(0u, f.header.itemCount);
  EXPECT_EQ(0u, f.header.sectionCount);
  EXPECT_EQ(0u, f.header.defaultActions);
  EXPECT_TRUE(f.out.rows.empty());
}

TEST(ListFile, SectionsCoverContiguousRangesAndLeaveTailRowsUnheaded) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.run(companion::test::kGoldenInbox));
  ASSERT_EQ(2u, f.header.sectionCount);
  EXPECT_EQ(0, f.header.sectionForItem(0));
  EXPECT_EQ(1, f.header.sectionForItem(1));
  EXPECT_EQ(1, f.header.sectionForItem(2));
  EXPECT_EQ(-1, f.header.sectionForItem(3));  // past the last section
}

// -------------------------------------------------------------------- limits

TEST(ListFile, OverCapStringsArriveTruncatedOnACharacterBoundary) {
  Fixture f;
  ASSERT_EQ(ParseError::None, f.run(companion::test::kGoldenCaps));
  // The encoder already cut each string to its cap and appended U+2026 (3
  // bytes); the reader's buffers must hold exactly that, NUL-terminated.
  EXPECT_EQ(limits::kTitle, strlen(f.header.title));
  ASSERT_EQ(1u, f.out.rows.size());
  const ListRow& r = f.out.rows[0];
  EXPECT_EQ(limits::kPrimary, strlen(r.primary));
  EXPECT_EQ(limits::kSecondary, strlen(r.secondary));
  EXPECT_EQ(limits::kMeta, strlen(r.meta));
  EXPECT_EQ(limits::kBadge, strlen(r.badge));  // badge is cut without an ellipsis
  EXPECT_STREQ("NEWNEWNE", r.badge);
  // The ellipsis survived intact: the last three bytes are E2 80 A6.
  const std::string primary(r.primary);
  EXPECT_EQ("\xE2\x80\xA6", primary.substr(primary.size() - 3));
}

TEST(ListFile, AHostileOverLongStringIsCutNotBuffered) {
  // 4 KB of 'x' in `primary` - far past both the cap and the parser's 256-byte
  // window. It must be copied up to the cap, cut on a boundary, and the rest
  // streamed past without any allocation.
  std::vector<uint8_t> b = {0xA7, 0x01, 0x01, 0x02, 0x62, 'h', 'x', 0x03, 0x61, 'T', 0x04, 0x00,
                            0x05, 0x01, 0x06, 0x80, 0x07, 0x81, 0xA2, 0x01, 0x62, 'i', '1', 0x02};
  b.push_back(0x79);  // tstr, 2-byte length
  b.push_back(0x10);
  b.push_back(0x00);  // 4096
  b.insert(b.end(), 4096, 'x');
  Fixture f;
  ASSERT_EQ(ParseError::None, f.runBytes(b));
  ASSERT_EQ(1u, f.out.rows.size());
  EXPECT_EQ(limits::kPrimary, strlen(f.out.rows[0].primary));
}

TEST(ListFile, RejectsMoreItemsThanTheFormatAllows) {
  // itemCount = 501.
  const std::vector<uint8_t> b = {0xA5, 0x01, 0x01, 0x02, 0x62, 'h', 'x', 0x03, 0x61, 'T',
                                  0x04, 0x00, 0x05, 0x19, 0x01, 0xF5, 0x06, 0x80};
  Fixture f;
  EXPECT_EQ(ParseError::TooManyItems, f.runBytes(b));
}

TEST(ListFile, RejectsAnItemCountThatDoesNotMatchTheArray) {
  // itemCount 2, one item.
  const std::vector<uint8_t> b = {0xA7, 0x01, 0x01, 0x02, 0x62, 'h', 'x', 0x03, 0x61, 'T', 0x04, 0x00, 0x05,
                                  0x02, 0x06, 0x80, 0x07, 0x81, 0xA2, 0x01, 0x62, 'i', '1', 0x02, 0x61, 'p'};
  Fixture f;
  EXPECT_EQ(ParseError::CountMismatch, f.runBytes(b));
}

TEST(ListFile, SectionTableOverflowDropsHeadingsInsteadOfFailing) {
  // kMaxSections + 4 one-row sections. The extras must be ignored so the list
  // still renders (un-headed rows) rather than refusing to open.
  const size_t sections = limits::kMaxSections + 4;
  std::vector<uint8_t> b = {0xA7, 0x01, 0x01, 0x02, 0x62, 'h', 'x', 0x03, 0x61, 'T', 0x04, 0x00, 0x05, 0x00, 0x06};
  b.push_back(0x98);  // array, 1-byte length
  b.push_back(static_cast<uint8_t>(sections));
  for (size_t i = 0; i < sections; ++i) {
    (void)i;
    const uint8_t s[] = {0xA3, 0x01, 0x61, 'S', 0x02, 0x00, 0x03, 0x01};
    b.insert(b.end(), std::begin(s), std::end(s));
  }
  b.insert(b.end(), {0x07, 0x80});
  Fixture f;
  ASSERT_EQ(ParseError::None, f.runBytes(b));
  EXPECT_EQ(limits::kMaxSections, f.header.sectionCount);
}

// ---------------------------------------------------------------- malformed

TEST(ListFile, RejectsAWrongVersion) {
  std::vector<uint8_t> b = fromHex(companion::test::kGoldenEmpty);
  b[2] = 0x02;  // version 2
  Fixture f;
  EXPECT_EQ(ParseError::BadVersion, f.runBytes(b));
}

TEST(ListFile, RejectsATruncatedFile) {
  const std::vector<uint8_t> full = fromHex(companion::test::kGoldenTodos);
  for (size_t cut : {size_t{1}, size_t{8}, full.size() / 2, full.size() - 1}) {
    Fixture f;
    const std::vector<uint8_t> partial(full.begin(), full.begin() + cut);
    EXPECT_NE(ParseError::None, f.runBytes(partial)) << "cut at " << cut;
  }
}

TEST(ListFile, RejectsIndefiniteLengthsAndFloats) {
  // Indefinite-length map at the root - explicitly outside the format's subset.
  Fixture indefinite;
  EXPECT_EQ(ParseError::Cbor, indefinite.runBytes({0xBF, 0x01, 0x01, 0xFF}));
  // A float where the version uint belongs.
  Fixture flt;
  EXPECT_EQ(ParseError::Cbor, flt.runBytes({0xA1, 0x01, 0xFA, 0x3F, 0x80, 0x00, 0x00}));
}

TEST(ListFile, RejectsARootThatIsNotAMap) {
  Fixture f;
  EXPECT_EQ(ParseError::Cbor, f.runBytes({0x82, 0x01, 0x02}));
}

TEST(ListFile, RejectsAnItemWithoutAnIdOrPrimary) {
  // One item carrying only `primary`.
  const std::vector<uint8_t> b = {0xA7, 0x01, 0x01, 0x02, 0x62, 'h', 'x', 0x03, 0x61, 'T', 0x04,
                                  0x00, 0x05, 0x01, 0x06, 0x80, 0x07, 0x81, 0xA1, 0x02, 0x61, 'p'};
  Fixture f;
  EXPECT_EQ(ParseError::MissingKey, f.runBytes(b));
}

TEST(ListFile, IgnoresUnknownKeysAtEveryLevel) {
  // Root key 9 (a map), item key 12 (an array): both must be skipped whole.
  const std::vector<uint8_t> b = {0xA8, 0x01, 0x01, 0x02, 0x62, 'h',  'x',  0x03, 0x61, 'T',  0x04, 0x00,
                                  0x05, 0x01, 0x06, 0x80, 0x07, 0x81, 0xA4, 0x01, 0x62, 'i',  '1',  0x02,
                                  0x61, 'p',  0x05, 0x02, 0x0C, 0x82, 0x01, 0x02, 0x09, 0xA1, 0x01, 0x01};
  Fixture f;
  ASSERT_EQ(ParseError::None, f.runBytes(b));
  ASSERT_EQ(1u, f.out.rows.size());
  EXPECT_STREQ("i1", f.out.rows[0].id);
  EXPECT_EQ(2u, f.out.rows[0].actions);
}

TEST(ListFile, ASinkThatStopsAbortsTheParse) {
  Fixture f;
  f.out.stopAfter = 1;
  EXPECT_EQ(ParseError::Aborted, f.run(companion::test::kGoldenInbox));
  EXPECT_EQ(1u, f.out.rows.size());
}

TEST(ListFile, ReadsThroughAnFsPortFile) {
  companion::test::FakeFs fs;
  const std::vector<uint8_t> bytes = fromHex(companion::test::kGoldenTodos);
  fs.mkdirs("/.companion/lists");
  fs.writeAll("/.companion/lists/todos.list", bytes.data(), bytes.size());

  FsByteSource src(fs, "/.companion/lists/todos.list");
  ASSERT_TRUE(src.ok());
  Fixture f;
  ASSERT_EQ(ParseError::None, f.parser.parse(src, f.header, f.scratch, collect, &f.out));
  EXPECT_STREQ("todos", f.header.listId);
  EXPECT_EQ(2u, f.out.rows.size());

  FsByteSource missing(fs, "/.companion/lists/nope.list");
  EXPECT_FALSE(missing.ok());
}

// ------------------------------------------------------------ action masks

TEST(ActionMask, RowMaskWins) {
  // The LISTFILE.md example: complete | snooze | edit.
  const uint32_t mask = resolveActions(0x206, kActionBit(3));
  uint8_t ids[kMaxActionId];
  const uint8_t n = listActions(mask, ids);
  ASSERT_EQ(3, n);
  EXPECT_EQ(1, ids[0]);  // complete
  EXPECT_EQ(2, ids[1]);  // snooze
  EXPECT_EQ(9, ids[2]);  // edit
}

TEST(ActionMask, ZeroFallsBackToTheListDefault) {
  const uint32_t mask = resolveActions(0, kActionBit(3) | kActionBit(5));
  uint8_t ids[kMaxActionId];
  ASSERT_EQ(2, listActions(mask, ids));
  EXPECT_EQ(3, ids[0]);  // open
  EXPECT_EQ(5, ids[1]);  // reply
}

TEST(ActionMask, ReservedBitZeroAndUnknownIdsAreDropped) {
  // Bit 0 is reserved by the format; bits above the catalogue name actions this
  // build cannot label, so an action sheet must not offer them.
  const uint32_t mask = resolveActions(0x1u | kActionBit(1) | (1u << 20), 0);
  uint8_t ids[kMaxActionId];
  ASSERT_EQ(1, listActions(mask, ids));
  EXPECT_EQ(1, ids[0]);
}

TEST(ActionMask, AnEmptyMaskOffersNothing) {
  uint8_t ids[kMaxActionId];
  EXPECT_EQ(0, listActions(resolveActions(0, 0), ids));
}

TEST(ActionMask, EveryCatalogueIdRoundTrips) {
  uint32_t all = 0;
  for (uint8_t id = 1; id <= kMaxActionId; ++id) all |= kActionBit(id);
  uint8_t ids[kMaxActionId];
  ASSERT_EQ(kMaxActionId, listActions(resolveActions(all, 0), ids));
  for (uint8_t i = 0; i < kMaxActionId; ++i) EXPECT_EQ(i + 1, ids[i]);
}

}  // namespace
}  // namespace companion::brain
