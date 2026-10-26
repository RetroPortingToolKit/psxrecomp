# HD FMV guest timeline contract

The optional `fmv_hd/` decoder replaces a 24-bit scanout only when the scanout
signature identifies a pack source frame. The presenter receives an immutable
`FmvHdTimeline` captured with that native scanout on the guest thread: guest
cycle, guest frame, execution epoch, CD stream generation and start LBA,
delivered sector LBA, and MDEC decode ordinal. FMVHD003 packs bind a movie to
an inclusive SetLoc range and provide one starting sector LBA per source
frame. The current delivered sector interval selects that source frame before
its signature is checked. This disambiguates identical source images across
movies and within one movie. An unmatched scanout or failed decode uses the
native frame. FMVHD002 packs lack stream/frame identity and are rejected by
default; `PSX_FMV_HD_LEGACY=1` enables the old signature-only behavior.

Within one recognized source frame, interpolated HD subframes advance from
guest cycles at 33,868,800 cycles/second. Repeated presents while the guest is
paused keep the same subframe. A new recognized source frame starts at
subframe zero. Savestate and rollback loads increment the execution epoch;
backward guest cycles or frame numbers and a new CD SetLoc/seek generation also
reset recognition. A new MDEC decode ordinal resets subframe phase even if the
source image is identical. A non-movie
scanout ends recognition, including a skip or movie end. Restart/context
recreation releases the decoder and all tracking.

The synthetic native test covers malformed packs, decode/worker failures,
restart, paused host time, guest-cycle advancement, a load epoch, duplicate
source images distinguished by sector LBA, seek generation, movie exit,
state-load and rollback epochs, and unmatched scanout fallback. Full runtime
builds and the focused test passed
on Windows and macOS with FMVHD003.

A short R4 local-disc replay used a local 80-frame synthetic FMVHD003 pack,
with no retail assets included here. It selected HD replacements at SetLoc
192822 and delivered LBAs 194127–194324 with zero dispatch misses. The first
replacement was at guest frame 1475, cycle 832608000, delivered LBA 194129,
and MDEC ordinal 260. CD audio input and host audio output counters were
active. A 0.5-second host pause held guest cycle, replacement count, and audio
counters; save/load incremented execution epoch from 1 to 2 and playback
resumed. These counters show active audio and a frozen pause timeline, but do
not measure exact audio/video lip-sync. During one unpaused movie interval,
guest time advanced 366.67 ms; SPU output advanced 16,170 samples at 44.1 kHz
(366.67 ms), CD input 16,464 samples (373.33 ms), and host output 15,876
samples (360.00 ms). This bounds counter-level cadence within 6.67 ms; it
does not compare sound content to image content.

In a second short replay, TCP Start skipped the movie. By guest frame 1494,
HD source tracking reset to -1, and the HD frame and lookup counts stopped
at 17 and 530 through frame 1514. CD input stopped while already queued host
audio drained; dispatch misses were zero. This exposed and fixed a 15-bit
present-path reset omission. R4's intro has no movie seek control; local
Rewind was off in this test configuration, and netplay rollback was inactive.

The title replay covers pause, skip, and state load. The focused test covers
seek generation and the rollback epoch transition. Exact content-level A/V
alignment remains unmeasured because the local replacement images are
synthetic. R4's current public candidate has no `fmv_hd/` pack and does not
pin this optional feature. A pack author must
record the source-frame sector table from the title's own stream, including
any prefetch behavior; if the table is wrong, the native scanout is the safe
fallback.
