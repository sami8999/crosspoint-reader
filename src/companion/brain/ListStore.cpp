#if CROSSPOINT_COMPANION

#include "ListStore.h"

#include <cstdio>
#include <cstring>

#include "../Log.h"

namespace companion::brain {

namespace {

// COMBINING LONG STROKE OVERLAY, U+0336. fontconvert.py exports U+0300-U+036F
// into every built-in font, so this draws as a real strike-through rather than
// a missing-glyph box.
constexpr char kStrike[] = "\xCC\xB6";
constexpr size_t kStrikeLen = sizeof(kStrike) - 1;

// Row-state markers. Both are in General Punctuation (U+2000-U+206F), which the
// built-in UI fonts also carry.
constexpr const char* kPinMark = "\xE2\x80\xA0 ";      // U+2020 DAGGER: pinned
constexpr const char* kUnreadMark = "\xE2\x80\xA2 ";   // U+2022 BULLET: unread

bool isContinuation(const char c) { return (static_cast<uint8_t>(c) & 0xC0) == 0x80; }

}  // namespace

void ListStore::release() {
  if (rows_) {
    sys_.freeBig(rows_);
    rows_ = nullptr;
  }
  if (arena_) {
    sys_.freeBig(arena_);
    arena_ = nullptr;
  }
  arenaCap_ = 0;
  arenaUsed_ = 1;
  count_ = 0;
  capacity_ = 0;
  overflow_ = false;
  header_.clear();
}

uint32_t ListStore::intern(const char* text) {
  if (!text || !*text) return 0;
  const size_t n = strlen(text) + 1;
  if (arenaUsed_ + n > arenaCap_) {
    overflow_ = true;
    return 0;
  }
  const uint32_t off = static_cast<uint32_t>(arenaUsed_);
  memcpy(arena_ + off, text, n);
  arenaUsed_ += n;
  return off;
}

uint32_t ListStore::internStruck(const char* text) {
  if (!text || !*text) return 0;
  const size_t len = strlen(text);
  // Worst case one combining mark per byte; the real cost is one per codepoint.
  if (arenaUsed_ + len * (1 + kStrikeLen) + 1 > arenaCap_) {
    overflow_ = true;
    return 0;
  }
  const uint32_t off = static_cast<uint32_t>(arenaUsed_);
  char* out = arena_ + off;
  size_t w = 0;
  for (size_t i = 0; i < len; ++i) {
    out[w++] = text[i];
    // The mark goes after the whole codepoint, never between its bytes.
    if (i + 1 < len && isContinuation(text[i + 1])) continue;
    if (text[i] == ' ') continue;  // struck spaces read as a solid bar
    memcpy(out + w, kStrike, kStrikeLen);
    w += kStrikeLen;
  }
  out[w] = '\0';
  arenaUsed_ += w + 1;
  return off;
}

void ListStore::refreshDisplay(const uint32_t i) {
  Row& r = rows_[i];
  const char* plain = str(r.primary);
  if (!plain) {
    r.display = 0;
    return;
  }
  // done wins: a struck row does not also need its unread dot.
  if (r.flags & flags::kDone) {
    const uint32_t struck = internStruck(plain);
    r.display = struck ? struck : r.primary;
    return;
  }
  if (!(r.flags & (flags::kPinned | flags::kUnread))) {
    r.display = r.primary;
    return;
  }
  // Prefix markers, built into the arena so no render allocates.
  char buf[limits::kPrimary + 12];
  buf[0] = '\0';
  if (r.flags & flags::kPinned) strncat(buf, kPinMark, sizeof(buf) - strlen(buf) - 1);
  if (r.flags & flags::kUnread) strncat(buf, kUnreadMark, sizeof(buf) - strlen(buf) - 1);
  strncat(buf, plain, sizeof(buf) - strlen(buf) - 1);
  const uint32_t marked = intern(buf);
  r.display = marked ? marked : r.primary;
}

bool ListStore::sink(void* user, const uint32_t index, const ListRow& row) {
  auto* self = static_cast<ListStore*>(user);
  if (index >= self->capacity_) return false;
  Row& r = self->rows_[index];
  r = Row{};
  r.id = self->intern(row.id);
  r.primary = self->intern(row.primary);
  r.secondary = self->intern(row.secondary);
  r.meta = self->intern(row.meta);
  r.badge = self->intern(row.badge);
  if (row.badge[0] && row.meta[0]) {
    char joined[limits::kBadge + limits::kMeta + 3];
    snprintf(joined, sizeof(joined), "%s  %s", row.badge, row.meta);
    r.value = self->intern(joined);
  } else if (row.badge[0]) {
    r.value = r.badge;
  } else {
    r.value = r.meta;
  }
  r.actions = row.actions;
  r.flags = row.flags;
  self->count_ = index + 1;
  self->refreshDisplay(index);
  // An arena that ran dry costs decoration and detail lines, never the rows
  // themselves: a truncated list would be worse than a plain one.
  return true;
}

ParseError ListStore::load(FsPort& fs, const char* path, const uint32_t fileBytes) {
  release();

  // A first pass that stops at the first row: itemCount and the section table
  // are written before the items precisely so the reader can size itself
  // without buffering them. An empty list has no first row, so it parses
  // through to the end instead - both outcomes are a good header.
  {
    FsByteSource probe(fs, path);
    if (!probe.ok()) return ParseError::Io;
    const auto stopAtFirstRow = [](void*, uint32_t, const ListRow&) { return false; };
    const ParseError e = parser_.parse(probe, header_, scratch_, stopAtFirstRow, nullptr);
    if (e != ParseError::None && e != ParseError::Aborted) return e;
  }
  capacity_ = header_.itemCount;
  if (capacity_ == 0) return ParseError::None;  // an empty list needs no storage

  const size_t bytes = fileBytes ? fileBytes : limits::kMaxFileBytes;
  // Every string came out of the file, so its length bounds them all; the
  // decorations (a 2-byte mark per codepoint, or a 4-byte prefix) are what the
  // rest is for.
  arenaCap_ = bytes + capacity_ * (limits::kPrimary * 2 + 16) + 64;
  rows_ = static_cast<Row*>(sys_.allocBig(capacity_ * sizeof(Row)));
  arena_ = static_cast<char*>(sys_.allocBig(arenaCap_));
  if (!rows_ || !arena_) {
    CLOG_ERR("list: OOM (%lu rows, %lu arena)", static_cast<unsigned long>(capacity_ * sizeof(Row)),
             static_cast<unsigned long>(arenaCap_));
    release();
    return ParseError::Io;
  }
  arena_[0] = '\0';
  arenaUsed_ = 1;

  FsByteSource src(fs, path);
  if (!src.ok()) {
    release();
    return ParseError::Io;
  }
  const ParseError e = parser_.parse(src, header_, scratch_, &ListStore::sink, this);
  if (e != ParseError::None) {
    release();
    return e;
  }
  if (overflow_) CLOG_ERR("list: arena full; some rows lost their detail text");
  return ParseError::None;
}

uint32_t ListStore::actionsFor(const uint32_t i) const {
  return resolveActions(rows_[i].actions, header_.defaultActions);
}

bool ListStore::applyOptimistic(const uint32_t i, const uint8_t actionId) {
  if (i >= count_) return false;
  Row& r = rows_[i];
  const uint32_t before = r.flags;
  switch (actionId) {
    case 1:  // complete
    case 4:  // archive
    case 8:  // delete
      r.flags |= flags::kDone;
      r.flags &= ~flags::kUnread;
      break;
    default:
      // open / reply / snooze / accept / decline / edit: the row is no longer
      // new, but nothing about it is finished until the phone says so.
      r.flags &= ~flags::kUnread;
      break;
  }
  if (r.flags == before) return false;
  refreshDisplay(i);
  return true;
}

}  // namespace companion::brain

#endif  // CROSSPOINT_COMPANION
