# Shaneomac PR #16: overlay-loader initialization guard

| Field | Value |
|---|---|
| Upstream PR | <https://github.com/mstan/psxrecomp/pull/16> |
| Source commit | [`e6809ccf7d778a4c2f32d9e27c0ec31a44cbd2ba`](https://github.com/mstan/psxrecomp/commit/e6809ccf7d778a4c2f32d9e27c0ec31a44cbd2ba) |
| Source author | Martin Penkava (`shaneomac1337`) `<mpenkava1337@gmail.com>` |
| Source co-author | Claude Fable 5 `<noreply@anthropic.com>` |
| Port base | `dde268dc0fb9daf8fe6529f4aebfe80995350334` |
| Evaluated/ported | 2026-07-13 (America/Los_Angeles) |

## Included

Only the overlay-loader lifecycle guards from the source commit were adapted.
`overlay_loader_dispatch()`, `overlay_loader_is_candidate()`, and
`overlay_loader_call_native()` now return without consulting overlay indexes
until `overlay_loader_init()` finishes constructing them and sets `s_active`.

The focused regression test is
`runtime/tests/test_overlay_init_guard.py`.

## Explicitly excluded

- The source commit's decodable-word/high-RAM dirty-interpreter fallback in
  `runtime/src/cpu/dirty_ram_interp.c`.
- All SmackDown-specific behavior and data.
- Every other PR #16 change, including Vulkan, audio/SPU, GPU primitive-size,
  MSVC portability, and helper-script work.

The source commit combined the lifecycle guard with a separate discovery
fallback. This port intentionally does not cherry-pick that commit; it adapts
only the three fail-closed `s_active` checks so the framework change remains
game-agnostic and reviewable in isolation.
