#if CROSSPOINT_COMPANION

#include "Transfer.h"

#include <cstring>

#include "../Log.h"

namespace companion {

namespace {

// Directories the phone owns (docs/INTEGRATION.md): lists/outbox/highlights/stats
// under /.companion, sleep cards under /.sleep, generated prose under /Brain.
// CrossPoint has no fixed library directory - the user's own books live anywhere
// on the card - so nothing else is writable over the link.
constexpr const char* kWritableRoots[] = {"/.companion/", "/.sleep/", "/Brain/"};

char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }

// FAT and exFAT are case-insensitive, so "/brain/x" and "/Brain/x" name the same
// file; the allow-list has to match the filesystem or it would only be theatre.
bool underRoot(const char* p, size_t len, const char* root) {
  size_t i = 0;
  for (; root[i]; ++i) {
    if (i >= len || fold(p[i]) != fold(root[i])) return false;
  }
  return len > i;  // a leaf name must follow the directory
}

}  // namespace

Transfer::Transfer(FsPort& fs, HashPort& hash, SysPort& sys) : fs_(fs), hash_(hash), sys_(sys) {}

Transfer::~Transfer() { release(); }

bool Transfer::validPath(const char* p, size_t len) {
  if (len < 2 || len > kMaxPath || p[0] != '/' || p[len - 1] == '/') return false;
  for (size_t i = 0; i < len; ++i) {
    if (p[i] == '\0' || p[i] == '\\') return false;
  }
  // Reject "." and ".." segments and empty segments.
  size_t segStart = 1;
  for (size_t i = 1; i <= len; ++i) {
    if (i == len || p[i] == '/') {
      const size_t segLen = i - segStart;
      if (segLen == 0) return false;
      if (segLen == 1 && p[segStart] == '.') return false;
      if (segLen == 2 && p[segStart] == '.' && p[segStart + 1] == '.') return false;
      segStart = i + 1;
    }
  }
  return true;
}

bool Transfer::writablePath(const char* p, size_t len) {
  if (!validPath(p, len)) return false;
  for (const char* root : kWritableRoots) {
    if (underRoot(p, len, root)) return true;
  }
  return false;
}

void Transfer::release() {
  part_.reset();
  if (data_) sys_.freeBig(data_);
  if (io_) sys_.freeBig(io_);
  if (bits_) sys_.freeBig(bits_);
  data_ = io_ = bits_ = nullptr;
  bitmap_.detach();
  active_ = false;
}

void Transfer::abort() {
  if (!active_) return;
  CLOG_INF("xfer %lu: abort", static_cast<unsigned long>(id_));
  release();
  if (fs_.exists(partPath_)) fs_.remove(partPath_);
}

Transfer::BeginResult Transfer::begin(const proto::PushFile& req, uint32_t nowMs, uint32_t maxChunkSize) {
  if (active_) return BeginResult::Busy;
  if (!validPath(req.path.data(), req.path.size())) return BeginResult::BadRequest;
  if (!writablePath(req.path.data(), req.path.size())) {
    CLOG_ERR("xfer: write to %.*s refused (outside the companion roots)", static_cast<int>(req.path.size()),
             req.path.data());
    return BeginResult::Denied;
  }
  if (maxChunkSize > kMaxChunkSize) maxChunkSize = kMaxChunkSize;
  if (req.size > kMaxFileSize || req.chunkSize == 0 || req.chunkSize > maxChunkSize) return BeginResult::BadRequest;
  if (req.sha256.len != proto::kSha256Len) return BeginResult::BadRequest;
  const uint32_t chunks = req.size == 0 ? 0 : (req.size + req.chunkSize - 1) / req.chunkSize;
  if (chunks > kMaxChunks) return BeginResult::BadRequest;

  memcpy(path_, req.path.data(), req.path.size());
  path_[req.path.size()] = '\0';
  snprintf(partPath_, sizeof(partPath_), "%s.part", path_);
  memcpy(sha_, req.sha256.data, proto::kSha256Len);
  id_ = req.transferId;
  size_ = req.size;
  chunkSize_ = req.chunkSize;
  chunkCount_ = chunks;
  lastActivityMs_ = nowMs;

  // Bitmap: <= 8 KiB for 65536 chunks. Allocated per transfer, freed on completion.
  const size_t bitBytes = ChunkBitmap::bytesFor(chunkCount_);
  bits_ = static_cast<uint8_t*>(sys_.allocBig(bitBytes ? bitBytes : 1));
  if (!bits_) {
    CLOG_ERR("xfer: OOM bitmap %u", static_cast<unsigned>(bitBytes));
    release();
    return BeginResult::IoError;
  }
  bitmap_.attach(bits_, chunkCount_);

  // Make sure the parent directory exists before committing to the transfer.
  const char* slash = strrchr(path_, '/');
  if (slash && slash != path_) {
    char dir[kMaxPath + 1];
    const size_t n = static_cast<size_t>(slash - path_);
    memcpy(dir, path_, n);
    dir[n] = '\0';
    if (!fs_.mkdirs(dir)) {
      CLOG_ERR("xfer: mkdir %s failed", dir);
      release();
      return BeginResult::IoError;
    }
  }

  if (size_ <= kPsramBudget) {
    // Whole-file PSRAM assembly (size_ bytes, freed on completion or abort).
    if (size_) {
      data_ = static_cast<uint8_t*>(sys_.allocBig(size_));
      if (!data_) {
        CLOG_ERR("xfer: OOM %lu bytes, streaming to SD instead", static_cast<unsigned long>(size_));
      }
    }
  }
  if (!data_ && size_) {
    // SD streaming: `.part` receives seek-writes; io_ (4 KiB) is the hash scratch.
    io_ = static_cast<uint8_t*>(sys_.allocBig(kIoBuf));
    part_ = fs_.openWrite(partPath_);
    if (!io_ || !part_) {
      CLOG_ERR("xfer: cannot open %s", partPath_);
      release();
      fs_.remove(partPath_);
      return BeginResult::IoError;
    }
    // Chunks are unordered (PROTOCOL.md 1.4), so `.part` must already be size_
    // bytes long before the first seek-write: SdFat's FatFile::seekSet fails for
    // pos > fileSize, which would drop every gapped chunk.
    if (!preSizePart()) {
      CLOG_ERR("xfer: cannot pre-size %s to %lu bytes", partPath_, static_cast<unsigned long>(size_));
      release();
      fs_.remove(partPath_);
      return BeginResult::IoError;
    }
  }
  active_ = true;
  CLOG_INF("xfer %lu: %s %lu bytes, %lu chunks x %lu, %s", static_cast<unsigned long>(id_), path_,
           static_cast<unsigned long>(size_), static_cast<unsigned long>(chunkCount_),
           static_cast<unsigned long>(chunkSize_), data_ ? "psram" : "sd");
  return BeginResult::Ok;
}

// One FAT operation when the backend supports it; otherwise a sequential
// zero-fill through io_ (already allocated, so no extra memory).
bool Transfer::preSizePart() {
  if (part_->preAllocate(size_)) return true;
  memset(io_, 0, kIoBuf);
  for (uint32_t off = 0; off < size_;) {
    const size_t n = size_ - off < kIoBuf ? size_ - off : kIoBuf;
    if (!part_->writeAt(off, io_, n)) return false;
    off += static_cast<uint32_t>(n);
  }
  return part_->flush();
}

void Transfer::onChunk(const proto::BulkChunk& chunk, uint32_t nowMs) {
  if (!active_ || chunk.index >= chunkCount_) return;
  const uint32_t offset = static_cast<uint32_t>(chunk.index) * chunkSize_;
  const uint32_t expect = chunk.index + 1 == chunkCount_ ? size_ - offset : chunkSize_;
  if (chunk.len != expect) {
    CLOG_DBG("xfer: chunk %u bad len %u (want %lu)", chunk.index, static_cast<unsigned>(chunk.len),
             static_cast<unsigned long>(expect));
    return;
  }
  lastActivityMs_ = nowMs;
  if (data_) {
    memcpy(data_ + offset, chunk.data, chunk.len);
  } else if (part_) {
    if (!part_->writeAt(offset, chunk.data, chunk.len)) {
      CLOG_ERR("xfer: write @%lu failed", static_cast<unsigned long>(offset));
      return;  // left unmarked; PushEnd reports it missing
    }
  }
  bitmap_.set(chunk.index);
}

bool Transfer::hashMatches() {
  uint8_t out[proto::kSha256Len];
  hash_.start();
  if (data_) {
    hash_.update(data_, size_);
  } else if (part_) {
    if (!part_->flush()) return false;
    uint32_t off = 0;
    while (off < size_) {
      size_t got = 0;
      const size_t want = size_ - off < kIoBuf ? size_ - off : kIoBuf;
      if (!part_->readAt(off, io_, want, got) || got == 0) return false;
      hash_.update(io_, got);
      off += static_cast<uint32_t>(got);
    }
  }
  hash_.finish(out);
  return memcmp(out, sha_, proto::kSha256Len) == 0;
}

proto::PushStatus Transfer::finalize() {
  if (!hashMatches()) {
    CLOG_ERR("xfer %lu: sha256 mismatch", static_cast<unsigned long>(id_));
    return proto::PushStatus::HashMismatch;
  }
  if (data_ || size_ == 0) {
    auto f = fs_.openWrite(partPath_);
    if (!f || (size_ && !f->writeAt(0, data_, size_)) || !f->flush()) {
      CLOG_ERR("xfer: write %s failed", partPath_);
      return proto::PushStatus::IoError;
    }
  } else if (part_ && !part_->flush()) {
    return proto::PushStatus::IoError;
  }
  part_.reset();  // close before rename
  if (fs_.exists(path_) && !fs_.remove(path_)) {
    CLOG_ERR("xfer: remove %s failed", path_);
    return proto::PushStatus::IoError;
  }
  if (!fs_.rename(partPath_, path_)) {
    CLOG_ERR("xfer: rename %s failed", partPath_);
    return proto::PushStatus::IoError;
  }
  fs_.onFileReplaced(path_);
  CLOG_INF("xfer %lu: %s complete", static_cast<unsigned long>(id_), path_);
  return proto::PushStatus::Ok;
}

void Transfer::end(uint32_t transferId, proto::PushAck& ack) {
  ack.transferId = transferId;
  ack.missingCount = 0;
  if (!active_ || transferId != id_) {
    ack.status = proto::PushStatus::UnknownTransfer;
    return;
  }
  if (!bitmap_.complete()) {
    ack.status = proto::PushStatus::Missing;
    ack.missingCount = bitmap_.missing(ack.missing, proto::kMaxMissingChunks);
    CLOG_INF("xfer %lu: %lu chunks missing", static_cast<unsigned long>(id_),
             static_cast<unsigned long>(bitmap_.missingCount()));
    return;
  }
  ack.status = finalize();
  release();
  if (ack.status != proto::PushStatus::Ok && fs_.exists(partPath_)) fs_.remove(partPath_);
}

void Transfer::tick(uint32_t nowMs) {
  if (active_ && nowMs - lastActivityMs_ > kIdleTimeoutMs) {
    CLOG_INF("xfer %lu: idle timeout", static_cast<unsigned long>(id_));
    abort();
  }
}

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
