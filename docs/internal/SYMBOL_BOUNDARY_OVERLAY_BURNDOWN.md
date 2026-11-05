# Symbol boundary and overlay follow-up

Owner issue: `beads-eio.3.441`. Baseline: v0.3.6-alpha,
`d9605697e25ee244fd9ea3d200c8ceb92ca1d08d`.

The previous pass scored PS-X EXE/CPE entries. This pass evaluates raw loaded
images, overlay identity and function extents using the retained private corpus.
No individual game port, runtime address exception or new interpreter fallback
is part of this work. Original binaries and symbols stay outside the repository.

| Candidate | Acceptance | Current disposition |
|---|---|---|
| Three raw linked images with SYM metadata | Establish load mapping independently of discovery; compare resident starts/ends; implement or reject concrete shared defects | Mapped from linker origins and frame/end instruction agreement; all 4,729 resident named starts already present in v0.3.6 |
| Eleven raw modules with MAP metadata | Account for each module's stored bytes, code/data sections and overlapping load region; evaluate code-bearing modules | Eight code-bearing modules analyzed byte-for-byte; three groups have only mapped data/rdata and no established executable evidence |
| Function extent and false-code candidates | Check debug ends and explicit data evidence against generated control flow; resolve demonstrated defects with regressions | Seven false entries removed: five string fragments and two numeric-data fragments. Shared heuristic fix preserves direct calls/explicit entries. Blanket debug-end merging rejected: native partitions retain branch/fallthrough continuations |
| ABI/control-flow candidates | Evaluate frame/register/return metadata and actual instructions; resolve demonstrated translation/partition errors | Reachable original-argument reads include forward joins and load delays; dead reads after jumps excluded, outward fragments uncertain. All 3,316 nonzero resident reference frames now match, correcting 381 mismatches, including seven branch-delay allocations. Seventeen focused cases pass |
| Remaining metadata gaps | Bound work on the two unsupported symbol files and additional trustworthy pairings; reject absent/wrong-revision references explicitly | Observed 9c function variant supplies 187 independently validated references, all already discovered. Of 167 overlay groups, one has matched retained loaded bytes; 166 remain unqualified |

Count outcomes by validated framework fixes and evidence-backed rejections.
Debug ends and section labels require instruction-level corroboration; neither
automatically defines an exclusive native body or proves that every byte is code.
Overlay symbols require matching bytes and preserve image/overlay identity when
addresses overlap. A missing reference is not a discovery failure.

Reuse the existing analyzer, emitter, tests and supported consumer regeneration.
Any emission change needs meaningful synthetic regression coverage and built
consumer qualification against the frozen baseline. Keep the previously observed
13 baseline test failures distinct from introduced failures. The normal owner
merge approval remains required; deliver the next framework release after
accepted changes are complete and validated. Frozen regeneration has identical
native code for all three previously cleared consumers (125 C/header/range
files). Tomba differs only in annotation comments in eight files; no game is
relaunched for a duplicate visual check. Full CTest is in progress.

The three linked images are `DIABPSX.BIN` at `80010000` and `BECODE_Z.BIN` /
`FECODE_Z.BIN` at `8005952c`. Loaded-image envelopes preserve all original bytes.
Their synthetic analysis root is not a claimed loader entry and is excluded from
discovery gains. Module VMAs overlap: untyped MAP labels cannot be pooled across
modules or used as a function recall/precision denominator. The `.OVL` files in
this collection are symbol streams, not raw executable overlays.

The false entries are addressed format strings immediately before a real
scheduled prologue. Their words resemble ALU instructions but have nonzero unused
shift fields. Only weak compiler-entry heuristics reject those encodings; direct
calls/explicit entries and hardware decoding remain permissive. The qualified
resident typed-data check found no exact-start collisions, so it does not justify
a broader code/data rule. Low-confidence module header/data fragments likewise
provide no basis for a global section-name ban: assembly can live in data sections.

Debug function ends describe source functions, while native partitions can split
one source body. For example, a conditional branch can target the next native
piece, or straight-line flow can continue into it. Existing emitter continuation
handling preserves those paths. Shorter reported native ranges alone do not prove
missing instructions or justify merging unrelated entries. The ABI report now
marks outward fragments uncertain; it remains a local-body heuristic, with opaque
calls and no interprocedural prototype guarantee.

The private 9c reader checks record boundaries and preserves its eight additional
bytes as opaque metadata. Frame sizes and all 187 end-return instructions agree
with the two matching binaries; the extra fields' meaning is not claimed. Other
overlay groups have absent or mismatching loaded bytes and are not discovery
failures. Original symbols, names and code remain private. Known MND layouts:
[psx-spx](https://psx-spx.consoledev.net/dev/ps1/cdr/cdromfileformats/executables/).
Load-delay behavior:
[R3000 specification notes](https://psx-spx.consoledev.net/cpuspecifications/).
