# src/companion

Everything the X4 Pro Companion fork adds to CrossPoint lives here (see the monorepo's `docs/ARCHITECTURE.md`).
Upstream files are touched only at logged hook points (`docs/UPSTREAM_TOUCHPOINTS.md`).

- `proto/` — protocol v1 codec: CBOR (`Cbor.h`), frame/segment/bulk headers (`Frame.h`), typed messages (`Messages.h`). Zero heap use; caller-owned buffers.
- `port/` — the seams between protocol logic and hardware: `Ports.h` (FsPort/FsFile, HashPort, SysPort) and the device implementations in `SdPorts.*` (HalStorage, mbedtls SHA-256, HAL battery/RTC/heap, PSRAM allocator).
- `ble/` — the link. `BleServer.*` is the NimBLE GATT server (ESP-IDF NimBLE host from the arduino-esp32 core; advertising as "X4 Pro", `ctrl`/`bulk`/`info` characteristics, MTU 517, per-connection `Reassembler`, `Segmenter`-based notify ring, PSRAM buffers). It only queues inbound writes from the NimBLE host task; all protocol work runs on the main loop via `poll()`/`pump()`. `Session.*` is the per-connection state machine (Hello/HelloAck, Status cadence, Query, Ack/Nack rules of PROTOCOL.md §2.1, outbox flush). `Transfer.*` + `ChunkBitmap.*` implement PushFile → chunks → PushEnd with SHA-256 verification: files ≤ 1 MiB (`Transfer::kPsramBudget`) are assembled in PSRAM and written once, larger ones stream to `<path>.part` with seek-writes; the result is renamed into place and `clearBookCache()` is invoked for book files (same util as the web upload path).
- `store/Outbox.*` — append-only event log on SD (`/.companion/outbox/<seq:010>.evt` + `seq`), monotonic across reboots, flushed on connect, trimmed by `AckEvents`. Other lanes call `Outbox::append(kind, encoder)`; `companion::emitChord()` in `Companion.cpp` is the Chord hook (build with `-DCOMPANION_DEBUG_CHORD=1` to fire it from the screenshot chord).
- `Companion.*` — the only API `main.cpp` uses: `begin()`, `loop()`, `wantsFastLoop()`, `prepareForSleep()`, `emitChord()`.
- Every file is wrapped in `#if CROSSPOINT_COMPANION … #endif`, so the stock envs compile none of it. Build the fork with `pio run -e x4pro-companion`, which defines `CROSSPOINT_COMPANION=1` and uses `partitions-companion.csv` (7.75 MiB OTA slots).

Threading model: NimBLE host task → FreeRTOS queue (PSRAM) → main loop. Session/Transfer/Outbox never run off the main loop, so they share `HalStorage`, `APP_STATE` and `activityManager` with the rest of the firmware without extra locking. While USB Drive owns the SD card (`requiresExclusiveStorageLoop()`), `companion::loop()` is not ticked; inbound frames queue up (32 deep) and are dropped beyond that.

Host tests (gtest, no device needed):

    cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test --output-on-failure -j

- `test/companion_proto/` checks every golden vector in `protocol/vectors/v1/` (regenerate `vectors_v1.h` with `gen_vectors_header.py` after the vectors change).
- `test/companion_ble/` runs Session, Transfer, ChunkBitmap and Outbox against in-memory ports (`FakePorts.h`, including a real SHA-256).
