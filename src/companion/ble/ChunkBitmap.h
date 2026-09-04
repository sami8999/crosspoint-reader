#pragma once
#if CROSSPOINT_COMPANION

// Receipt bitmap for one bulk transfer (PROTOCOL.md §1.4). The bit storage is
// caller-owned so it can live in PSRAM; up to 65536 chunks = 8 KiB of bits.

#include <cstddef>
#include <cstdint>

namespace companion {

class ChunkBitmap {
 public:
  static constexpr size_t bytesFor(uint32_t count) { return (count + 7) / 8; }

  // `bits` must hold bytesFor(count) bytes; it is cleared here.
  void attach(uint8_t* bits, uint32_t count);
  void detach();

  uint32_t count() const { return count_; }
  uint32_t received() const { return received_; }
  uint32_t missingCount() const { return count_ - received_; }
  bool complete() const { return received_ == count_; }
  bool test(uint32_t index) const;
  // Marks a chunk received; returns true only the first time.
  bool set(uint32_t index);
  // Writes up to `cap` missing indices in ascending order; returns how many were written.
  size_t missing(uint16_t* out, size_t cap) const;

 private:
  uint8_t* bits_ = nullptr;
  uint32_t count_ = 0;
  uint32_t received_ = 0;
};

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
