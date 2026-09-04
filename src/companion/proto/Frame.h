#pragma once
#if CROSSPOINT_COMPANION

// Companion protocol v1 transport framing (PROTOCOL.md §1.2–§1.4):
// frame header, ctrl segmentation/reassembly and bulk chunk header.
// All multi-byte integers are little-endian. No dynamic allocation.

#include <cstddef>
#include <cstdint>

namespace companion::proto {

constexpr size_t kFrameHeaderSize = 5;
constexpr size_t kMaxFrameSize = 4096;
constexpr size_t kMaxPayloadSize = kMaxFrameSize - kFrameHeaderSize;
constexpr size_t kAttHeaderSize = 3;  // ATT opcode + handle; segment = MTU - 3 bytes
constexpr size_t kSegmentHeaderSize = 1;
constexpr uint8_t kSegmentFin = 0x80;
constexpr uint8_t kSegmentIndexMask = 0x7F;
constexpr size_t kBulkChunkHeaderSize = 2;

// frame = type:u8 | seq:u16 | len:u16 | payload[len]
struct FrameHeader {
  uint8_t type = 0;
  uint16_t seq = 0;
  uint16_t len = 0;
};

void encodeFrameHeader(const FrameHeader& h, uint8_t* out /* [kFrameHeaderSize] */);
// `n` must be >= kFrameHeaderSize.
bool decodeFrameHeader(const uint8_t* p, size_t n, FrameHeader& out);

// Header plus a view of the payload inside the caller's buffer.
struct FrameView {
  FrameHeader header;
  const uint8_t* payload = nullptr;
};

// Accepts exactly one complete frame: n == kFrameHeaderSize + header.len <= kMaxFrameSize.
bool parseFrame(const uint8_t* p, size_t n, FrameView& out);

// Splits one frame into ctrl segments for a given ATT MTU (§1.2). Each call to
// next() writes one segment (hdr + body) into `out`. Returns false when the frame is
// exhausted, `out` is too small, or the frame needs more than 128 segments.
class Segmenter {
 public:
  Segmenter(const uint8_t* frame, size_t len, size_t mtu);

  bool next(uint8_t* out, size_t cap, size_t& outLen);
  bool done() const { return pos_ >= len_ && started_; }
  size_t segmentBodySize() const { return body_; }
  // Number of segments the frame will produce (0 if it cannot be segmented).
  size_t segmentCount() const;

 private:
  const uint8_t* frame_;
  size_t len_;
  size_t body_;
  size_t pos_ = 0;
  uint8_t index_ = 0;
  bool started_ = false;
};

// Rebuilds frames from ctrl segments into a caller-supplied buffer (§1.2).
// Index 0 always starts a new frame (dropping any frame in progress); a gap or
// an overflow beyond the buffer drops the frame in progress and returns Error.
class Reassembler {
 public:
  enum class Result : uint8_t {
    More,      // segment accepted, frame not complete
    Complete,  // FIN seen; data()/size() hold the frame until the next feed()
    Error,     // segment rejected; in-progress frame dropped
  };

  Reassembler(uint8_t* buffer, size_t cap) : buf_(buffer), cap_(cap) {}

  Result feed(const uint8_t* segment, size_t n);
  void reset();

  const uint8_t* data() const { return buf_; }
  size_t size() const { return size_; }
  bool inProgress() const { return active_; }

 private:
  uint8_t* buf_;
  size_t cap_;
  size_t size_ = 0;
  uint8_t expect_ = 0;
  bool active_ = false;
};

// bulk write = chunkIndex:u16 | data
struct BulkChunk {
  uint16_t index = 0;
  const uint8_t* data = nullptr;
  size_t len = 0;
};

void encodeBulkChunkHeader(uint16_t index, uint8_t* out /* [kBulkChunkHeaderSize] */);
// `n` must be >= kBulkChunkHeaderSize; the data view points into `p`.
bool decodeBulkChunk(const uint8_t* p, size_t n, BulkChunk& out);

}  // namespace companion::proto

#endif  // CROSSPOINT_COMPANION
