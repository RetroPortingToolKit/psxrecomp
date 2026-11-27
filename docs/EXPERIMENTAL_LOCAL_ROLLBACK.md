# PlayStation: experimental local rollback

This is an integration-preparation draft for the shared
[recomp-net experiment](https://github.com/RetroPortingToolKit/recomp-net/pull/56).
The candidate is `fd72202fdf0582acd2f27bcfbe49b0f7adf49407` (wire 4, module ABI 1.4). Dependency preparation
does not enable or qualify a local-rollback game. The existing episode/delay
path remains the comparison route until the host adapter is ready.

## Host boundary

Preserve active-low digital/analog device semantics, exact right-stick input, safe native continuation and all GPU/SPU/CD/device state in `runtime/src/psx_netplay_rb.c` and `netplay_snap_ring.c`. Shared-driver migration and replacing full RAM copies are separate changes. Dirty-page adoption requires CPU, DMA, GPU readback, HLE and mod writers to participate.

## Shared contract

Use the pinned submodule, not an independently updated sibling checkout.
Keep the full build/content identity and an experimental lobby namespace;
wire incompatibility alone is not a lobby announcement. Agree sync mode and a
common qualified CPU/backend profile once before frame 0. Local corrections
load a snapshot whose entire preceding input prefix is confirmed.

Canonicalize fixed-width inputs before publication and compare every byte.
Report non-consumption only after proving the guest and retained input buffers
did not observe that row. Suppress repeated presentation/audio during replay.
Budget delay/depth from built-host live, replay and rewind costs, not a console
name or a zero-duration coarse-clock sample.

The original retail image remains read-only. Valid guarded runtime image/RAM
and mod writes remain supported and belong in the snapshot/digest contract.
Preserve native saves; disposable save states need no cross-build compatibility.

The existing lobby/signaling/Coturn service can carry this protocol. Client
mesh/TURN integration and deployed-service capacity still need qualification;
the retired gameplay UDP SFU is not a fallback.

Before promotion, qualify real HLE racer/shooter A/B workloads, individual
near-link regressions, complete match reports, cross-OS network matches,
minimum-hardware frame budget, long soaks and tester observations. Separate
Windows/Linux library tests do not establish those game-level results. See the
[RFC](https://github.com/RetroPortingToolKit/recomp-net/blob/experimental/local-rollback/docs/rfc-local-rollback.md).
