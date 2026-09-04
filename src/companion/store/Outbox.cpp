#if CROSSPOINT_COMPANION

#include "Outbox.h"

#include <cstdio>
#include <cstring>

#include "../Log.h"

namespace companion {

namespace {

struct ScanState {
  uint32_t after;
  uint32_t best;
  bool found;
  uint32_t count;
  uint32_t maxSeq;
};

bool parseDigits(const char* name, uint32_t& seq) {
  // "<10 digits>.evt"
  if (strlen(name) != 14 || strcmp(name + 10, ".evt") != 0) return false;
  uint64_t v = 0;
  for (int i = 0; i < 10; ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
    v = v * 10 + static_cast<uint64_t>(name[i] - '0');
  }
  if (v == 0 || v > 0xFFFFFFFFu) return false;
  seq = static_cast<uint32_t>(v);
  return true;
}

bool scanVisitor(void* user, const DirEntry& e) {
  auto* s = static_cast<ScanState*>(user);
  uint32_t seq;
  if (e.isDir || !parseDigits(e.name, seq)) return true;
  ++s->count;
  if (seq > s->maxSeq) s->maxSeq = seq;
  if (seq > s->after && (!s->found || seq < s->best)) {
    s->best = seq;
    s->found = true;
  }
  return true;
}

struct AckCollect {
  uint32_t upTo;
  uint32_t seqs[64];
  size_t n;
};

bool ackVisitor(void* user, const DirEntry& e) {
  auto* c = static_cast<AckCollect*>(user);
  uint32_t seq;
  if (e.isDir || !parseDigits(e.name, seq) || seq > c->upTo) return true;
  c->seqs[c->n++] = seq;
  return c->n < sizeof(c->seqs) / sizeof(c->seqs[0]);
}

}  // namespace

Outbox::Outbox(FsPort& fs, SysPort& sys, const char* dir) : fs_(fs), sys_(sys) {
  strncpy(dir_, dir, kMaxDirLen);
  dir_[kMaxDirLen] = '\0';
}

Outbox::~Outbox() {
  if (scratch_) sys_.freeBig(scratch_);
}

bool Outbox::parseSeq(const char* name, uint32_t& seq) { return parseDigits(name, seq); }

void Outbox::eventPath(uint32_t seq, char* out, size_t cap) const {
  snprintf(out, cap, "%s/%010lu.evt", dir_, static_cast<unsigned long>(seq));
}

void Outbox::seqPath(char* out, size_t cap) const { snprintf(out, cap, "%s/seq", dir_); }

bool Outbox::begin() {
  if (!scratch_) {
    // One payload-sized encode buffer for the lifetime of the outbox (4 KiB, PSRAM).
    scratch_ = static_cast<uint8_t*>(sys_.allocBig(kMaxPayload));
    if (!scratch_) {
      CLOG_ERR("outbox: OOM %u", static_cast<unsigned>(kMaxPayload));
      return false;
    }
  }
  if (!fs_.mkdirs(dir_)) {
    CLOG_ERR("outbox: mkdir %s failed", dir_);
    return false;
  }
  char path[kMaxDirLen + 24];
  seqPath(path, sizeof(path));
  uint32_t persisted = 0;
  uint8_t buf[16];
  size_t len = 0;
  if (fs_.readAll(path, buf, sizeof(buf) - 1, len)) {
    buf[len] = 0;
    unsigned long v = 0;
    if (sscanf(reinterpret_cast<const char*>(buf), "%lu", &v) == 1) persisted = static_cast<uint32_t>(v);
  }
  ScanState s{0, 0, false, 0, 0};
  fs_.listDir(dir_, scanVisitor, &s);
  pending_ = s.count;
  lastSeq_ = persisted > s.maxSeq ? persisted : s.maxSeq;
  CLOG_INF("outbox: seq=%lu pending=%lu", static_cast<unsigned long>(lastSeq_), static_cast<unsigned long>(pending_));
  return true;
}

bool Outbox::persistSeq(uint32_t seq) {
  char path[kMaxDirLen + 24];
  seqPath(path, sizeof(path));
  char text[16];
  const int n = snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(seq));
  return n > 0 && fs_.writeAll(path, reinterpret_cast<const uint8_t*>(text), static_cast<size_t>(n));
}

bool Outbox::commit(uint32_t seq, const uint8_t* payload, size_t len) {
  if (!persistSeq(seq)) {
    CLOG_ERR("outbox: seq write failed");
    return false;
  }
  char path[kMaxDirLen + 24];
  eventPath(seq, path, sizeof(path));
  if (!fs_.writeAll(path, payload, len)) {
    CLOG_ERR("outbox: event write failed");
    // The seq is consumed either way; never reuse it.
    lastSeq_ = seq;
    return false;
  }
  lastSeq_ = seq;
  ++pending_;
  return true;
}

uint32_t Outbox::appendEncoded(proto::EventKind kind, const uint8_t* ctx, size_t ctxLen) {
  if (!scratch_) return 0;
  proto::Event e;
  e.seq = lastSeq_ + 1;
  e.kind = kind;
  e.ts = sys_.unixTime();
  e.ctx = proto::CborBytes{ctx, ctxLen};
  proto::CborWriter w(scratch_, kMaxPayload);
  if (!e.encode(w)) return 0;
  return commit(e.seq, scratch_, w.size()) ? e.seq : 0;
}

bool Outbox::nextAfter(uint32_t after, uint32_t& seq) {
  ScanState s{after, 0, false, 0, 0};
  if (!fs_.listDir(dir_, scanVisitor, &s)) return false;
  if (!s.found) return false;
  seq = s.best;
  return true;
}

bool Outbox::read(uint32_t seq, uint8_t* buf, size_t cap, size_t& len) {
  char path[kMaxDirLen + 24];
  eventPath(seq, path, sizeof(path));
  return fs_.readAll(path, buf, cap, len);
}

bool Outbox::ack(uint32_t upToSeq) {
  // Collect-then-delete in batches: deleting while iterating a FAT directory is unsafe.
  bool ok = true;
  for (;;) {
    AckCollect c{upToSeq, {}, 0};
    if (!fs_.listDir(dir_, ackVisitor, &c)) return false;
    if (c.n == 0) break;
    char path[kMaxDirLen + 24];
    for (size_t i = 0; i < c.n; ++i) {
      eventPath(c.seqs[i], path, sizeof(path));
      if (fs_.remove(path)) {
        if (pending_) --pending_;
      } else {
        ok = false;
      }
    }
    if (c.n < sizeof(c.seqs) / sizeof(c.seqs[0]) || !ok) break;
  }
  return ok;
}

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
