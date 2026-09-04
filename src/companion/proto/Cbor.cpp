#if CROSSPOINT_COMPANION

#include "Cbor.h"

#include <cstring>

namespace companion::proto {

namespace {
constexpr unsigned kMaxSkipDepth = 8;

constexpr size_t headSize(uint64_t arg) {
  return arg < 24 ? 1 : arg <= 0xFF ? 2 : arg <= 0xFFFF ? 3 : arg <= 0xFFFFFFFFu ? 5 : 9;
}
}  // namespace

// ---------------------------------------------------------------- CborWriter

bool CborWriter::put(uint8_t b) {
  if (failed_ || pos_ >= cap_) {
    failed_ = true;
    return false;
  }
  buf_[pos_++] = b;
  return true;
}

bool CborWriter::head(uint8_t major, uint64_t arg) {
  const uint8_t mt = static_cast<uint8_t>(major << 5);
  if (arg < 24) return put(mt | static_cast<uint8_t>(arg));
  unsigned bytes;
  uint8_t ai;
  if (arg <= 0xFF) {
    bytes = 1;
    ai = 24;
  } else if (arg <= 0xFFFF) {
    bytes = 2;
    ai = 25;
  } else if (arg <= 0xFFFFFFFFu) {
    bytes = 4;
    ai = 26;
  } else {
    bytes = 8;
    ai = 27;
  }
  if (failed_ || cap_ - pos_ < 1 + bytes) {
    failed_ = true;
    return false;
  }
  buf_[pos_++] = mt | ai;
  for (unsigned i = bytes; i-- > 0;) buf_[pos_++] = static_cast<uint8_t>(arg >> (8 * i));
  return true;
}

bool CborWriter::writeNegInt(int64_t v) {
  if (v >= 0) {
    failed_ = true;
    return false;
  }
  // CBOR encodes -1 - n; -(v + 1) never overflows for v < 0.
  return head(1, static_cast<uint64_t>(-(v + 1)));
}

bool CborWriter::writeRaw(const uint8_t* p, size_t n) {
  if (failed_ || cap_ - pos_ < n) {
    failed_ = true;
    return false;
  }
  if (n) memcpy(buf_ + pos_, p, n);
  pos_ += n;
  return true;
}

// Strings are checked for space up front so a failed write leaves no partial item.
bool CborWriter::writeBstr(const uint8_t* p, size_t n) {
  if (failed_ || cap_ - pos_ < headSize(n) + n) {
    failed_ = true;
    return false;
  }
  return head(2, n) && writeRaw(p, n);
}

bool CborWriter::writeTstr(std::string_view s) {
  if (failed_ || cap_ - pos_ < headSize(s.size()) + s.size()) {
    failed_ = true;
    return false;
  }
  return head(3, s.size()) && writeRaw(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ---------------------------------------------------------------- CborReader

// Parses the initial byte plus argument. Rejects additional-info 28..31 (reserved
// and indefinite). Does not advance the cursor; `next` points past the head.
bool CborReader::readHead(uint8_t& major, uint64_t& arg, const uint8_t*& next) const {
  if (cur_ >= end_) return false;
  const uint8_t ib = *cur_;
  major = ib >> 5;
  const uint8_t ai = ib & 0x1F;
  const uint8_t* p = cur_ + 1;
  if (ai < 24) {
    arg = ai;
  } else if (ai <= 27) {
    const unsigned bytes = 1u << (ai - 24);
    if (static_cast<size_t>(end_ - p) < bytes) return false;
    arg = 0;
    for (unsigned i = 0; i < bytes; ++i) arg = (arg << 8) | p[i];
    p += bytes;
  } else {
    return false;
  }
  next = p;
  return true;
}

bool CborReader::peekMajor(CborMajor& out) const {
  if (cur_ >= end_) return false;
  out = static_cast<CborMajor>(*cur_ >> 5);
  return true;
}

bool CborReader::readUint(uint64_t& v) {
  uint8_t major;
  uint64_t arg;
  const uint8_t* next;
  if (!readHead(major, arg, next) || major != 0) return false;
  v = arg;
  cur_ = next;
  return true;
}

bool CborReader::readUint32(uint32_t& v) {
  uint64_t wide;
  const uint8_t* save = cur_;
  if (!readUint(wide)) return false;
  if (wide > 0xFFFFFFFFu) {
    cur_ = save;
    return false;
  }
  v = static_cast<uint32_t>(wide);
  return true;
}

bool CborReader::readInt(int64_t& v) {
  uint8_t major;
  uint64_t arg;
  const uint8_t* next;
  if (!readHead(major, arg, next) || major > 1) return false;
  if (arg > static_cast<uint64_t>(INT64_MAX)) return false;
  v = major == 0 ? static_cast<int64_t>(arg) : -1 - static_cast<int64_t>(arg);
  cur_ = next;
  return true;
}

bool CborReader::readInt32(int32_t& v) {
  int64_t wide;
  const uint8_t* save = cur_;
  if (!readInt(wide)) return false;
  if (wide < INT32_MIN || wide > INT32_MAX) {
    cur_ = save;
    return false;
  }
  v = static_cast<int32_t>(wide);
  return true;
}

bool CborReader::readBool(bool& v) {
  if (cur_ >= end_ || (*cur_ != 0xF4 && *cur_ != 0xF5)) return false;
  v = *cur_++ == 0xF5;
  return true;
}

bool CborReader::readNull() {
  if (cur_ >= end_ || *cur_ != 0xF6) return false;
  ++cur_;
  return true;
}

bool CborReader::readBstr(CborBytes& out) {
  uint8_t major;
  uint64_t arg;
  const uint8_t* next;
  if (!readHead(major, arg, next) || major != 2) return false;
  if (arg > static_cast<uint64_t>(end_ - next)) return false;
  out.data = next;
  out.len = static_cast<size_t>(arg);
  cur_ = next + arg;
  return true;
}

bool CborReader::readTstr(std::string_view& out) {
  uint8_t major;
  uint64_t arg;
  const uint8_t* next;
  if (!readHead(major, arg, next) || major != 3) return false;
  if (arg > static_cast<uint64_t>(end_ - next)) return false;
  out = std::string_view(reinterpret_cast<const char*>(next), static_cast<size_t>(arg));
  cur_ = next + arg;
  return true;
}

bool CborReader::readUint32(std::optional<uint32_t>& v) {
  uint32_t tmp;
  if (!readUint32(tmp)) return false;
  v = tmp;
  return true;
}

bool CborReader::readTstr(std::optional<std::string_view>& v) {
  std::string_view tmp;
  if (!readTstr(tmp)) return false;
  v = tmp;
  return true;
}

bool CborReader::enterArray(size_t& count) {
  uint8_t major;
  uint64_t arg;
  const uint8_t* next;
  if (!readHead(major, arg, next) || major != 4) return false;
  // Every element needs at least one byte.
  if (arg > static_cast<uint64_t>(end_ - next)) return false;
  count = static_cast<size_t>(arg);
  cur_ = next;
  return true;
}

bool CborReader::enterMap(size_t& count) {
  uint8_t major;
  uint64_t arg;
  const uint8_t* next;
  if (!readHead(major, arg, next) || major != 5) return false;
  // Every pair needs at least two bytes.
  if (arg > static_cast<uint64_t>(end_ - next) / 2) return false;
  count = static_cast<size_t>(arg);
  cur_ = next;
  return true;
}

bool CborReader::readRaw(CborBytes& out) {
  const uint8_t* start = cur_;
  if (!skip()) return false;
  out.data = start;
  out.len = static_cast<size_t>(cur_ - start);
  return true;
}

bool CborReader::skip() {
  const uint8_t* save = cur_;
  if (skipDepth(0)) return true;
  cur_ = save;
  return false;
}

// Skips one well-formed item. Unlike the typed reads this tolerates floats and
// tags (well-formed, known length) so unknown keys carrying them are ignored.
bool CborReader::skipDepth(unsigned depth) {
  if (depth > kMaxSkipDepth) return false;
  uint8_t major;
  uint64_t arg;
  const uint8_t* next;
  if (!readHead(major, arg, next)) return false;
  switch (major) {
    case 0:
    case 1:
      cur_ = next;
      return true;
    case 2:
    case 3:
      if (arg > static_cast<uint64_t>(end_ - next)) return false;
      cur_ = next + arg;
      return true;
    case 4:
    case 5: {
      const uint64_t items = major == 5 ? arg * 2 : arg;
      if (arg > static_cast<uint64_t>(end_ - next)) return false;
      cur_ = next;
      for (uint64_t i = 0; i < items; ++i) {
        if (!skipDepth(depth + 1)) return false;
      }
      return true;
    }
    case 6:
      cur_ = next;
      return skipDepth(depth + 1);
    default: {  // 7: simple values and floats; the argument bytes are the value.
      const uint8_t ai = *cur_ & 0x1F;
      if (ai >= 28) return false;
      cur_ = next;
      return true;
    }
  }
}

}  // namespace companion::proto

#endif  // CROSSPOINT_COMPANION
