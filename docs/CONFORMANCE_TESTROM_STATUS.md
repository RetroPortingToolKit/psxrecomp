# External test-ROM conformance status

Date: 2026-08-21

This lane turns small public PS1 hardware tests into deterministic PSXRecomp
regression fixtures. It boots the exact pinned PS-X EXE from a locally built
disc, captures guest TTY bytes through a bounded debug endpoint, and compares
the assertion log with the upstream hardware-derived result.

## Current result

| Fixture | Assertions | PSXRecomp result | Meaning |
|---|---:|---|---|
| `gte-test-all` | 1,150 | **1,150 passed, 0 failed** | All valid GTE opcodes and tested register semantics match the pinned oracle. |
| `cpu-cop` | 17 | **Blocked after 2 passes** | The strict AOT compiler replaces functions containing intentional invalid COP instructions with `psx_unknown_dispatch` stubs. |

The GTE fixture initially passed zero of its 50 register tests. The oracle then
identified and verified title-neutral corrections for register sign extension,
RES1, FLAG writes, 44-bit MAC overflow, projection overflow flags, MVMVA edge
cases, lighting/color staging, and depth-cue pipeline ordering. The completed
fixture passes from a clean `prepare` -> recompile -> build -> headless run.

The CPU result is not a harness failure. Its first two tests execute and pass;
the next intentionally invalid COP instruction reaches generated function
`0x80010144`, which is a fatal unknown-dispatch stub. A faithful interpreter or
exception-generating fallback for compiler-rejected instructions is required
before this fixture can test the remaining CPU behavior.

## Provenance and boundaries

- Fixture source: `JaCzekanski/ps1-tests`, MIT, release `build-158`, commit
  `5f73d61b27a8d51269b54dde1c456d2d2f7dc865`.
- Exact binary/log/archive hashes are in
  `tools/conformance_testrom/fixtures.json` and each private run's
  `prepare-receipt.json`.
- Fixture binaries and locally owned PlayStation license-sector data are not
  committed.
- The upstream expected logs are the automated oracle. A matched physical-PS1
  recapture remains the promotion gate for a hardware-qualified receipt.

## Next factory steps

1. Add a nonfatal compiler fallback for deliberately invalid instructions and
   finish the 17-test CPU/COP fixture.
2. Have Eagle recapture the hashed synthetic GTE disc on hardware.
3. Add focused CPU, cache, DMA, timer, CD, GPU, SPU, and SIO fixtures one at a
   time; each must retain exact input hashes and a machine-readable result.
4. Run this lane before whole-game validation so architectural regressions are
   found below the game layer.

Reproduction instructions are in `tools/conformance_testrom/README.md`.
