#pragma once
#if CROSSPOINT_COMPANION

// Minimal CBOR (RFC 8949) codec for the companion protocol.
// Deterministic subset only: shortest-form integers, definite lengths, no floats,
// no tags, no indefinite items. Zero dynamic allocation; the caller owns every buffer.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace companion::proto {

enum class CborMajor : uint8_t { Uint = 0, NegInt = 1, Bstr = 2, Tstr = 3, Array = 4, Map = 5, Tag = 6, Simple = 7 };

// View of raw bytes inside a caller-owned buffer (bstr contents or one encoded CBOR item).
struct CborBytes {
  const uint8_t* data = nullptr;
  size_t len = 0;
};

// Appends CBOR items to a fixed buffer. Once a write does not fit, the writer is
// sticky-failed: every further call returns false and size() stops growing.
class CborWriter {
 public:
  CborWriter(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

  bool writeUint(uint64_t v) { return head(0, v); }
  bool writeNegInt(int64_t v);  // v must be < 0
  bool writeInt(int64_t v) { return v < 0 ? writeNegInt(v) : writeUint(static_cast<uint64_t>(v)); }
  bool writeBstr(const uint8_t* p, size_t n);
  bool writeBstr(CborBytes b) { return writeBstr(b.data, b.len); }
  bool writeTstr(std::string_view s);
  bool writeArrayHeader(size_t count) { return head(4, count); }
  bool writeMapHeader(size_t count) { return head(5, count); }
  bool writeBool(bool b) { return put(b ? 0xF5 : 0xF4); }
  bool writeNull() { return put(0xF6); }
  // Copies pre-encoded CBOR verbatim (used to forward an already-encoded map).
  bool writeRaw(const uint8_t* p, size_t n);
  bool writeRaw(CborBytes b) { return writeRaw(b.data, b.len); }

  // key/value pairs for uint-keyed maps.
  bool keyUint(uint64_t key, uint64_t v) { return writeUint(key) && writeUint(v); }
  bool keyInt(uint64_t key, int64_t v) { return writeUint(key) && writeInt(v); }
  bool keyBool(uint64_t key, bool v) { return writeUint(key) && writeBool(v); }
  bool keyTstr(uint64_t key, std::string_view v) { return writeUint(key) && writeTstr(v); }
  bool keyBstr(uint64_t key, CborBytes v) { return writeUint(key) && writeBstr(v); }

  const uint8_t* data() const { return buf_; }
  size_t size() const { return pos_; }
  size_t capacity() const { return cap_; }
  bool ok() const { return !failed_; }

 private:
  bool head(uint8_t major, uint64_t arg);
  bool put(uint8_t b);

  uint8_t* buf_;
  size_t cap_;
  size_t pos_ = 0;
  bool failed_ = false;
};

// Pull decoder over a caller-owned buffer. Every read returns false and leaves the
// cursor unchanged on malformed input, indefinite lengths, floats, tags, or a type
// mismatch. Strings and byte strings are returned as views into the input.
class CborReader {
 public:
  CborReader(const uint8_t* p, size_t n) : cur_(p), end_(p + n) {}
  explicit CborReader(CborBytes b) : CborReader(b.data, b.len) {}

  bool atEnd() const { return cur_ == end_; }
  size_t remaining() const { return static_cast<size_t>(end_ - cur_); }
  const uint8_t* cursor() const { return cur_; }

  bool peekMajor(CborMajor& out) const;

  bool readUint(uint64_t& v);
  bool readUint32(uint32_t& v);
  bool readInt(int64_t& v);
  bool readInt32(int32_t& v);
  bool readBool(bool& v);
  bool readNull();
  bool readBstr(CborBytes& out);
  bool readTstr(std::string_view& out);
  // Optional-field conveniences: same as above, assigning into the optional.
  bool readUint32(std::optional<uint32_t>& v);
  bool readTstr(std::optional<std::string_view>& v);

  // Consume an array/map header; `count` is the number of elements / key-value pairs.
  bool enterArray(size_t& count);
  bool enterMap(size_t& count);

  // Consume one complete item (recursing into containers, bounded depth) and
  // return its raw encoded bytes.
  bool readRaw(CborBytes& out);
  // Consume one complete item and discard it (unknown map keys).
  bool skip();

 private:
  bool readHead(uint8_t& major, uint64_t& arg, const uint8_t*& next) const;
  bool skipDepth(unsigned depth);

  const uint8_t* cur_;
  const uint8_t* end_;
};

}  // namespace companion::proto

#endif  // CROSSPOINT_COMPANION
