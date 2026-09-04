#if CROSSPOINT_COMPANION

#include "ListFile.h"

#include <cstring>

namespace companion::brain {

namespace {

constexpr uint8_t kMajorUint = 0;
constexpr uint8_t kMajorBstr = 2;
constexpr uint8_t kMajorTstr = 3;
constexpr uint8_t kMajorArray = 4;
constexpr uint8_t kMajorMap = 5;
constexpr uint8_t kMajorSimple = 7;
constexpr unsigned kMaxDepth = 8;

// Root keys (LISTFILE.md "Root map").
constexpr uint64_t kKeyVersion = 1;
constexpr uint64_t kKeyListId = 2;
constexpr uint64_t kKeyTitle = 3;
constexpr uint64_t kKeyGeneratedAt = 4;
constexpr uint64_t kKeyItemCount = 5;
constexpr uint64_t kKeySections = 6;
constexpr uint64_t kKeyItems = 7;
constexpr uint64_t kKeyDefaultActions = 8;

// Largest prefix that fits `cap` bytes without splitting a UTF-8 sequence.
size_t utf8Fit(const char* s, size_t len, size_t cap) {
  if (len <= cap) return len;
  size_t n = cap;
  while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
  return n;
}

}  // namespace

// ------------------------------------------------------------ small helpers

uint32_t resolveActions(const uint32_t itemActions, const uint32_t defaultActions) {
  const uint32_t raw = itemActions != 0 ? itemActions : defaultActions;
  // Bit 0 is reserved and ids above the catalogue are not renderable here; a
  // future action must land with a build that knows its label.
  const uint32_t known = ((1u << (kMaxActionId + 1)) - 1u) & ~1u;
  return raw & known;
}

uint8_t listActions(const uint32_t mask, uint8_t* out) {
  uint8_t n = 0;
  for (uint8_t id = 1; id <= kMaxActionId; ++id) {
    if (mask & kActionBit(id)) out[n++] = id;
  }
  return n;
}

const char* errorName(const ParseError e) {
  switch (e) {
    case ParseError::None: return "ok";
    case ParseError::Io: return "io";
    case ParseError::Cbor: return "cbor";
    case ParseError::BadVersion: return "version";
    case ParseError::MissingKey: return "missing key";
    case ParseError::TooManyItems: return "too many items";
    case ParseError::CountMismatch: return "count mismatch";
    case ParseError::Aborted: return "aborted";
  }
  return "?";
}

void ListHeader::clear() {
  // Field-by-field, not `*this = ListHeader{}`: the struct carries the whole
  // section table (>1 KB), and a temporary that size would blow the 256-byte
  // stack-local budget (CLAUDE.md Resource Protocol).
  listId[0] = '\0';
  title[0] = '\0';
  generatedAt = 0;
  itemCount = 0;
  defaultActions = 0;
  sectionCount = 0;
}

int ListHeader::sectionForItem(const uint32_t item) const {
  for (uint16_t i = 0; i < sectionCount; ++i) {
    const ListSection& s = sections[i];
    if (item >= s.start && item < s.start + s.count) return static_cast<int>(i);
  }
  return -1;
}

void ListRow::clear() {
  id[0] = primary[0] = secondary[0] = meta[0] = badge[0] = '\0';
  actions = 0;
  flags = 0;
}

// ------------------------------------------------------------- FsByteSource

FsByteSource::FsByteSource(FsPort& fs, const char* path) : file_(fs.openRead(path)) {}

size_t FsByteSource::read(uint8_t* out, const size_t len) {
  if (!file_) return 0;
  size_t got = 0;
  if (!file_->readAt(offset_, out, len, got)) return 0;
  offset_ += static_cast<uint32_t>(got);
  return got;
}

// ------------------------------------------------------------------ window

bool ListParser::fill(const size_t need) {
  if (need > kWindow) return false;
  if (tail_ - head_ >= need) return true;
  if (head_ > 0) {
    memmove(buf_, buf_ + head_, tail_ - head_);
    tail_ -= head_;
    head_ = 0;
  }
  while (tail_ - head_ < need && !eof_) {
    const size_t got = src_->read(buf_ + tail_, kWindow - tail_);
    if (got == 0) {
      eof_ = true;
      break;
    }
    tail_ += got;
  }
  return tail_ - head_ >= need;
}

// Reads one CBOR head. Rejects indefinite lengths (31), floats and every simple
// value: the format is the deterministic subset (LISTFILE.md "Encoding rules").
bool ListParser::head(uint8_t& major, uint64_t& arg) {
  if (!fill(1)) return false;
  const uint8_t ib = buf_[head_];
  major = static_cast<uint8_t>(ib >> 5);
  const uint8_t ai = ib & 0x1F;
  if (ai < 24) {
    ++head_;
    arg = ai;
    return true;
  }
  if (ai > 27) return false;  // 28-30 reserved, 31 indefinite
  const size_t n = static_cast<size_t>(1u << (ai - 24));
  if (!fill(1 + n)) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) v = (v << 8) | buf_[head_ + 1 + i];
  head_ += 1 + n;
  arg = v;
  return true;
}

bool ListParser::readUint(uint64_t& v) {
  uint8_t major = 0;
  if (!head(major, v)) return false;
  return major == kMajorUint;
}

bool ListParser::enterArray(uint64_t& count) {
  uint8_t major = 0;
  if (!head(major, count)) return false;
  return major == kMajorArray;
}

bool ListParser::enterMap(uint64_t& count) {
  uint8_t major = 0;
  if (!head(major, count)) return false;
  return major == kMajorMap;
}

bool ListParser::skipBytes(uint64_t n) {
  while (n > 0) {
    const size_t have = tail_ - head_;
    if (have > 0) {
      const size_t take = static_cast<size_t>(n < have ? n : have);
      head_ += take;
      n -= take;
      continue;
    }
    if (!fill(1)) return false;
  }
  return true;
}

bool ListParser::readTextInto(char* out, const size_t cap) {
  uint8_t major = 0;
  uint64_t len = 0;
  if (!head(major, len)) return false;
  if (major != kMajorTstr) return false;
  const size_t want = static_cast<size_t>(len < cap ? len : cap);
  // Any string within its declared cap fits the window in one refill; an
  // over-cap (or hostile) string is copied up to `cap` and the tail streamed.
  if (want > 0) {
    if (!fill(want)) return false;
    // Only a string that outran its cap needs the boundary cut; a whole one is
    // already well-formed.
    const size_t keep = want < len ? utf8Fit(reinterpret_cast<const char*>(buf_ + head_), len, want) : want;
    memcpy(out, buf_ + head_, keep);
    out[keep] = '\0';
    head_ += want;
  } else {
    out[0] = '\0';
  }
  return skipBytes(len - want);
}

// Consumes one complete item of any type (used for unknown keys).
bool ListParser::skipItem(const unsigned depth) {
  if (depth > kMaxDepth) return false;
  uint8_t major = 0;
  uint64_t arg = 0;
  if (!head(major, arg)) return false;
  switch (major) {
    case kMajorUint:
    case 1:  // negint
      return true;
    case kMajorBstr:
    case kMajorTstr: return skipBytes(arg);
    case kMajorArray:
      for (uint64_t i = 0; i < arg; ++i) {
        if (!skipItem(depth + 1)) return false;
      }
      return true;
    case kMajorMap:
      for (uint64_t i = 0; i < arg * 2; ++i) {
        if (!skipItem(depth + 1)) return false;
      }
      return true;
    case kMajorSimple:
      // true/false/null only; floats (25-27) already consumed their payload as
      // `arg`, which would desynchronise the stream, so reject them.
      return arg == 20 || arg == 21 || arg == 22;
    default: return false;  // tags are not in the subset
  }
}

// -------------------------------------------------------------------- parse

ParseError ListParser::parse(ByteSource& src, ListHeader& header, ListRow& row, const ItemSink sink, void* user) {
  src_ = &src;
  head_ = tail_ = 0;
  eof_ = false;
  header.clear();

  uint64_t rootPairs = 0;
  if (!enterMap(rootPairs)) return ParseError::Cbor;

  bool sawVersion = false;
  bool sawListId = false;
  bool sawItems = false;
  bool sawCount = false;
  uint32_t emitted = 0;

  for (uint64_t i = 0; i < rootPairs; ++i) {
    uint64_t key = 0;
    if (!readUint(key)) return ParseError::Cbor;
    switch (key) {
      case kKeyVersion: {
        uint64_t v = 0;
        if (!readUint(v)) return ParseError::Cbor;
        if (v != 1) return ParseError::BadVersion;
        sawVersion = true;
        break;
      }
      case kKeyListId:
        if (!readTextInto(header.listId, limits::kId)) return ParseError::Cbor;
        sawListId = true;
        break;
      case kKeyTitle:
        if (!readTextInto(header.title, limits::kTitle)) return ParseError::Cbor;
        break;
      case kKeyGeneratedAt: {
        uint64_t v = 0;
        if (!readUint(v)) return ParseError::Cbor;
        header.generatedAt = static_cast<uint32_t>(v);
        break;
      }
      case kKeyItemCount: {
        uint64_t v = 0;
        if (!readUint(v)) return ParseError::Cbor;
        if (v > limits::kMaxItems) return ParseError::TooManyItems;
        header.itemCount = static_cast<uint32_t>(v);
        sawCount = true;
        break;
      }
      case kKeySections: {
        uint64_t n = 0;
        if (!enterArray(n)) return ParseError::Cbor;
        for (uint64_t s = 0; s < n; ++s) {
          uint64_t pairs = 0;
          if (!enterMap(pairs)) return ParseError::Cbor;
          ListSection section;
          for (uint64_t p = 0; p < pairs; ++p) {
            uint64_t sk = 0;
            if (!readUint(sk)) return ParseError::Cbor;
            if (sk == 1) {
              if (!readTextInto(section.title, limits::kSectionTitle)) return ParseError::Cbor;
            } else if (sk == 2 || sk == 3) {
              uint64_t v = 0;
              if (!readUint(v)) return ParseError::Cbor;
              (sk == 2 ? section.start : section.count) = static_cast<uint32_t>(v);
            } else if (!skipItem(0)) {
              return ParseError::Cbor;
            }
          }
          // Sections past the table cap are dropped, not an error: their rows
          // simply render un-headed.
          if (header.sectionCount < limits::kMaxSections && section.count > 0) {
            header.sections[header.sectionCount++] = section;
          }
        }
        break;
      }
      case kKeyDefaultActions: {
        uint64_t v = 0;
        if (!readUint(v)) return ParseError::Cbor;
        header.defaultActions = static_cast<uint32_t>(v);
        break;
      }
      case kKeyItems: {
        // The format guarantees keys 1-6 arrive first, so itemCount and the
        // section table are already known here; nothing has to be buffered.
        if (!sawVersion) return ParseError::BadVersion;
        if (!sawCount) return ParseError::MissingKey;
        uint64_t n = 0;
        if (!enterArray(n)) return ParseError::Cbor;
        if (n != header.itemCount) return ParseError::CountMismatch;
        for (uint64_t it = 0; it < n; ++it) {
          uint64_t pairs = 0;
          if (!enterMap(pairs)) return ParseError::Cbor;
          row.clear();
          bool sawId = false;
          bool sawPrimary = false;
          for (uint64_t p = 0; p < pairs; ++p) {
            uint64_t ik = 0;
            if (!readUint(ik)) return ParseError::Cbor;
            switch (ik) {
              case 1:
                if (!readTextInto(row.id, limits::kId)) return ParseError::Cbor;
                sawId = true;
                break;
              case 2:
                if (!readTextInto(row.primary, limits::kPrimary)) return ParseError::Cbor;
                sawPrimary = true;
                break;
              case 3:
                if (!readTextInto(row.secondary, limits::kSecondary)) return ParseError::Cbor;
                break;
              case 4:
                if (!readTextInto(row.meta, limits::kMeta)) return ParseError::Cbor;
                break;
              case 5: {
                uint64_t v = 0;
                if (!readUint(v)) return ParseError::Cbor;
                row.actions = static_cast<uint32_t>(v);
                break;
              }
              case 6: {
                uint64_t v = 0;
                if (!readUint(v)) return ParseError::Cbor;
                row.flags = static_cast<uint32_t>(v);
                break;
              }
              case 7:
                if (!readTextInto(row.badge, limits::kBadge)) return ParseError::Cbor;
                break;
              default:
                if (!skipItem(0)) return ParseError::Cbor;
                break;
            }
          }
          if (!sawId || !sawPrimary) return ParseError::MissingKey;
          if (sink && !sink(user, static_cast<uint32_t>(it), row)) return ParseError::Aborted;
          ++emitted;
        }
        sawItems = true;
        break;
      }
      default:
        // "Skip unknown keys (of any type) inside any map; skip unknown root
        // keys > 8" - LISTFILE.md, Reader behaviour.
        if (!skipItem(0)) return ParseError::Cbor;
        break;
    }
  }

  if (!sawVersion) return ParseError::BadVersion;
  if (!sawListId || !sawItems) return ParseError::MissingKey;
  if (emitted != header.itemCount) return ParseError::CountMismatch;
  return ParseError::None;
}

}  // namespace companion::brain

#endif  // CROSSPOINT_COMPANION
