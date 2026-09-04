#pragma once
#if CROSSPOINT_COMPANION

// Append-only event log on SD (PROTOCOL.md §2). Every reader-originated Event is
// written as one file `<dir>/<seq:010u>.evt` holding the encoded Event payload
// (CBOR map, ready to be wrapped in a 0x83 frame). `<dir>/seq` stores the last
// assigned seq in ASCII so seqs stay monotonic across reboots; it is written before
// the event file, and begin() also takes the max over existing files, so a crash
// between the two writes can skip a number but never reuse one.

#include <cstddef>
#include <cstdint>

#include "../port/Ports.h"
#include "../proto/Cbor.h"
#include "../proto/Messages.h"

namespace companion {

class Outbox {
 public:
  static constexpr size_t kMaxPayload = proto::kMaxPayloadSize;
  static constexpr size_t kMaxDirLen = 48;
  static constexpr const char* kDefaultDir = "/.companion/outbox";

  Outbox(FsPort& fs, SysPort& sys, const char* dir = kDefaultDir);
  ~Outbox();

  // Creates the directory, loads the seq counter and counts pending events.
  bool begin();
  bool ready() const { return scratch_ != nullptr; }

  uint32_t pending() const { return pending_; }
  uint32_t lastSeq() const { return lastSeq_; }

  // Appends an event whose ctx map is written by `encodeCtx(CborWriter&) -> bool`.
  // Returns the new seq, or 0 on failure.
  template <class F>
  uint32_t append(proto::EventKind kind, F&& encodeCtx) {
    struct Adapter {
      F& f;
      bool encode(proto::CborWriter& w) const { return f(w); }
    };
    return appendWith(kind, Adapter{encodeCtx});
  }
  // Appends an event with a pre-encoded ctx map.
  uint32_t appendEncoded(proto::EventKind kind, const uint8_t* ctx, size_t ctxLen);

  // Smallest pending seq greater than `after`; false when none.
  bool nextAfter(uint32_t after, uint32_t& seq);
  // Reads one pending event payload.
  bool read(uint32_t seq, uint8_t* buf, size_t cap, size_t& len);
  // Deletes every pending event with seq <= upToSeq.
  bool ack(uint32_t upToSeq);

 private:
  template <class Ctx>
  uint32_t appendWith(proto::EventKind kind, const Ctx& ctx) {
    if (!scratch_) return 0;
    proto::Event e;
    e.seq = lastSeq_ + 1;
    e.kind = kind;
    e.ts = sys_.unixTime();
    proto::CborWriter w(scratch_, kMaxPayload);
    if (!e.encodeWith(w, ctx)) return 0;
    return commit(e.seq, scratch_, w.size()) ? e.seq : 0;
  }
  bool commit(uint32_t seq, const uint8_t* payload, size_t len);
  bool persistSeq(uint32_t seq);
  void eventPath(uint32_t seq, char* out, size_t cap) const;
  void seqPath(char* out, size_t cap) const;
  static bool parseSeq(const char* name, uint32_t& seq);

  FsPort& fs_;
  SysPort& sys_;
  char dir_[kMaxDirLen + 1] = {};
  uint8_t* scratch_ = nullptr;  // kMaxPayload bytes, PSRAM on device
  uint32_t lastSeq_ = 0;
  uint32_t pending_ = 0;
};

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
