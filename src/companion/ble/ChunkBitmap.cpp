#if CROSSPOINT_COMPANION

#include "ChunkBitmap.h"

#include <cstring>

namespace companion {

void ChunkBitmap::attach(uint8_t* bits, uint32_t count) {
  bits_ = bits;
  count_ = count;
  received_ = 0;
  if (bits_ && count_) memset(bits_, 0, bytesFor(count_));
}

void ChunkBitmap::detach() {
  bits_ = nullptr;
  count_ = 0;
  received_ = 0;
}

bool ChunkBitmap::test(uint32_t index) const {
  if (index >= count_ || !bits_) return false;
  return (bits_[index >> 3] >> (index & 7)) & 1u;
}

bool ChunkBitmap::set(uint32_t index) {
  if (index >= count_ || !bits_) return false;
  const uint8_t mask = static_cast<uint8_t>(1u << (index & 7));
  if (bits_[index >> 3] & mask) return false;
  bits_[index >> 3] |= mask;
  ++received_;
  return true;
}

size_t ChunkBitmap::missing(uint16_t* out, size_t cap) const {
  size_t n = 0;
  if (!bits_) return 0;
  for (uint32_t i = 0; i < count_ && n < cap; ++i) {
    if (!test(i)) out[n++] = static_cast<uint16_t>(i);
  }
  return n;
}

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
