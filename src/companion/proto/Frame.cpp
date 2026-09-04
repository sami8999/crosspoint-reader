#if CROSSPOINT_COMPANION

#include "Frame.h"

#include <cstring>

namespace companion::proto {

namespace {
inline void putLe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline uint16_t getLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
}  // namespace

// ---------------------------------------------------------------- frame header

void encodeFrameHeader(const FrameHeader& h, uint8_t* out) {
  out[0] = h.type;
  putLe16(out + 1, h.seq);
  putLe16(out + 3, h.len);
}

bool decodeFrameHeader(const uint8_t* p, size_t n, FrameHeader& out) {
  if (n < kFrameHeaderSize) return false;
  out.type = p[0];
  out.seq = getLe16(p + 1);
  out.len = getLe16(p + 3);
  return out.len <= kMaxPayloadSize;
}

bool parseFrame(const uint8_t* p, size_t n, FrameView& out) {
  if (!decodeFrameHeader(p, n, out.header)) return false;
  if (n != kFrameHeaderSize + out.header.len) return false;
  out.payload = p + kFrameHeaderSize;
  return true;
}

// ---------------------------------------------------------------- Segmenter

Segmenter::Segmenter(const uint8_t* frame, size_t len, size_t mtu)
    : frame_(frame),
      len_(len),
      body_(mtu > kAttHeaderSize + kSegmentHeaderSize ? mtu - kAttHeaderSize - kSegmentHeaderSize : 0) {}

size_t Segmenter::segmentCount() const {
  if (body_ == 0 || len_ == 0) return 0;
  const size_t count = (len_ + body_ - 1) / body_;
  return count <= kSegmentIndexMask + 1 ? count : 0;
}

bool Segmenter::next(uint8_t* out, size_t cap, size_t& outLen) {
  if (body_ == 0 || (started_ && pos_ >= len_)) return false;
  if (index_ > kSegmentIndexMask) return false;
  size_t take = len_ - pos_;
  if (take > body_) take = body_;
  if (cap < kSegmentHeaderSize + take) return false;
  const bool fin = pos_ + take >= len_;
  out[0] = static_cast<uint8_t>(index_ | (fin ? kSegmentFin : 0));
  if (take) memcpy(out + kSegmentHeaderSize, frame_ + pos_, take);
  pos_ += take;
  ++index_;
  started_ = true;
  outLen = kSegmentHeaderSize + take;
  return true;
}

// ---------------------------------------------------------------- Reassembler

void Reassembler::reset() {
  size_ = 0;
  expect_ = 0;
  active_ = false;
}

Reassembler::Result Reassembler::feed(const uint8_t* segment, size_t n) {
  if (n < kSegmentHeaderSize) {
    reset();
    return Result::Error;
  }
  const uint8_t index = segment[0] & kSegmentIndexMask;
  const bool fin = segment[0] & kSegmentFin;
  if (index == 0) {
    reset();  // a new frame starts; anything in flight is dropped
  } else if (!active_ || index != expect_) {
    reset();
    return Result::Error;
  }
  const size_t body = n - kSegmentHeaderSize;
  if (size_ + body > cap_ || size_ + body > kMaxFrameSize) {
    reset();
    return Result::Error;
  }
  if (body) memcpy(buf_ + size_, segment + kSegmentHeaderSize, body);
  size_ += body;
  active_ = true;
  if (fin) {
    active_ = false;
    expect_ = 0;
    return Result::Complete;
  }
  if (index == kSegmentIndexMask) {  // no room for another index before FIN
    reset();
    return Result::Error;
  }
  expect_ = static_cast<uint8_t>(index + 1);
  return Result::More;
}

// ---------------------------------------------------------------- bulk chunk

void encodeBulkChunkHeader(uint16_t index, uint8_t* out) { putLe16(out, index); }

bool decodeBulkChunk(const uint8_t* p, size_t n, BulkChunk& out) {
  if (n < kBulkChunkHeaderSize) return false;
  out.index = getLe16(p);
  out.data = p + kBulkChunkHeaderSize;
  out.len = n - kBulkChunkHeaderSize;
  return true;
}

}  // namespace companion::proto

#endif  // CROSSPOINT_COMPANION
