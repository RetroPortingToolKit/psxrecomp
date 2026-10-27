# Trusted title host snapshots

Guest RAM alone cannot resume a trusted title mod that keeps player contexts,
actor ownership or resource descriptors in host memory. Register a
`PSXModSnapshotProvider` from `runtime/include/mod_snapshot.h` once at startup,
using the stable manifest package ID. The runtime copies the callbacks and
includes only providers in the committed enabled simulation plan. Pending UI
choices and netplay local-view packages do not participate. A forced trusted
netplay profile participates under its plugin ID, so initial complete-machine
transfers carry its host state; this does not change user-save/netplay policy.

The provider owns its explicit little-endian wire and nonzero schema version.
`bytes()` measures the current payload; `write()` writes exactly that length
and returns 1. Payload size may vary between captures. `validate()` must accept
the saved length independently of the current live size, check every value and
resource bound that `read()` requires, and return 1 only when `read()` cannot
fail. Do not encode host pointers, struct padding or handles. Guest byte arrays
and integer guest addresses are valid. Treat bytes/write/validate as read-only:
they must not advance gameplay, allocate guest memory or call the guest.

The optional `ready()` callback returns 0 while actor projection, render-only
guest calls or another non-resumable host C stack is active. User save/load and
rewind requests stay pending until every enabled provider is ready. Do not
return 0 merely because the frontend UI is paused: it must be possible to load
from the save/rewind panel. Direct serializer calls refuse while not ready.
The existing scheduler resume-PC checks still apply.

Capture measures, allocates, writes, checks unchanged sizes/active providers,
and validates the complete host payload as one operation. Failed capture
returns NULL/0 and produces no successful snapshot. Load frames, inflates and
validates every machine and provider section before mutating guest or host
state. It then restores the machine and invokes every provider `read()` after
all guest/device sections, even if the input host section came first. Read
callbacks must not allocate or fail. Rebind/reconstruct derived renderer and
audio resources through the existing savestate plugin callback before guest
execution resumes; register resources at activation so restore does not change
the enhancement-memory layout. Capture and restore never update native campaign
save records on a provider's behalf.

Snapshots with active providers use boot-state v10 and `BS_SEC_MOD_HOST` (0x16).
Older readers reject v10 instead of ignoring required host state. With no
providers, writers keep the existing v9 header, section layout and compatibility
cookie. The cookie includes the enabled provider IDs/schema versions, not their
changing payload sizes. A missing, extra, disabled, duplicate or incompatible
provider rejects the whole load. Provider sections are bounded to 16 MiB and 64
providers, in lexicographic package-ID order. Their wire is:

| Field | Encoding |
| --- | --- |
| Header | u32 LE magic `0x504e534d` (`MSNP`), format 1, count, reserved 0 |
| Each record | u32 LE schema, ID length, payload length; ID bytes without NUL; payload |

Diagnostic code may register a loaded observer for a package. A successful
complete restore supplies the exact borrowed full `.pst` input before it is
released. Raw rewind snapshots are also complete images, suitable as a new
diagnostic baseline. Observers may write artifacts, but must not change
simulation or call the guest. The same callback registered under multiple
enabled IDs runs once; failed loads never notify.

Frontend acceptance may depend on the restored RAM. Savestate and rewind
callers capture a rollback image, use `boot_state_load_buffer_checked()` or
`boot_state_load_checked()` with their resume-PC predicate, and quietly restore
rollback after refusal. Observers run only after that predicate accepts. The
ordinary load functions retain direct complete-machine restore semantics for
initial netplay transfers. `boot_state_load_buffer_quiet()` is for internal
rollback; it must not announce a diagnostic timeline change.

Focused tests are `mod_snapshot_test`, `mod_snapshot_boot_state_test`, and the
quiescence cases in `savestate_title_policy_test`. The boot-state test exercises
the real writer/loader with counted device doubles, raw/zlib round trips,
provider-first section ordering, observer deduplication, malformed later
providers and zero-mutation refusals. Existing handoff/RAM-geometry tests also
link the registry to qualify no-provider compatibility.
