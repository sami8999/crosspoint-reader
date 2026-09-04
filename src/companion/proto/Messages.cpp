#if CROSSPOINT_COMPANION

#include "Messages.h"

namespace companion::proto {

namespace {

constexpr unsigned bit(unsigned key) { return 1u << key; }

// Walks a uint-keyed map, tracking which keys (< 32) were present.
class MapDecoder {
 public:
  explicit MapDecoder(CborReader& r) : r_(r) { ok_ = r.enterMap(left_); }

  bool next(uint64_t& key) {
    if (!ok_ || left_ == 0) return false;
    --left_;
    ok_ = r_.readUint(key);
    if (ok_ && key < 32) seen_ |= bit(static_cast<unsigned>(key));
    return ok_;
  }
  bool finish(unsigned required) const { return ok_ && (seen_ & required) == required; }

 private:
  CborReader& r_;
  size_t left_ = 0;
  unsigned seen_ = 0;
  bool ok_;
};

template <class E>
bool readEnum(CborReader& r, E& out) {
  uint32_t v;
  if (!r.readUint32(v) || v > 0xFF) return false;
  out = static_cast<E>(v);
  return true;
}

template <class E>
uint64_t enumValue(E e) {
  return static_cast<uint64_t>(static_cast<uint8_t>(e));
}

template <class T>
bool decodeArray(CborReader& r, T* items, size_t cap, size_t& count) {
  size_t n;
  if (!r.enterArray(n) || n > cap) return false;
  for (size_t i = 0; i < n; ++i) {
    items[i] = T{};
    if (!items[i].decode(r)) return false;
  }
  count = n;
  return true;
}

template <class T>
bool encodeArray(CborWriter& w, const T* items, size_t count, size_t cap) {
  if (count > cap || !w.writeArrayHeader(count)) return false;
  for (size_t i = 0; i < count; ++i) {
    if (!items[i].encode(w)) return false;
  }
  return true;
}

}  // namespace

// ------------------------------------------------------------ sub-structs

bool Anchor::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(xpath); break;
      case 2: ok = r.readUint32(visibleTextOffset); break;
      case 3: ok = r.readUint32(spine); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2) | bit(3));
}

bool Anchor::encode(CborWriter& w) const {
  return w.writeMapHeader(3) && w.keyTstr(1, xpath) && w.keyUint(2, visibleTextOffset) && w.keyUint(3, spine);
}

bool CardEntry::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(path); break;
      case 2: ok = r.readUint32(fromMin); break;
      case 3: ok = r.readUint32(toMin); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1));
}

bool CardEntry::encode(CborWriter& w) const {
  return w.writeMapHeader(1 + fromMin.has_value() + toMin.has_value()) && w.keyTstr(1, path) &&
         (!fromMin || w.keyUint(2, *fromMin)) && (!toMin || w.keyUint(3, *toMin));
}

bool FileEntry::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(name); break;
      case 2: ok = r.readUint32(size); break;
      case 3: ok = r.readBool(isDir); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2) | bit(3));
}

bool FileEntry::encode(CborWriter& w) const {
  return w.writeMapHeader(3) && w.keyTstr(1, name) && w.keyUint(2, size) && w.keyBool(3, isDir);
}

// ------------------------------------------------------------ phone → reader

bool Hello::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readUint32(proto); break;
      case 2: ok = r.readTstr(app); break;
      case 3: ok = r.readUint32(caps); break;
      case 4: ok = r.readUint32(clock); break;
      case 5: ok = r.readInt32(tzOffsetMin); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2) | bit(3) | bit(4) | bit(5));
}

bool Hello::encode(CborWriter& w) const {
  return w.writeMapHeader(5) && w.keyUint(1, proto) && w.keyTstr(2, app) && w.keyUint(3, caps) &&
         w.keyUint(4, clock) && w.keyInt(5, tzOffsetMin);
}

bool PushFile::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(path); break;
      case 2: ok = r.readUint32(size); break;
      case 3: ok = r.readBstr(sha256) && sha256.len == kSha256Len; break;
      case 4: ok = r.readUint32(chunkSize); break;
      case 5: ok = r.readUint32(transferId); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2) | bit(3) | bit(4) | bit(5));
}

bool PushFile::encode(CborWriter& w) const {
  return sha256.len == kSha256Len && w.writeMapHeader(5) && w.keyTstr(1, path) && w.keyUint(2, size) &&
         w.keyBstr(3, sha256) && w.keyUint(4, chunkSize) && w.keyUint(5, transferId);
}

bool PushEnd::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    if (!(k == 1 ? r.readUint32(transferId) : r.skip())) return false;
  }
  return m.finish(bit(1));
}

bool PushEnd::encode(CborWriter& w) const { return w.writeMapHeader(1) && w.keyUint(1, transferId); }

bool DeleteFile::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    if (!(k == 1 ? r.readTstr(path) : r.skip())) return false;
  }
  return m.finish(bit(1));
}

bool DeleteFile::encode(CborWriter& w) const { return w.writeMapHeader(1) && w.keyTstr(1, path); }

bool SetCards::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = readEnum(r, mode); break;
      case 2: ok = decodeArray(r, entries, kMaxCardEntries, entryCount); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2));
}

bool SetCards::encode(CborWriter& w) const {
  return w.writeMapHeader(2) && w.keyUint(1, enumValue(mode)) && w.writeUint(2) &&
         encodeArray(w, entries, entryCount, kMaxCardEntries);
}

bool OpenBook::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    if (!(k == 1 ? r.readTstr(path) : r.skip())) return false;
  }
  return m.finish(bit(1));
}

bool OpenBook::encode(CborWriter& w) const { return w.writeMapHeader(1) && w.keyTstr(1, path); }

bool ShowReply::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(text); break;
      case 2: ok = r.readTstr(title); break;
      case 3: ok = r.readUint32(forEventSeq); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1));
}

bool ShowReply::encode(CborWriter& w) const {
  return w.writeMapHeader(1 + title.has_value() + forEventSeq.has_value()) && w.keyTstr(1, text) &&
         (!title || w.keyTstr(2, *title)) && (!forEventSeq || w.keyUint(3, *forEventSeq));
}

bool AckEvents::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    if (!(k == 1 ? r.readUint32(upToSeq) : r.skip())) return false;
  }
  return m.finish(bit(1));
}

bool AckEvents::encode(CborWriter& w) const { return w.writeMapHeader(1) && w.keyUint(1, upToSeq); }

bool Query::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = readEnum(r, what); break;
      case 2: ok = r.readTstr(path); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1));
}

bool Query::encode(CborWriter& w) const {
  return w.writeMapHeader(1 + path.has_value()) && w.keyUint(1, enumValue(what)) && (!path || w.keyTstr(2, *path));
}

bool EnterWifiUpload::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = readEnum(r, mode); break;
      case 2: ok = r.readTstr(ssid); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1));
}

bool EnterWifiUpload::encode(CborWriter& w) const {
  return w.writeMapHeader(1 + ssid.has_value()) && w.keyUint(1, enumValue(mode)) && (!ssid || w.keyTstr(2, *ssid));
}

bool Ack::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    if (!(k == 1 ? r.readUint32(seq) : r.skip())) return false;
  }
  return m.finish(bit(1));
}

bool Ack::encode(CborWriter& w) const { return w.writeMapHeader(1) && w.keyUint(1, seq); }

bool Nack::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readUint32(seq); break;
      case 2: ok = readEnum(r, code); break;
      case 3: ok = r.readTstr(msg); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2));
}

bool Nack::encode(CborWriter& w) const {
  return w.writeMapHeader(2 + msg.has_value()) && w.keyUint(1, seq) && w.keyUint(2, enumValue(code)) &&
         (!msg || w.keyTstr(3, *msg));
}

// ------------------------------------------------------------ reader → phone

bool HelloAck::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readUint32(proto); break;
      case 2: ok = r.readTstr(fw); break;
      case 3: ok = r.readUint32(caps); break;
      case 4: ok = r.readInt32(clockDelta); break;
      case 5: ok = r.readTstr(device); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2) | bit(3) | bit(4) | bit(5));
}

bool HelloAck::encode(CborWriter& w) const {
  return w.writeMapHeader(5) && w.keyUint(1, proto) && w.keyTstr(2, fw) && w.keyUint(3, caps) &&
         w.keyInt(4, clockDelta) && w.keyTstr(5, device);
}

bool Status::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readUint32(battery); break;
      case 2: ok = r.readBool(charging); break;
      case 3: ok = r.readTstr(book); break;
      case 4: ok = r.readUint32(permille); break;
      case 5: ok = r.readUint32(freeHeap); break;
      case 6: ok = r.readUint32(outboxCount); break;
      case 7: ok = r.readUint32(uptime); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2) | bit(5) | bit(6) | bit(7));
}

bool Status::encode(CborWriter& w) const {
  return w.writeMapHeader(5 + book.has_value() + permille.has_value()) && w.keyUint(1, battery) &&
         w.keyBool(2, charging) && (!book || w.keyTstr(3, *book)) && (!permille || w.keyUint(4, *permille)) &&
         w.keyUint(5, freeHeap) && w.keyUint(6, outboxCount) && w.keyUint(7, uptime);
}

bool ChordCtx::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(screen); break;
      case 2: ok = r.readTstr(book); break;
      case 3: ok = r.readUint32(spine); break;
      case 4: ok = r.readUint32(page); break;
      case 5: ok = r.readTstr(pageText); break;
      case 6: {
        Anchor a;
        ok = a.decode(r);
        if (ok) anchor = a;
        break;
      }
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1));
}

bool ChordCtx::encode(CborWriter& w) const {
  const size_t n = 1 + book.has_value() + spine.has_value() + page.has_value() + pageText.has_value() + anchor.has_value();
  return w.writeMapHeader(n) && w.keyTstr(1, screen) && (!book || w.keyTstr(2, *book)) &&
         (!spine || w.keyUint(3, *spine)) && (!page || w.keyUint(4, *page)) &&
         (!pageText || w.keyTstr(5, *pageText)) && (!anchor || (w.writeUint(6) && anchor->encode(w)));
}

bool TapCtx::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(listId); break;
      case 2: ok = r.readTstr(itemId); break;
      case 3: ok = readEnum(r, action); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2) | bit(3));
}

bool TapCtx::encode(CborWriter& w) const {
  return w.writeMapHeader(3) && w.keyTstr(1, listId) && w.keyTstr(2, itemId) && w.keyUint(3, enumValue(action));
}

bool HighlightCtx::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(book); break;
      case 2: ok = anchor.decode(r); break;
      case 3: ok = r.readUint32(len); break;
      case 4: ok = r.readTstr(text); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2) | bit(3) | bit(4));
}

bool HighlightCtx::encode(CborWriter& w) const {
  return w.writeMapHeader(4) && w.keyTstr(1, book) && w.writeUint(2) && anchor.encode(w) && w.keyUint(3, len) &&
         w.keyTstr(4, text);
}

bool ProgressCtx::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(book); break;
      case 2: ok = r.readUint32(permille); break;
      case 3: ok = r.readUint32(wpm); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2) | bit(3));
}

bool ProgressCtx::encode(CborWriter& w) const {
  return w.writeMapHeader(3) && w.keyTstr(1, book) && w.keyUint(2, permille) && w.keyUint(3, wpm);
}

bool SessionEndCtx::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(book); break;
      case 2: ok = r.readUint32(durationS); break;
      case 3: ok = r.readUint32(pages); break;
      case 4: ok = r.readUint32(words); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2) | bit(3) | bit(4));
}

bool SessionEndCtx::encode(CborWriter& w) const {
  return w.writeMapHeader(4) && w.keyTstr(1, book) && w.keyUint(2, durationS) && w.keyUint(3, pages) &&
         w.keyUint(4, words);
}

bool ComposeCtx::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(target); break;
      case 2: ok = r.readTstr(text); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2));
}

bool ComposeCtx::encode(CborWriter& w) const {
  return w.writeMapHeader(2) && w.keyTstr(1, target) && w.keyTstr(2, text);
}

bool LookupCtx::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(word); break;
      case 2: ok = r.readTstr(book); break;
      case 3: ok = r.readTstr(sentence); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1));
}

bool LookupCtx::encode(CborWriter& w) const {
  return w.writeMapHeader(1 + book.has_value() + sentence.has_value()) && w.keyTstr(1, word) &&
         (!book || w.keyTstr(2, *book)) && (!sentence || w.keyTstr(3, *sentence));
}

bool Event::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readUint32(seq); break;
      case 2: ok = readEnum(r, kind); break;
      case 3: ok = r.readUint32(ts); break;
      case 4: {
        CborMajor major;
        ok = r.peekMajor(major) && major == CborMajor::Map && r.readRaw(ctx);
        break;
      }
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2) | bit(3) | bit(4));
}

bool Event::encodeHead(CborWriter& w) const {
  return w.writeMapHeader(4) && w.keyUint(1, seq) && w.keyUint(2, enumValue(kind)) && w.keyUint(3, ts) &&
         w.writeUint(4);
}

bool Event::encode(CborWriter& w) const {
  if (!encodeHead(w)) return false;
  return ctx.data ? w.writeRaw(ctx) : w.writeMapHeader(0);
}

bool PushAck::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readUint32(transferId); break;
      case 2: ok = readEnum(r, status); break;
      case 3: {
        size_t n;
        ok = r.enterArray(n) && n <= kMaxMissingChunks;
        for (size_t i = 0; ok && i < n; ++i) {
          uint32_t v;
          ok = r.readUint32(v) && v <= 0xFFFF;
          missing[i] = static_cast<uint16_t>(v);
        }
        if (ok) missingCount = n;
        break;
      }
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2));
}

bool PushAck::encode(CborWriter& w) const {
  if (missingCount > kMaxMissingChunks) return false;
  if (!w.writeMapHeader(2 + (missingCount > 0)) || !w.keyUint(1, transferId) || !w.keyUint(2, enumValue(status))) {
    return false;
  }
  if (missingCount == 0) return true;
  if (!w.writeUint(3) || !w.writeArrayHeader(missingCount)) return false;
  for (size_t i = 0; i < missingCount; ++i) {
    if (!w.writeUint(missing[i])) return false;
  }
  return true;
}

bool Files::decode(CborReader& r) {
  MapDecoder m(r);
  for (uint64_t k; m.next(k);) {
    bool ok;
    switch (k) {
      case 1: ok = r.readTstr(path); break;
      case 2: ok = decodeArray(r, entries, kMaxFileEntries, entryCount); break;
      default: ok = r.skip(); break;
    }
    if (!ok) return false;
  }
  return m.finish(bit(1) | bit(2));
}

bool Files::encode(CborWriter& w) const {
  return w.writeMapHeader(2) && w.keyTstr(1, path) && w.writeUint(2) &&
         encodeArray(w, entries, entryCount, kMaxFileEntries);
}

}  // namespace companion::proto

#endif  // CROSSPOINT_COMPANION
