#pragma once
#if CROSSPOINT_COMPANION

// The decoded list, held for as long as its screen is open.
//
// ListParser streams; something still has to remember the rows so the list can
// be scrolled and tapped. That is this: two PSRAM blocks taken once when the
// file is loaded and released when the screen closes - a row table and a string
// arena the rows point into. No per-row allocation, nothing on the internal
// heap, and no allocation at all once the file is read (an e-ink list repaints
// on every cursor move, so a std::string per row would churn the allocator for
// the whole session).
//
// The arena is sized from the file: every string in it came out of that file, so
// the file's own byte count is a hard upper bound, plus room for the
// strike-through copies a completed row needs.

#include <cstddef>
#include <cstdint>

#include "../port/Ports.h"
#include "ListFile.h"

namespace companion::brain {

class ListStore {
 public:
  struct Row {
    // Offsets into the arena; 0 means "absent" (offset 0 is a reserved NUL).
    uint32_t id = 0;
    uint32_t primary = 0;
    uint32_t display = 0;  // primary with its state decoration, drawn instead
    uint32_t secondary = 0;
    uint32_t meta = 0;
    uint32_t badge = 0;
    // badge and meta joined for the row's right-hand slot; the list component
    // has one value field, and building it here keeps it in the arena instead
    // of a per-row std::string on the internal heap.
    uint32_t value = 0;
    uint32_t actions = 0;
    uint32_t flags = 0;
  };

  explicit ListStore(SysPort& sys) : sys_(sys) {}
  ~ListStore() { release(); }
  ListStore(const ListStore&) = delete;
  ListStore& operator=(const ListStore&) = delete;

  // Reads `path` and fills the store. `fileBytes` is the file's size from the
  // directory listing; 0 falls back to the format's 64 KB ceiling.
  ParseError load(FsPort& fs, const char* path, uint32_t fileBytes);
  void release();

  const ListHeader& header() const { return header_; }
  uint32_t count() const { return count_; }
  const Row& row(uint32_t i) const { return rows_[i]; }
  const char* str(uint32_t offset) const { return offset ? arena_ + offset : nullptr; }

  // The row's effective action mask (its own, or the list default).
  uint32_t actionsFor(uint32_t i) const;
  // Applies the local mark a Tap earns until the phone pushes a new file:
  // complete/archive/delete strike the row through, the rest only clear the
  // unread bit. Returns true when the row changed and the screen should repaint.
  bool applyOptimistic(uint32_t i, uint8_t actionId);

 private:
  // Copies `text` into the arena and returns its offset (0 when empty or full).
  uint32_t intern(const char* text);
  // Writes `text` with U+0336 COMBINING LONG STROKE OVERLAY after every
  // codepoint - a real strike-through, using a mark the built-in UI fonts carry
  // (fontconvert.py exports U+0300-U+036F).
  uint32_t internStruck(const char* text);
  void refreshDisplay(uint32_t i);
  static bool sink(void* user, uint32_t index, const ListRow& row);

  SysPort& sys_;
  ListHeader header_;
  ListParser parser_;
  ListRow scratch_;
  Row* rows_ = nullptr;
  char* arena_ = nullptr;
  size_t arenaCap_ = 0;
  size_t arenaUsed_ = 1;  // offset 0 is the reserved "absent" NUL
  uint32_t count_ = 0;
  uint32_t capacity_ = 0;
  bool overflow_ = false;
};

}  // namespace companion::brain

#endif  // CROSSPOINT_COMPANION
