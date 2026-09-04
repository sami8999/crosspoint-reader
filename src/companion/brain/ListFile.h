#pragma once
#if CROSSPOINT_COMPANION

// Reader for the phone's list files (ios/Packages/Artifacts/LISTFILE.md), the
// CBOR documents at /.companion/lists/<listId>.list that BrainListActivity
// renders.
//
// The format was designed for a zero-allocation streaming reader: definite
// lengths only, uint keys in ascending order, and every header field - including
// `itemCount` and the whole section table - written *before* the item array. So
// this parser makes exactly one forward pass and never allocates: it pulls bytes
// through a 256-byte window (larger than any single string the format allows),
// fills a caller-owned ListHeader, then hands each item to a sink one at a time
// through a caller-owned ListRow scratch. Over-long or unknown values are
// skipped through the window rather than buffered, so a hostile file cannot make
// it allocate or overrun.
//
// Nothing here touches Arduino or the SD card; SdListSource in ListFile.cpp
// adapts an FsPort file, and the host tests feed a memory buffer.

#include <cstddef>
#include <cstdint>

#include "../port/Ports.h"

namespace companion::brain {

// Where the parser pulls bytes from. FsByteSource below reads an FsPort file in
// windows; the host tests hand it a memory buffer.
class ByteSource {
 public:
  virtual ~ByteSource() = default;
  // Reads up to `len` bytes forward from the current position. Returns the count
  // (0 = end of input). A short read is only end-of-input, never a retry hint.
  virtual size_t read(uint8_t* out, size_t len) = 0;
};

// Reads a file through FsPort::openRead() in kWindow-sized pulls. Holds the open
// handle for its lifetime.
class FsByteSource final : public ByteSource {
 public:
  FsByteSource(FsPort& fs, const char* path);
  bool ok() const { return file_ != nullptr; }
  size_t read(uint8_t* out, size_t len) override;

 private:
  std::unique_ptr<FsFile> file_;
  uint32_t offset_ = 0;
};

// LISTFILE.md "Encoding rules": byte caps on every display string.
namespace limits {
constexpr size_t kId = 32;
constexpr size_t kTitle = 64;
constexpr size_t kPrimary = 120;
constexpr size_t kSecondary = 200;
constexpr size_t kMeta = 40;
constexpr size_t kBadge = 8;
constexpr size_t kSectionTitle = 48;
constexpr uint32_t kMaxItems = 500;
// Sections are contiguous runs over `items`; a list with more headings than
// this is malformed for our purposes and the extras are skipped, not stored.
constexpr size_t kMaxSections = 24;
// Recommended file ceiling from LISTFILE.md; used to size the row arena.
constexpr size_t kMaxFileBytes = 64 * 1024;
}  // namespace limits

// Item `flags` (key 6).
namespace flags {
constexpr uint32_t kDone = 0x01;
constexpr uint32_t kUnread = 0x02;
constexpr uint32_t kPinned = 0x04;
}  // namespace flags

// Item `actions` (key 5): bit n set <=> Tap action id n (PROTOCOL.md §3.3) is
// allowed. Bit 0 is reserved; an all-zero mask means "use defaultActions".
constexpr uint32_t kActionBit(uint8_t actionId) { return 1u << actionId; }
constexpr uint8_t kMaxActionId = 9;

// Resolves a row's effective action mask: its own, or the list default when the
// row's is 0. Reserved bit 0 and bits above the last known action are dropped so
// a future action id cannot open an action sheet entry this build cannot name.
uint32_t resolveActions(uint32_t itemActions, uint32_t defaultActions);
// Fills `out` (capacity kMaxActionId) with the allowed action ids, ascending.
// Returns how many were written.
uint8_t listActions(uint32_t mask, uint8_t* out);

enum class ParseError : uint8_t {
  None = 0,
  Io,             // the source stopped early
  Cbor,           // malformed / non-deterministic encoding, or an unsupported type
  BadVersion,     // key 1 is not 1
  MissingKey,     // a required root or item key is absent
  TooManyItems,   // itemCount > kMaxItems
  CountMismatch,  // len(items) != itemCount
  Aborted,        // the item sink asked to stop
};

const char* errorName(ParseError e);

struct ListSection {
  char title[limits::kSectionTitle + 1] = {};
  uint32_t start = 0;
  uint32_t count = 0;
};

// Everything the reader knows before it has seen a single row (that is the
// point of the format's key order).
struct ListHeader {
  char listId[limits::kId + 1] = {};
  char title[limits::kTitle + 1] = {};
  uint32_t generatedAt = 0;
  uint32_t itemCount = 0;
  uint32_t defaultActions = 0;
  uint16_t sectionCount = 0;
  ListSection sections[limits::kMaxSections];

  // Resets the scalars; the section table is written up to sectionCount only,
  // so its stale tail is never read.
  void clear();
  // Index of the section covering `item`, or -1 when the row is un-headed.
  int sectionForItem(uint32_t item) const;
};

// One decoded row. ~410 bytes: never a stack local (CLAUDE.md Resource
// Protocol); the caller keeps one in its own long-lived storage.
struct ListRow {
  char id[limits::kId + 1] = {};
  char primary[limits::kPrimary + 1] = {};
  char secondary[limits::kSecondary + 1] = {};
  char meta[limits::kMeta + 1] = {};
  char badge[limits::kBadge + 1] = {};
  uint32_t actions = 0;
  uint32_t flags = 0;
  void clear();
};

// Return false to stop the parse (ParseError::Aborted).
using ItemSink = bool (*)(void* user, uint32_t index, const ListRow& row);

// The parse itself. ~300 bytes of window plus bookkeeping, so like ListRow it
// belongs in the caller's storage, not on the stack.
class ListParser {
 public:
  // One forward pass. `row` is the caller's scratch, reused for every item.
  ParseError parse(ByteSource& src, ListHeader& header, ListRow& row, ItemSink sink, void* user);

 private:
  // Window big enough for any head plus the longest string the format allows
  // (secondary, 200 bytes) without a second refill.
  static constexpr size_t kWindow = 256;

  bool fill(size_t need);
  bool head(uint8_t& major, uint64_t& arg);
  bool readUint(uint64_t& v);
  bool enterArray(uint64_t& count);
  bool enterMap(uint64_t& count);
  // Copies at most `cap` bytes of a tstr into `out` (NUL-terminated, cut on a
  // UTF-8 boundary) and skips the rest.
  bool readTextInto(char* out, size_t cap);
  bool skipBytes(uint64_t n);
  bool skipItem(unsigned depth);

  ByteSource* src_ = nullptr;
  uint8_t buf_[kWindow] = {};
  size_t head_ = 0;
  size_t tail_ = 0;
  bool eof_ = false;
};

}  // namespace companion::brain

#endif  // CROSSPOINT_COMPANION
