#pragma once
#if CROSSPOINT_COMPANION

// Phone → reader file transfer (PROTOCOL.md §4). Chunks are assembled either in a
// PSRAM buffer (size <= kPsramBudget, then written to `<path>.part` in one
// sequential write) or streamed straight into `<path>.part` with seek-writes
// (larger files). Either way receipt is tracked by a ChunkBitmap, the SHA-256 is
// checked on PushEnd and the file is renamed into place only when it matches.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "../port/Ports.h"
#include "../proto/Messages.h"
#include "ChunkBitmap.h"

namespace companion {

class Transfer {
 public:
  // Files up to this size are assembled in PSRAM (one contiguous block); the
  // device has 8 MiB and the biggest routine payloads (sleep cards ~48 KiB, list
  // files, Brain EPUBs) sit far below it. Anything larger streams to SD.
  static constexpr uint32_t kPsramBudget = 1u << 20;
  static constexpr uint32_t kMaxFileSize = 64u << 20;
  static constexpr uint32_t kMaxChunks = 65536;  // chunkIndex is u16
  static constexpr uint32_t kMaxChunkSize = 4096;
  static constexpr uint32_t kIdleTimeoutMs = 60000;
  static constexpr size_t kMaxPath = 200;
  static constexpr size_t kIoBuf = 4096;

  enum class BeginResult : uint8_t { Ok, Busy, BadRequest, IoError };

  Transfer(FsPort& fs, HashPort& hash, SysPort& sys);
  ~Transfer();

  // `maxChunkSize` is the link's MTU - 5 (PROTOCOL.md §2.1); larger chunkSize is a BadRequest.
  BeginResult begin(const proto::PushFile& req, uint32_t nowMs, uint32_t maxChunkSize = kMaxChunkSize);
  void onChunk(const proto::BulkChunk& chunk, uint32_t nowMs);
  // Handles PushEnd; always fills `ack`. On Missing the transfer stays active for
  // the resend round; every other status ends it.
  void end(uint32_t transferId, proto::PushAck& ack);
  // Aborts an idle transfer after kIdleTimeoutMs without chunks.
  void tick(uint32_t nowMs);
  void abort();

  bool active() const { return active_; }
  bool usesSd() const { return active_ && part_ != nullptr && data_ == nullptr; }
  uint32_t transferId() const { return id_; }
  const ChunkBitmap& bitmap() const { return bitmap_; }
  const char* path() const { return path_; }

  static bool validPath(const char* path, size_t len);

 private:
  proto::PushStatus finalize();
  bool hashMatches();
  void release();

  FsPort& fs_;
  HashPort& hash_;
  SysPort& sys_;

  bool active_ = false;
  uint32_t id_ = 0;
  uint32_t size_ = 0;
  uint32_t chunkSize_ = 0;
  uint32_t chunkCount_ = 0;
  uint32_t lastActivityMs_ = 0;
  uint8_t sha_[proto::kSha256Len] = {};
  char path_[kMaxPath + 1] = {};
  char partPath_[kMaxPath + 6] = {};

  uint8_t* data_ = nullptr;  // PSRAM assembly buffer (size_ bytes) or nullptr in SD mode
  uint8_t* io_ = nullptr;    // kIoBuf scratch for SD-mode hashing
  uint8_t* bits_ = nullptr;  // ChunkBitmap storage
  std::unique_ptr<FsFile> part_;
  ChunkBitmap bitmap_;
};

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
