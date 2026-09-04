#if CROSSPOINT_COMPANION

#include "ComposeTarget.h"

#include <cstring>

namespace companion::brain {

namespace {

struct KindName {
  ComposeKind kind;
  const char* name;
  bool allowsNew;
};

constexpr KindName kKinds[] = {
    {ComposeKind::Thread, "thread", false}, {ComposeKind::Todo, "todo", true},
    {ComposeKind::Diary, "diary", false},   {ComposeKind::Note, "note", true},
    {ComposeKind::Person, "person", false},
};

bool isIdByte(const char c) {
  const auto u = static_cast<unsigned char>(c);
  return u > 0x20 && u < 0x7F;  // printable ASCII, no space
}

// yyyy-mm-dd with in-range month and day. Not a calendar check (no leap-year
// rule): the phone owns the real date, this only rejects typos and injections.
bool isIsoDate(const char* s, const size_t len) {
  if (len != 10) return false;
  for (size_t i = 0; i < len; ++i) {
    const bool dash = i == 4 || i == 7;
    if (dash != (s[i] == '-')) return false;
    if (!dash && (s[i] < '0' || s[i] > '9')) return false;
  }
  const int month = (s[5] - '0') * 10 + (s[6] - '0');
  const int day = (s[8] - '0') * 10 + (s[9] - '0');
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

}  // namespace

const char* composeKindName(const ComposeKind kind) {
  for (const auto& k : kKinds) {
    if (k.kind == kind) return k.name;
  }
  return nullptr;
}

bool ComposeTarget::isNew() const {
  if (strcmp(id, "new") != 0) return false;
  for (const auto& k : kKinds) {
    if (k.kind == kind) return k.allowsNew;
  }
  return false;
}

bool ComposeTarget::parse(const char* text, ComposeTarget& out) {
  out.kind = ComposeKind::Unknown;
  out.id[0] = '\0';
  if (!text) return false;
  const char* colon = strchr(text, ':');
  if (!colon || colon == text) return false;
  const size_t prefixLen = static_cast<size_t>(colon - text);

  ComposeKind kind = ComposeKind::Unknown;
  bool allowsNew = false;
  for (const auto& k : kKinds) {
    if (strlen(k.name) == prefixLen && strncmp(text, k.name, prefixLen) == 0) {
      kind = k.kind;
      allowsNew = k.allowsNew;
      break;
    }
  }
  if (kind == ComposeKind::Unknown) return false;

  const char* id = colon + 1;
  const size_t idLen = strlen(id);
  if (idLen == 0 || idLen > kMaxId) return false;
  for (size_t i = 0; i < idLen; ++i) {
    if (!isIdByte(id[i])) return false;
  }
  // A second colon would make the target ambiguous when the phone splits it.
  if (memchr(id, ':', idLen) != nullptr) return false;
  if (kind == ComposeKind::Diary && !isIsoDate(id, idLen)) return false;
  if (!allowsNew && idLen == 3 && memcmp(id, "new", 3) == 0) return false;

  out.kind = kind;
  memcpy(out.id, id, idLen);
  out.id[idLen] = '\0';
  return true;
}

bool composeTargetForList(const char* listId, const char* itemId, ComposeTarget& out) {
  out.kind = ComposeKind::Unknown;
  out.id[0] = '\0';
  if (!listId || !itemId) return false;

  static constexpr struct {
    const char* prefix;
    ComposeKind kind;
  } kMap[] = {
      {"inbox", ComposeKind::Thread}, {"thread", ComposeKind::Thread}, {"todo", ComposeKind::Todo},
      {"diary", ComposeKind::Diary},  {"note", ComposeKind::Note},     {"people", ComposeKind::Person},
      {"person", ComposeKind::Person},
  };
  ComposeKind kind = ComposeKind::Unknown;
  for (const auto& m : kMap) {
    const size_t n = strlen(m.prefix);
    if (strncmp(listId, m.prefix, n) == 0) {
      kind = m.kind;
      break;
    }
  }
  if (kind == ComposeKind::Unknown) return false;

  // Round-trip through parse() so the id gets the same validation a target off
  // the wire would: no spaces, no second colon, no over-long ids.
  char text[ComposeTarget::kMaxText + 1];
  const char* name = composeKindName(kind);
  const size_t nameLen = strlen(name);
  const size_t idLen = strlen(itemId);
  if (nameLen + 1 + idLen + 1 > sizeof(text)) return false;
  memcpy(text, name, nameLen);
  text[nameLen] = ':';
  memcpy(text + nameLen + 1, itemId, idLen + 1);
  return ComposeTarget::parse(text, out);
}

bool ComposeTarget::format(char* out, const size_t cap) const {
  const char* name = composeKindName(kind);
  if (!name || !out) return false;
  const size_t need = strlen(name) + 1 + strlen(id);
  if (need + 1 > cap) return false;
  memcpy(out, name, strlen(name));
  out[strlen(name)] = ':';
  memcpy(out + strlen(name) + 1, id, strlen(id) + 1);
  return true;
}

}  // namespace companion::brain

#endif  // CROSSPOINT_COMPANION
