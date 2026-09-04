# src/companion

Everything the X4 Pro Companion fork adds to CrossPoint lives here (see the monorepo's `docs/ARCHITECTURE.md`).
Upstream files are touched only at logged hook points (`docs/UPSTREAM_TOUCHPOINTS.md`).

- `proto/` — protocol v1 codec: CBOR (`Cbor.h`), frame/segment/bulk headers (`Frame.h`), typed messages (`Messages.h`). Zero heap use; caller-owned buffers.
- `port/` — the seams between protocol logic and hardware: `Ports.h` (FsPort/FsFile, HashPort, SysPort) and the device implementations in `SdPorts.*` (HalStorage, mbedtls SHA-256, HAL battery/RTC/heap, PSRAM allocator).
- `ble/` — the link. `BleServer.*` is the NimBLE GATT server (ESP-IDF NimBLE host from the arduino-esp32 core; advertising as "X4 Pro", `ctrl`/`bulk`/`info` characteristics, MTU 517, per-connection `Reassembler`, `Segmenter`-based notify ring, PSRAM buffers). It only queues inbound writes from the NimBLE host task; all protocol work runs on the main loop via `poll()`/`pump()`. `Session.*` is the per-connection state machine (Hello/HelloAck, Status cadence, Query, Ack/Nack rules of PROTOCOL.md §2.1, outbox flush). A reply that cannot be queued because the TX ring is full is held in a one-slot pending reply and retried from `tick()` before Status and the outbox, so §2.1's "must produce a reply" holds without dropping a `PushAck` (which would strand the transfer until its 60 s idle timeout). `Transfer.*` + `ChunkBitmap.*` implement PushFile → chunks → PushEnd with SHA-256 verification: files ≤ 1 MiB (`Transfer::kPsramBudget`) are assembled in PSRAM and written once, larger ones stream to `<path>.part` with seek-writes (`begin()` pre-sizes the `.part` file to the full length — SdFat's `seekSet` refuses to seek past EOF, so unordered chunk writes would otherwise all fail); the result is renamed into place and `clearBookCache()` is invoked for book files (same util as the web upload path).
- `store/Outbox.*` — append-only event log on SD (`/.companion/outbox/<seq:010>.evt` + `seq`), monotonic across reboots, flushed on connect, trimmed by `AckEvents`. `nextAfter()` serves a flush from a 64-entry cache of pending seqs (the tail of the same PSRAM scratch block), refreshed on append and rebuilt with one `listDir` per window, so flushing N events costs ceil(N/64) directory listings rather than N. Other lanes call `Outbox::append(kind, encoder)`; `companion::emitChord()` in `Companion.cpp` is the Chord hook (build with `-DCOMPANION_DEBUG_CHORD=1` to fire it from the screenshot chord).
- `Companion.*` — the only API `main.cpp` uses: `begin()`, `loop()`, `wantsFastLoop()`, `prepareForSleep()`, `emitChord()`.
- Every file is wrapped in `#if CROSSPOINT_COMPANION … #endif`, so the stock envs compile none of it. Build the fork with `pio run -e x4pro-companion`, which defines `CROSSPOINT_COMPANION=1` and uses `partitions-companion.csv` (7.75 MiB OTA slots).

Threading model: NimBLE host task → FreeRTOS queue (PSRAM) → main loop. Session/Transfer/Outbox never run off the main loop, so they share `HalStorage`, `APP_STATE` and `activityManager` with the rest of the firmware without extra locking. While USB Drive owns the SD card (`requiresExclusiveStorageLoop()`), `companion::loop()` is not ticked; inbound frames queue up (32 deep) and are dropped beyond that.

Host tests (gtest, no device needed):

    cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test --output-on-failure -j

- `test/companion_proto/` checks every golden vector in `protocol/vectors/v1/` (regenerate `vectors_v1.h` with `gen_vectors_header.py` after the vectors change).
- `test/companion_ble/` runs Session, Transfer, ChunkBitmap and Outbox against in-memory ports (`FakePorts.h`, including a real SHA-256).

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
