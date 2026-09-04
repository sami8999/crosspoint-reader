# src/companion

Everything the X4 Pro Companion fork adds to CrossPoint lives here (see the monorepo's `docs/ARCHITECTURE.md`).
Upstream files are touched only at logged hook points (`docs/UPSTREAM_TOUCHPOINTS.md`).

- `proto/` — protocol v1 codec: CBOR (`Cbor.h`), frame/segment/bulk headers (`Frame.h`), typed messages (`Messages.h`). Zero heap use; caller-owned buffers.
- `port/` — the seams between protocol logic and hardware: `Ports.h` (FsPort/FsFile, HashPort, SysPort) and the device implementations in `SdPorts.*` (HalStorage, mbedtls SHA-256, HAL battery/RTC/heap, PSRAM allocator).
- `ble/` — the link. `BleServer.*` is the NimBLE GATT server (ESP-IDF NimBLE host from the arduino-esp32 core; advertising as "X4 Pro", `ctrl`/`bulk`/`info` characteristics, MTU 517, per-connection `Reassembler`, `Segmenter`-based notify ring, PSRAM buffers). It only queues inbound writes from the NimBLE host task; all protocol work runs on the main loop via `poll()`/`pump()`. `Session.*` is the per-connection state machine (Hello/HelloAck, Status cadence, Query, Ack/Nack rules of PROTOCOL.md §2.1, outbox flush). A reply that cannot be queued because the TX ring is full is held in a one-slot pending reply and retried from `tick()` before Status and the outbox, so §2.1's "must produce a reply" holds without dropping a `PushAck` (which would strand the transfer until its 60 s idle timeout). `Transfer.*` + `ChunkBitmap.*` implement PushFile → chunks → PushEnd with SHA-256 verification: files ≤ 1 MiB (`Transfer::kPsramBudget`) are assembled in PSRAM and written once, larger ones stream to `<path>.part` with seek-writes (`begin()` pre-sizes the `.part` file to the full length — SdFat's `seekSet` refuses to seek past EOF, so unordered chunk writes would otherwise all fail); the result is renamed into place and `clearBookCache()` is invoked for book files (same util as the web upload path).
- `chord/` — the universal chord (F3). `Chord.*` is pure logic: a debounced state machine over the two logical page-turn buttons plus the page-turn filter described under "The chord and page turns" below, and the `ChordContext` struct with its CBOR encoder. `ChordCapture.*` is the device half — it asks the foreground activity (`Activity::fillChordContext`) what the user is looking at and falls back to `ScreenshotInfo` + `APP_STATE` when the screen has nothing to say. Page text is capped at 2 KB (`ChordContext::kPageTextCap`), cut on a UTF-8 boundary: a full X4 Pro page is roughly 1200-1800 bytes, so that carries a whole page with room for CJK while leaving half the 4091-byte frame payload for the book path, the anchor and the rest of the ctx.
- `brain/` — the list files the phone pushes (`ios/Packages/Artifacts/LISTFILE.md`). `ListFile.*` is a one-pass streaming reader: bytes come through a 256-byte window (wider than the longest string the format allows), the header lands in a caller-owned struct, and each row is handed to a sink through one reused scratch. It allocates nothing, ever - over-long or unknown values are streamed past rather than buffered, so a file the reader did not write cannot make it allocate or overrun. `ListStore.*` is what holds a loaded list for the screen: two PSRAM blocks taken once (a row table and a string arena the rows point into), the state decorations built into the arena, and the optimistic marks a Tap earns until the phone pushes a replacement. `ComposeTarget.*` is the `docs/INTEGRATION.md` target grammar, parsed strictly and formatted back byte-for-byte.
- `ui/` — the screens. `BrainHomeActivity` is the Home "Brain" entry: it lists `/.companion/lists/*.list`, reading each file's header only (the format puts `itemCount`, `title` and `generatedAt` before the items, so the scan stops at the first row). `BrainListActivity` renders one list through `UiListActivity` - section headings as header rows in the same list, `badge`/`meta` in the right-hand value slot, and the state bits as visible decoration (see "Row state on a 1-bit screen"). `ComposeActivity` wraps `KeyboardEntryActivity` and adds the dictate affordance. `ReplyActivity` + `ReplyStore` display `ShowReply`. `ChordOverlay` is the "Listening…"/"Sent" panel.
- `store/Outbox.*` — append-only event log on SD (`/.companion/outbox/<seq:010>.evt` + `seq`), monotonic across reboots, flushed on connect, trimmed by `AckEvents`. `nextAfter()` serves a flush from a 64-entry cache of pending seqs (the tail of the same PSRAM scratch block), refreshed on append and rebuilt with one `listDir` per window, so flushing N events costs ceil(N/64) directory listings rather than N. Other lanes call `Outbox::append(kind, encoder)`; `companion::emitChord()` in `Companion.cpp` is the Chord hook (build with `-DCOMPANION_DEBUG_CHORD=1` to fire it from the screenshot chord).
- `Companion.*` — the only API `main.cpp` uses: `begin()`, `loop()`, `wantsFastLoop()`, `wantsStayAwake()`, `prepareForSleep()`, `emitChord()`.
- Every file is wrapped in `#if CROSSPOINT_COMPANION … #endif`, so the stock envs compile none of it. Build the fork with `pio run -e x4pro-companion`, which defines `CROSSPOINT_COMPANION=1` and uses `partitions-companion.csv` (7.75 MiB OTA slots).

Two link slots share one outbox (`BleServer::kMaxLinks` = 2) but hold independent
flush cursors, so an `AckEvents` from one phone could delete events the other is
still flushing. The rule: **a session refuses `AckEvents` with `Nack{4 busy}` while
any flush other than its own is in progress** (`Outbox::flushers()`), and the phone
retries — the window is one flush long. PROTOCOL.md v1 assumes a single phone;
anything better than this (per-phone cursors persisted on SD) waits for a protocol
that names the peer.

`SetCards`, `OpenBook` and `EnterWifiUpload` are decoded and answered with
`Nack{7 unsupported}` until their lanes land — the phone can tell "not wired up
yet" from `Nack{2 unknownType}`. `ShowReply` is live as of this lane (caps bit2,
`lists/actions`): `Session` hands the text to a `UiPort`, and a build without one
still answers `Nack{7}`.

## The chord and page turns

The chord is both page-turn buttons held together, read through
`MappedInputManager`'s logical `Left`/`Right` so remapping and the
orientation flip are already applied. It lives on the buttons the reader turns
pages with, so most of the design is about making sure it never turns one:

- `chordUpdate()` runs in `main.cpp` **before** `activityManager.loop()`, and
  while the combo is engaged (from the second press to the second release) it
  tells the loop to skip the activity entirely — the same early return the
  screenshot combo has always used. So the second press, every hold frame, and
  both releases never reach the reader.
- That still leaves the *first* press. With the default
  `longPressButtonBehavior = OFF` the reader turns pages on the press, so by the
  time the second button lands a page has already gone by.
  `ReaderUtils::detectPageTurn` therefore routes through
  `companion::filterPageTurn()`, which holds a turn that came from one of the two
  chord buttons for `pairWindowMs` (200 ms) and drops it if the chord fires in
  that window. A turn nobody comes back for inside `staleTurnMs` is dropped
  rather than fired late, so a reader that closed mid-window cannot hand its page
  turn to whatever screen is up next. Side-rocker, touch-zone and tilt turns are
  never delayed.
- The chord hold (180 ms) sits well under `ReaderUtils::SKIP_HOLD_MS` (700 ms, the
  chapter-skip hold), so a two-button hold is always read as a chord first, and a
  `releaseLockoutMs` quiet period after both buttons lift keeps contact bounce
  from firing a second one.

The overlay never repaints the page: it saves the pixels under a small panel with
`readFramebufferRegion`, draws, pushes a `FAST_REFRESH` (differential — only that
band is driven), and writes the pixels back when it expires. It comes down as soon
as the user does anything, **before** that input reaches the activity, so the
restore can never paste a stale band over a page the reader has since repainted.

## Row state on a 1-bit screen

FreeInkUI's list styles rows list-wide, not per row, so the state bits from
`LISTFILE.md` are carried in the row text itself, built into the store's arena
once rather than per render:

- **done** — a real strike-through: U+0336 COMBINING LONG STROKE OVERLAY after
  every non-space codepoint. `fontconvert.py` exports U+0300–U+036F into every
  built-in font, so it draws rather than boxing. The row also renders dimmed
  (`StateDisabled`, which `list()` treats as visual-only and keeps tappable).
- **unread** — a leading U+2022 BULLET, the mail-client dot.
- **pinned** — a leading U+2020 DAGGER.

Both markers are in General Punctuation, which the built-in UI fonts carry.
Ordering is the phone's job (`LISTFILE.md` says rows arrive in display order), so
`pinned` is a marker here, not a re-sort.

`accept` and `decline` are labelled "Accept in Calendar" / "Decline in Calendar"
and get no optimistic mark, because EventKit cannot RSVP to a real invitation
(`docs/INTEGRATION.md`, "EventKit limits") — the phone can only open Calendar, and
the reader must not draw the invitation as settled.

Threading model: NimBLE host task → FreeRTOS queue (PSRAM) → main loop. Session/Transfer/Outbox never run off the main loop, so they share `HalStorage`, `APP_STATE` and `activityManager` with the rest of the firmware without extra locking. While USB Drive owns the SD card (`requiresExclusiveStorageLoop()`), `companion::loop()` is not ticked; inbound frames queue up (32 deep) and are dropped beyond that.

Host tests (gtest, no device needed):

    cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test --output-on-failure -j

- `test/companion_proto/` checks every golden vector in `protocol/vectors/v1/` (regenerate `vectors_v1.h` with `gen_vectors_header.py` after the vectors change).
- `test/companion_ble/` runs Session, Transfer, ChunkBitmap and Outbox against in-memory ports (`FakePorts.h`, including a real SHA-256).
- `test/companion_ui/` runs the list-file reader, the row store, the chord state machine, the Compose grammar and the outbox encoding of `Chord`/`Tap`/`Compose`. The list-file goldens in `Goldens.h` were produced by the phone's own `ListFileBuilder` (a throwaway SwiftPM executable depending on `ios/Packages/Artifacts` by path), so the reader is checked against the normative writer rather than against hand-rolled CBOR.

## Sleep

`main.cpp` resets its `lastActivityTime` while `companion::wantsStayAwake()` is
true, so auto-sleep cannot cut the link off mid-task. "Busy" means a transfer in
flight, frames still queued for notification, an outbox backlog the phone has not
been sent yet, or a reply still owed to it (`Session::hasPendingWork()`).

A **merely connected, idle phone deliberately does not hold the reader awake.**
The phone is connected for most of the day; letting the connection alone defeat
auto-sleep would flatten the battery for no benefit, and BLE is re-established on
its own after a wake (deep sleep resets the chip, `setup()` calls
`companion::begin()` and advertising resumes). The cost is that a push started
during the sleep timeout's last moments can be interrupted — the phone sees the
disconnect and retries, and `PushFile` is idempotent because `.part` is discarded.

## Security (known limitations)

The GATT service is **unauthenticated and unencrypted**: `ble_hs_cfg.sm_bonding = 0`,
`sm_mitm = 0`, `sm_sc = 0` and no characteristic carries an encryption/authentication
permission (`BleServer::begin`). Any BLE central in range can connect, send `Hello`
and drive the whole protocol — there is no pairing step and nothing on the link is
confidential.

What that means today, and what limits it:

- **Writes and deletes are confined to an allow-list.** `Transfer::writablePath()`
  accepts only `/.companion/…`, `/.sleep/…` and `/Brain/…` — the directories the
  phone owns (`docs/INTEGRATION.md`) — for both `PushFile` and `DeleteFile`.
  Everything else (`/.crosspoint/settings.json`, credentials, the user's own books,
  caches) is refused with `Nack{5 notFound}`, which leaks nothing about what is on
  the card. Matching is case-insensitive because FAT/exFAT is: `/BRAIN/x` and
  `/Brain/x` are the same file, so a case-sensitive check would be theatre.
  CrossPoint has no fixed library directory — books live anywhere on the card — so
  no book directory is writable over the link.
- **`Query{files}` is still rooted at `/`.** Directory listings of the whole card
  (names and sizes, no contents) are readable by any peer in range, as are the
  `Status` fields (battery, current book path, free heap, uptime) and every pending
  outbox `Event` — which can contain page text, highlights and composed replies.
- **Residual risk:** an attacker in range can read that metadata, drain the outbox
  (and `AckEvents` it away), fill `/Brain` or `/.sleep` with junk, and delete the
  phone's own artifacts. They cannot brick the reader's settings or its library.

**TODO before the reader carries personal data** (highlights, diary, messages —
Lanes F3/F4): require pairing + encryption. Set `sm_bonding = 1`, `sm_sc = 1`, mark
`ctrl`/`bulk` `BLE_GATT_CHR_F_*_ENC` (and `_AUTHEN` once there is an out-of-band
confirmation path), persist bonds through `ble_store_config`, and reject
non-encrypted connections in `Session::handleHello`. The phone side must then hold
the bond too (`ReaderLink`).
