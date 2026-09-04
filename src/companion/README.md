# src/companion

Everything the X4 Pro Companion fork adds to CrossPoint lives here (see the monorepo's `docs/ARCHITECTURE.md`).
Upstream files are touched only at logged hook points (`docs/UPSTREAM_TOUCHPOINTS.md`).

- `proto/` — protocol v1 codec: CBOR (`Cbor.h`), frame/segment/bulk headers (`Frame.h`), typed messages (`Messages.h`). Zero heap use; caller-owned buffers.
- Every file is wrapped in `#if CROSSPOINT_COMPANION … #endif`, so the stock envs compile none of it. Build the fork with `pio run -e x4pro-companion`, which defines `CROSSPOINT_COMPANION=1`.

Host tests (gtest, no device needed):

    cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test --output-on-failure -j

The suite `test/companion_proto/` checks every golden vector in `protocol/vectors/v1/` (regenerate `vectors_v1.h` with `gen_vectors_header.py` after the vectors change).
