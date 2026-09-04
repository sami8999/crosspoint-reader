#pragma once
#if CROSSPOINT_COMPANION

// The Compose target grammar shared by this firmware and the phone's SyncEngine
// (docs/INTEGRATION.md, "Compose target grammar"):
//
//   thread:<uuid>        reply to a message thread
//   todo:<uuid>          edit a todo          todo:new    create one
//   diary:<yyyy-mm-dd>   append a diary line
//   note:<uuid>          edit a note          note:new    create one
//   person:<uuid>        update a person's chapter
//
// Parsing and formatting round-trip byte-for-byte, so a target that came off a
// list file goes back out in Event{kind: Compose, ctx: {1: target}} unchanged.
// Pure logic, no allocation - the host tests drive it directly.

#include <cstddef>
#include <cstdint>

namespace companion::brain {

enum class ComposeKind : uint8_t { Unknown = 0, Thread, Todo, Diary, Note, Person };

// The prefix for a kind ("thread", "todo", ...) or nullptr for Unknown.
const char* composeKindName(ComposeKind kind);

struct ComposeTarget {
  // A UUID is 36 bytes; the ceiling leaves room for the source-prefixed ids the
  // message plugins mint without letting a list file smuggle in a long string.
  static constexpr size_t kMaxId = 48;
  static constexpr size_t kMaxText = 60;  // "person:" + kMaxId + NUL, rounded up

  ComposeKind kind = ComposeKind::Unknown;
  char id[kMaxId + 1] = {};

  bool valid() const { return kind != ComposeKind::Unknown; }
  // "new" is only meaningful for the two kinds that can be created from the
  // reader; a "thread:new" would have nowhere to go.
  bool isNew() const;

  // Strict parse: known prefix, exactly one ':', and an id of printable ASCII
  // with no spaces. `diary` additionally requires a yyyy-mm-dd date, which is
  // what makes a mistyped target fail here rather than on the phone.
  static bool parse(const char* text, ComposeTarget& out);
  // Writes "<kind>:<id>" into `out`; false when it does not fit or the target
  // is invalid.
  bool format(char* out, size_t cap) const;
};

// List files carry no target field, so the list id is the contract: the phone
// names each list after the record type it routes taps back to. Recognised
// prefixes are inbox/thread -> thread, todo -> todo, diary -> diary, note ->
// note, people/person -> person. Anything else has no Compose route and the
// reader must say so rather than guess a destination.
bool composeTargetForList(const char* listId, const char* itemId, ComposeTarget& out);

}  // namespace companion::brain

#endif  // CROSSPOINT_COMPANION
