# Shared resident preparation ownership

This package changes the complete shared cold preparation job. In the matched
Tomba USA production builds, cold startup peak process commit falls from about
831.3 to 725.6 MiB in both opposite-order pairs (105.7 MiB lower); commit at the
observed ready boundary falls by about 43.4 MiB. One comparable-load pair reduces
whole preparation from 590.9 to 351.4 ms (40.5%). The other pair also improves,
but its host load differs substantially and it is supporting evidence only.

These are measurements under a fixed Windows Job budget of 0.5 physical-core
equivalents, identical for both choices and all observer children. They are not
unrestricted-host wall-time or exclusive preparation-CPU claims. Verified-cache
wall and startup CPU results are mixed and overlapping: neutral/inconclusive.
Both verified pairs have unequal host load, so no verified-cache speedup is
claimed. The original 673.7/56.7 ms observation selected the boundary only.

`PSX_NATIVE_RESIDENT_PREPARATION` selects owned blobs in ENHANCED builds.
Explicit OFF and REFERENCE retain the original flat builder. A normal caller
receives the same immutable file/derived bytes, metadata and dedup aliases.
The native builder hashes source bytes once for stock checks and dedup, moves
new source allocations into the pack, owns borrowed derived outputs once, and
publishes the original aligned pack format directly from these owned blobs.
It avoids repeated growth/copying of the complete resident payload. Warm cache
loading still verifies every blob and uses the original flat allocation.

Every file is still read and every derive callback still runs in spec order,
including duplicate source files. Source pointers remain valid while callbacks
emit further blobs. Modified-disc policy, opaque callback behavior/errors,
fingerprint, blob order, table digests, atomic file replacement and memory-only
fallback are preserved. No title loader or codec changes are included. The
portable SHA implementation remains available on every supported platform.

This is host preparation only. Selection keeps simulation identity unchanged;
qualification requires byte-identical entire packs and caller observations.
The real original/native whole-job fixture covers stock/modified inputs,
duplicates, derived outputs, callback/source failures, invalid input, cache
corruption/rebuild and publication failure. The existing real ISO/mod-runtime
fixture also runs the owned implementation.

The retail comparison uses the same source, title, generated code, dependencies
and production configuration with only the resident option OFF/ON. G4/H1 stay
ON; DEBUG, CPU spans, IPO and PGO stay OFF. Eight launches use opposite orders
and fresh private application caches followed by verified-cache reuse; OS
caches are not flushed. Every actual 74,498,804-byte pack has the same SHA-256,
including all 1,062 source and 637 derived records. Passive reads of the exact
production binaries verify actual GL/1x, PGXP OFF, interpolation OFF, rewind
OFF, progress to at least 400 frames and normal service batches; all close cleanly.
No production TCP server or instrumented rebuild is used.

The failed TCP harness attempt is retained without performance credit. The
first successful passive sample required only a stdout/stderr postcheck
correction; its original numbers and lease provenance are retained without
repeating cold preparation. Launch-level results and the unequal-load
classifications are in
[the qualification data](performance/resident-preparation-qualification.json).
Final combined owner adoption remains pending. These bounded startup/output
checks do not establish FPS, GPU, speaker audio, every gameplay path, two-peer
netplay or all-title gains.
