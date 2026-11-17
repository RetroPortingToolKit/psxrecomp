#!/usr/bin/env python3
"""Overlay shards keyed by segment, end to end (docs/SEGMENT_AWARE_CODE.md §5.7).

Overlay bytes are segment-free; execution is not. A PC's segment is part of
every link, EPC, I-cache tag and store-PC stamp a compiled body bakes, so rollout
PR E keys overlay shards by segment:

  - capture schema v3 records the segments each dispatch entry entered through
    (`dispatch_entry_segments`); a capture without it is KSEG0 only;
  - compile_overlays.py builds one shard per (region, segment with entries),
    compiled at `segment | phys`, named and exported by that VA;
  - KSEG0 shards keep their place and names in the cache tag directory, and
    KUSEG/KSEG1 shards go in its seg-kuseg/ and seg-kseg1/ subdirectories
    under the same {phys}_{crc} grammar (decision 4); their manifests carry
    `S <segment>`;
  - the loader runs a shard only for PCs in its own segment.

This test drives the real pieces:
  1. capture_segment_views() on v2 and v3 records, and its refusals;
  2. the manifest writer and the runtime-contract parser (S record);
  3. the per-segment cache directories and candidate-capacity namespace;
  4. compile_overlays.py's real driver, in-process, on a v3 capture of a small
     overlay entered at KUSEG and KSEG1 (the real psxrecomp-game compiles each
     view; the C compiler builds the shards): the files land in their segment
     directories, every PC each shard bakes is in its segment, a KSEG1 shard
     charges a fetch before every instruction, a second run is served from the
     cache per segment, the KSEG0 view of a v3 capture compiles exactly as the
     same bytes captured as v2, and --static emits per-segment namespaces and
     exact rows, including an isolated fragment per segment of one entry;
     a record with execution evidence only compiles as its v2 reading, and a
     --force-interior PC gets its KSEG0 view where no KSEG0 entry was seen;
     each segment's shard reads only its own segment's prior manifest and
     fragments; config code sites (mod function-entry hooks) reach every
     segment's view whatever segment the config spells them in;
  5. the real overlay loader (runtime/tests/overlay_pair_dedup_harness.c) on
     those shards: each segment's PC runs its own shard through its CPS
     continuations, links and stamps in its segment, and the fetch tags it hands
     the I-cache model (runtime/src/psx_icache.c) cost exactly what Beetle's
     fetch model charges for the instructions it executed; a KSEG0 PC, which
     has no shard, is interpreted.

Usage: python test_overlay_segment_keys.py --recompiler <psxrecomp-game>
           [--compiler cc]
Exit 0 = PASS.
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import importlib.util
import io
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RUNTIME = ROOT / "runtime"
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(RUNTIME / "tests"))

import compile_overlays as co  # noqa: E402
import test_overlay_pair_dedup_runtime as pair  # noqa: E402

KUSEG, KSEG0, KSEG1 = 0x00000000, 0x80000000, 0xA0000000
PHYS = 0x000A0000
GAME_TOML = ROOT / "tools" / "cycle_testrom" / "game.toml"
GAME_ID = "CYCT-00101"


def load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


GEN = load("gen_segment_exe", ROOT / "tools" / "segment_testrom" / "gen_segment_exe.py")
LEDGER = load("segment_ledger", HERE / "test_segment_aware_codegen.py")

FAILURES: list[str] = []


def check(cond: bool, what: str) -> None:
    if not cond:
        FAILURES.append(what)
        print("  FAIL: " + what)


# ---------------------------------------------------------------------------
# The overlay: a framed function that calls a leaf with jal (a link in its
# segment), runs a straight line, stores and reloads $ra (a store-PC stamp in
# its segment) and returns. Its bytes are the same in every segment.
# ---------------------------------------------------------------------------
def overlay_image():
    a = GEN.Asm(KSEG0 | PHYS)
    a.label("ov_run")
    a.emit(GEN.addiu("sp", "sp", -8), GEN.sw("ra", 4, "sp"))
    a.jal("ov_getpc")
    a.emit(GEN.nop())
    a.label("ov_after")                        # the jal's return point
    a.emit(*[GEN.addiu("t3", "t3", 1) for _ in range(4)])
    a.emit(GEN.lw("ra", 4, "sp"), GEN.addiu("sp", "sp", 8),
           GEN.jr("ra"), GEN.nop())
    a.label("ov_getpc")
    a.emit(GEN.jr("ra"), GEN.addu("v1", "ra", "zero"))
    a.label("ov_end")
    body = a.resolve()
    labels = {k: v & co.PHYS_MASK for k, v in a.labels.items()}
    return body + b"\0" * (0x1000 - len(body)), labels


def executed_sequence(labels):
    """Physical PCs in the order one call executes them."""
    run = list(range(labels["ov_run"], labels["ov_after"], 4))
    leaf = list(range(labels["ov_getpc"], labels["ov_end"], 4))
    rest = list(range(labels["ov_after"], labels["ov_getpc"], 4))
    return run + leaf + rest


def capture(data, labels, segments=None, *, v2=False):
    """A runtime-shaped capture record (overlay_capture.c write_json_window)."""
    load_addr = KSEG0 | PHYS
    executed = [f"0x{KSEG0 | pc:08X}" for pc in executed_sequence(labels)]
    cap = {
        "schema": "psxrecomp overlay capture v2" if v2 else
                  "psxrecomp overlay capture v3",
        "load_addr": f"0x{load_addr:08X}",
        "size": len(data),
        "guard_bytes": 0,
        "bytes_b64": base64.b64encode(data).decode(),
        "executed_pcs": sorted(executed),
        "dispatch_entry_pcs": [f"0x{load_addr:08X}"],
        "function_entry_pcs": [],
        "seeds": [f"0x{load_addr:08X}"],
    }
    if not v2:
        cap["dispatch_entry_segments"] = {
            name: [f"0x{seg | PHYS:08X}"] if seg in segments else []
            for name, seg in (("kuseg", KUSEG), ("kseg0", KSEG0), ("kseg1", KSEG1))
        }
    return cap


def raw_capture(data, executed_phys, entries, function_entries=()):
    """A v3 record from explicit evidence: `entries` maps a segment to the
    physical dispatch entries seen there."""
    load_addr = KSEG0 | PHYS
    dispatched = sorted({pc for pcs in entries.values() for pc in pcs})
    return {
        "schema": "psxrecomp overlay capture v3",
        "load_addr": f"0x{load_addr:08X}",
        "size": len(data),
        "guard_bytes": 0,
        "bytes_b64": base64.b64encode(data).decode(),
        "executed_pcs": [f"0x{KSEG0 | pc:08X}" for pc in sorted(executed_phys)],
        "dispatch_entry_pcs": [f"0x{KSEG0 | pc:08X}" for pc in dispatched],
        "dispatch_entry_segments": {
            name: [f"0x{seg | pc:08X}" for pc in sorted(entries.get(seg, ()))]
            for name, seg in (("kuseg", KUSEG), ("kseg0", KSEG0), ("kseg1", KSEG1))},
        "function_entry_pcs": [f"0x{KSEG0 | pc:08X}" for pc in function_entries],
        "seeds": [f"0x{KSEG0 | pc:08X}" for pc in dispatched],
    }


def fragment_image():
    """A root function, then an entry with no callable boundary (the probe's
    position-independent ov_run shape): it compiles only as an isolated
    fragment."""
    a = GEN.Asm(KSEG0 | PHYS)
    a.label("root")
    a.emit(GEN.jr("ra"), GEN.nop())
    a.label("pi")
    a.emit(GEN.addu("t9", "ra", "zero"))
    a.bgezal("zero", "pi_leaf")
    a.emit(GEN.nop())
    a.emit(*[GEN.addiu("t3", "t3", 1) for _ in range(4)])
    a.emit(GEN.jr("t9"), GEN.addu("v0", "t9", "zero"))
    a.label("pi_leaf")
    a.emit(GEN.jr("ra"), GEN.addu("v1", "ra", "zero"))
    a.label("end")
    body = a.resolve()
    labels = {k: v & co.PHYS_MASK for k, v in a.labels.items()}
    return body + b"\0" * (0x1000 - len(body)), labels


# ---------------------------------------------------------------------------
# 1. capture views
# ---------------------------------------------------------------------------
def check_views(data, labels):
    print("capture views")
    v2 = capture(data, labels, v2=True)
    check(co.capture_segment_views(v2) == [v2] and co.capture_segment_views(v2)[0] is v2,
          "a v2 capture is returned unchanged")
    v3 = capture(data, labels, {KUSEG, KSEG1})
    v3["static_dispatch_entry_pcs"] = [f"0x{KSEG0 | PHYS:08X}"]
    v3["producer_ranges"] = [{"start": f"0x{KSEG0 | PHYS:08X}",
                              "end": f"0x{KSEG0 | PHYS + 0x40:08X}"}]
    views = co.capture_segment_views(v3)
    check([v["load_addr"] for v in views] == [f"0x{KUSEG | PHYS:08X}",
                                              f"0x{KSEG1 | PHYS:08X}"],
          "one view per segment with entries, spelled in it")
    for v in views:
        seg = int(v["load_addr"], 16) & co.SEGMENT_MASK
        check("dispatch_entry_segments" not in v, "views carry no segment record")
        check(v["dispatch_entry_pcs"] == [f"0x{seg | PHYS:08X}"],
              f"{seg:08X}: dispatch entries are the segment's own")
        check(all(int(pc, 16) & co.SEGMENT_MASK == seg for pc in v["executed_pcs"]) and
              len(v["executed_pcs"]) == len(v3["executed_pcs"]),
              f"{seg:08X}: execution evidence carried whole, respelled")
        check(v["static_dispatch_entry_pcs"] == v["dispatch_entry_pcs"],
              f"{seg:08X}: static dispatch entries stay a subset")
        check(v["producer_ranges"][0]["start"] == f"0x{seg | PHYS:08X}",
              f"{seg:08X}: producer ranges respelled")
        check(v["bytes_b64"] == v3["bytes_b64"] and v["size"] == v3["size"],
              f"{seg:08X}: the bytes are shared")
    # An entry that no segment list names is KSEG0, as in v2.
    mixed = capture(data, labels, {KUSEG})
    mixed["dispatch_entry_pcs"].append(f"0x{KSEG0 | PHYS + 0x30:08X}")
    views = co.capture_segment_views(mixed)
    check([v["dispatch_entry_pcs"] for v in views] ==
          [[f"0x{KSEG0 | PHYS + 0x30:08X}"], [f"0x{KUSEG | PHYS:08X}"]],
          "an unlabelled dispatch entry is a KSEG0 entry")
    # Execution evidence only (the runtime writes such regions): one KSEG0
    # view, read as a v2 reader reads the record.
    bare = capture(data, labels, set())
    bare["dispatch_entry_pcs"] = bare["seeds"] = []
    views = co.capture_segment_views(bare)
    as_v2 = {k: v for k, v in bare.items() if k != "dispatch_entry_segments"}
    check(len(views) == 1 and views[0] == as_v2,
          "a record with no dispatch entry is one KSEG0 view, as in v2")
    # A demand from outside the capture names a segment it has no entry in.
    views = co.capture_segment_views(capture(data, labels, {KUSEG}), {KSEG0})
    check([(v["load_addr"], v["dispatch_entry_pcs"]) for v in views] ==
          [(f"0x{KSEG0 | PHYS:08X}", []),
           (f"0x{KUSEG | PHYS:08X}", [f"0x{KUSEG | PHYS:08X}"])],
          "a required segment gets a view without entries of its own")
    inside = KSEG0 | PHYS + 0x10
    declared = co.declared_view_segments
    check(declared(v3, {}, {inside}) == {KSEG0} and
          declared(v3, {}, {KSEG0 | PHYS + len(data)}) == set(),
          "a --force-interior PC inside the record asks for its KSEG0 view")
    check(declared(v3, {"overlays": [{"load_addr": f"0x{KUSEG | PHYS:08X}",
                                      "entries": [f"0x{KUSEG | PHYS:08X}"]}]}) == {KUSEG} and
          declared(v3, {"overlays": [{"load_addr": f"0x{KUSEG | PHYS:08X}"}]}) == set() and
          declared(v3, {"overlays": [{"entry": f"0x{KSEG1 | PHYS + 8:08X}"}]}) == {KSEG1} and
          declared(v3, {"overlays": [{"entry": f"0x{KSEG1 | PHYS + 0x2000:08X}"}]}) == set() and
          declared(v3, {"overlays": [{"entry": f"0x{0xC0000000 | PHYS:08X}"}]}) == set(),
          "a game.toml [[overlays]] table names the segment it spells, inside the record")
    for bad, why in ((dict(v3, dispatch_entry_segments={"kseg2": []}), "unknown name"),
                     (dict(v3, dispatch_entry_segments={"kuseg": [f"0x{KSEG1 | PHYS:08X}"]}),
                      "PC in another segment"),
                     (dict(v3, dispatch_entry_segments=["0x000A0000"]), "not an object")):
        try:
            co.capture_segment_views(bad)
            check(False, f"malformed segment record accepted ({why})")
        except RuntimeError:
            pass


# ---------------------------------------------------------------------------
# 2. manifests
# ---------------------------------------------------------------------------
def check_manifests():
    print("manifests")
    ranges = [(KSEG0 | PHYS, 0x30)]
    k0 = co.overlay_ranges_text([(KSEG0 | PHYS, 0x1234, ranges)], 0xABC)
    check(k0 == ("# psxrecomp overlay code-range manifest v2 (entry+code_crc)\n"
                 "P 0000000000000ABC\nF 800A0000 00001234\nR 800A0000 30\n"),
          "a KSEG0 manifest is the pre-§5.7 text")
    ku = co.overlay_ranges_text([(KUSEG | PHYS, 0x1234, ranges)], 0xABC)
    check(ku.splitlines()[1:4] == ["S 00000000", "P 0000000000000ABC",
                                   "F 000A0000 00001234"] and
          "R 800A0000 30" in ku,
          "a KUSEG manifest: S first, full-VA F, KSEG0-spelled R")
    try:
        co.overlay_ranges_text([(KUSEG | PHYS, 1, ranges), (KSEG1 | PHYS + 8, 1, ranges)])
        check(False, "a manifest with two segments was written")
    except ValueError:
        pass
    pid, funcs = co.parse_runtime_shard_manifest(ku, expected_segment=KUSEG)
    check(pid == 0xABC and funcs and funcs[0][0] == KUSEG | PHYS,
          "the parser keeps a KUSEG entry's full VA")
    pid, funcs = co.parse_runtime_shard_manifest(k0, expected_segment=KSEG0)
    check(funcs and funcs[0][0] == KSEG0 | PHYS, "a manifest without S is KSEG0")
    for text, seg, why in (
            (ku, KSEG0, "KUSEG manifest in the KSEG0 directory"),
            (k0, KUSEG, "KSEG0 manifest in seg-kuseg/"),
            (ku.replace("F 000A0000", "F 800A0000"), KUSEG, "F outside S"),
            (k0 + "S 80000000\n", None, "S after F"),
            ("S 00000000\n" + ku, None, "S twice"),
            (ku.replace("S 00000000", "S C0000000"), None, "S in KSEG2"),
            (ku.replace("S 00000000", "S 00010000"), None, "S not a segment base")):
        check(co.parse_runtime_shard_manifest(text, expected_segment=seg) == (None, []),
              f"parser refuses {why}")


# ---------------------------------------------------------------------------
# 3. cache layout
# ---------------------------------------------------------------------------
def check_layout():
    print("cache layout")
    leaf = os.path.join("C", "GAME", "gcc", "macos-arm64", "cg13_00000000_gc00000000_f0")
    check(co.segment_cache_dir(leaf, KSEG0) == leaf and
          co.segment_cache_dir(leaf, KUSEG) == os.path.join(leaf, "seg-kuseg") and
          co.segment_cache_dir(leaf, KSEG1) == os.path.join(leaf, "seg-kseg1"),
          "KSEG0 keeps the leaf; KUSEG/KSEG1 use seg-kuseg/ and seg-kseg1/")
    check([co.cache_dir_segment(co.segment_cache_dir(leaf, s))
           for s in (KUSEG, KSEG0, KSEG1)] == [KUSEG, KSEG0, KSEG1],
          "a directory names its segment")
    lock, dirs = co._candidate_capacity_namespace(os.path.join(leaf, "00010000_00000001.so"))
    lock_k, dirs_k = co._candidate_capacity_namespace(
        os.path.join(leaf, "seg-kseg1", "00010000_00000001.so"))
    check((lock, dirs) == (lock_k, dirs_k) and
          os.path.abspath(os.path.join(leaf, "seg-kuseg")) in dirs and
          os.path.abspath(os.path.join(leaf, "seg-kseg1")) in dirs,
          "every segment directory shares one candidate namespace")


# ---------------------------------------------------------------------------
# 4. compile_overlays.py, in-process
# ---------------------------------------------------------------------------
def run_compile(recompiler, compiler, captures, out_dir, extra=(), toml=GAME_TOML):
    """compile_overlays.main() on `captures`. The cache-tag stamp comes from a
    runtime build this test does not need: the recompiler/stamp match and the
    config hash are not under test here, so they are pinned."""
    cap_path = os.path.join(out_dir, "captures.json")
    with open(cap_path, "w", encoding="utf-8") as f:
        json.dump(captures, f)
    argv = ["compile_overlays.py", "--captures", cap_path,
            "--game-toml", str(toml), "--recompiler", recompiler,
            "--runtime-include", str(RUNTIME / "include"),
            "--out-dir", out_dir, "--gcc", compiler, "--jobs", "1", *extra]
    env = {k: v for k, v in os.environ.items()
           if k not in ("PSX_OVERLAY_CACHE_DIR", "PSX_OVERLAY_CAPTURES",
                        "PSX_OVERLAY_FLAVOR")}
    log = io.StringIO()
    code = 0
    with patch.object(co, "verify_recompiler_matches_tag", lambda *a: None), \
            patch.object(co, "overlay_config_hash", lambda *a: 0), \
            patch.object(sys, "argv", argv), patch.dict(os.environ, env, clear=True), \
            contextlib.redirect_stdout(log):
        try:
            co.main()
        except SystemExit as exc:
            code = exc.code or 0
    return code, log.getvalue()


def leaf_dir(cache_root, recompiler):
    with patch.object(co, "overlay_config_hash", lambda *a: 0):
        tag = co.cache_tag(str(RUNTIME / "include"), recompiler, str(GAME_TOML), 0)
    return os.path.join(cache_root, GAME_ID, "gcc", co.cache_arch_abi(), tag)


def manifest_entries(directory):
    """Every F entry of every manifest in `directory`."""
    out = set()
    for name in os.listdir(directory):
        if name.endswith(".ranges"):
            with open(os.path.join(directory, name), encoding="utf-8") as f:
                out |= {int(ln.split()[1], 16) for ln in f if ln.startswith("F ")}
    return out


def functions(c_text):
    return {m.group(1): m.group(2) for m in re.finditer(
        r"^void (func_[0-9A-F]{8})\(CPUState\* cpu\)\n\{(.*?)^\}", c_text, re.M | re.S)}


def check_compile(recompiler, compiler, data, labels, tmp):
    print("compile_overlays.py")
    crc = __import__("binascii").crc32(data) & 0xFFFFFFFF
    stem = f"{PHYS:08X}_{crc:08X}"
    cache = os.path.join(tmp, "cache")
    os.makedirs(cache)
    code, log = run_compile(recompiler, compiler,
                            [capture(data, labels, {KUSEG, KSEG1})], cache)
    check(code == 0 and "built OK : 2" in log, "both views build")
    leaf = leaf_dir(cache, recompiler)
    ext = co.overlay_ext()
    check(not os.path.exists(os.path.join(leaf, stem + ext)),
          "no KSEG0 shard: no entry was dispatched at KSEG0")
    shards = {}
    for seg, sub in ((KUSEG, "seg-kuseg"), (KSEG1, "seg-kseg1")):
        d = os.path.join(leaf, sub)
        check(os.path.isfile(os.path.join(d, stem + ext)), f"{sub}/{stem}{ext} exists")
        with open(os.path.join(d, stem + ".ranges"), encoding="utf-8") as f:
            manifest = f.read()
        check(f"S {seg:08X}\n" in manifest and
              f"F {seg | labels['ov_run']:08X} " in manifest and
              f"F {seg | labels['ov_getpc']:08X} " in manifest,
              f"{sub}: manifest names its segment and full-VA entries")
        with open(os.path.join(d, f"{crc:08X}_patched.c"), encoding="latin-1") as f:
            c_text = f.read()
        bodies = functions(c_text)
        check(set(bodies) == {f"func_{seg | labels['ov_run']:08X}",
                              f"func_{seg | labels['ov_getpc']:08X}"},
              f"{sub}: functions are named by their VA in the segment")
        foreign = sorted({pc for body in bodies.values()
                          for gid, pattern in LEDGER.PC_SITES.items()
                          for pc in LEDGER.pc_constants(body, pattern)
                          if pc & co.SEGMENT_MASK != seg})
        check(not foreign, f"{sub}: every baked PC is in the segment"
              + (f" (e.g. 0x{foreign[0]:08X})" if foreign else ""))
        stamps = {pc for body in bodies.values()
                  for pc in LEDGER.pc_constants(body, LEDGER.PC_SITES["store-pc-segment"])}
        check(stamps == {seg | (labels["ov_run"] + 4)},
              f"{sub}: the store-PC stamp is the store's PC in the segment")
        shards[seg] = bodies
    # KSEG1: a fetch before every instruction; KUSEG: line leaders only.
    k1 = [pc for body in shards[KSEG1].values()
          for pc in LEDGER.pc_constants(body, LEDGER.PC_SITES["fetch-tag-segment"])]
    check(sorted(k1) == sorted(KSEG1 | pc for pc in executed_sequence(labels)),
          "KSEG1 shard charges a fetch before every instruction")
    ku = [pc for body in shards[KUSEG].values()
          for pc in LEDGER.pc_constants(body, LEDGER.PC_SITES["fetch-tag-segment"])]
    check(len(ku) < len(k1), "KUSEG shard keeps the cached line-leader rule")

    code, log = run_compile(recompiler, compiler,
                            [capture(data, labels, {KUSEG, KSEG1})], cache)
    check(code == 0 and log.count("SKIP: runtime-valid current-byte cache serves") == 2,
          "a second run is served from each segment's cache")

    # The KSEG0 view of a v3 capture compiles exactly as the same bytes as v2.
    outs = {}
    for name, cap in (("v2", capture(data, labels, v2=True)),
                      ("v3", capture(data, labels, {KSEG0}))):
        out = os.path.join(tmp, "cache-" + name)
        os.makedirs(out)
        code, _log = run_compile(recompiler, compiler, [cap], out)
        d = leaf_dir(out, recompiler)
        with open(os.path.join(d, stem + ".ranges"), encoding="utf-8") as f:
            manifest = f.read()
        with open(os.path.join(d, f"{crc:08X}_patched.c"), encoding="latin-1") as f:
            outs[name] = (code, manifest, f.read())
    check(outs["v2"] == outs["v3"] and outs["v2"][0] == 0 and
          "\nS " not in outs["v2"][1],
          "a KSEG0 view compiles byte-identically to the v2 capture")

    # --static: per-segment namespaces and exact rows beside KSEG0 ones.
    static_dir = os.path.join(tmp, "static")
    os.makedirs(static_dir)
    code, log = run_compile(recompiler, compiler,
                            [capture(data, labels, {KUSEG, KSEG0, KSEG1})],
                            static_dir, ("--static", "--static-single-file"))
    with open(os.path.join(static_dir, "overlays_static.c"), encoding="latin-1") as f:
        static_c = f.read()
    rows = {int(a, 16) for a in re.findall(
        r"^    \{ 0x([0-9A-F]{8})u, \d+u, \d+u \},$", static_c, re.M)}
    check(code == 0 and {seg | labels["ov_run"] for seg in (KUSEG, KSEG0, KSEG1)} <= rows,
          "static dispatch has an exact row per segment")
    check(f"ov_s0_{PHYS:08X}_" in static_c and f"ov_s5_{PHYS:08X}_" in static_c and
          f"ov_{PHYS:08X}_" in static_c,
          "static namespaces: ov_s0_ (KUSEG), ov_s5_ (KSEG1), ov_ (KSEG0)")
    check("(addr & 0xE0000000u) != 0x00000000u" in static_c and
          "(addr & 0xE0000000u) != 0xA0000000u" in static_c,
          "the static segment gate admits every compiled segment")
    return cache


def check_demands(recompiler, compiler, data, labels, tmp):
    """Records and demands a view split must not drop (§5.7)."""
    print("compile_overlays.py: demands without a dispatch entry")
    run, after = KSEG0 | labels["ov_run"], KSEG0 | labels["ov_after"]
    # Execution evidence only, plus an operator's --force-interior: the same
    # fragment as the same bytes captured as v2.
    outs = {}
    for name, v2 in (("v2", True), ("v3", False)):
        cap = capture(data, labels, set(), v2=v2)
        cap["dispatch_entry_pcs"] = cap["seeds"] = []
        out = os.path.join(tmp, "bare-" + name)
        os.makedirs(out)
        code, log = run_compile(recompiler, compiler, [cap], out,
                                ("--force-interior", f"0x{after:08X}"))
        d = leaf_dir(out, recompiler)
        outs[name] = (code, "built OK : 1" in log, sorted(os.listdir(d)),
                      {n: open(os.path.join(d, n), "rb").read()
                       for n in os.listdir(d)
                       if n.endswith((".ranges", "_patched.c"))})
    check(outs["v2"][0] == 0 and outs["v2"][1] and outs["v2"][2] and
          outs["v3"] == outs["v2"],
          "a dispatch-less v3 record compiles byte-identically to its v2 reading")
    # A KUSEG-only record: --force-interior (a KSEG0 PC) still builds its
    # KSEG0 fragment next to the KUSEG shard.
    out = os.path.join(tmp, "forced-kuseg")
    os.makedirs(out)
    code, log = run_compile(recompiler, compiler, [capture(data, labels, {KUSEG})], out,
                            ("--force-interior", f"0x{run:08X}"))
    d = leaf_dir(out, recompiler)
    check(code == 0 and run in manifest_entries(d) and
          KUSEG | labels["ov_run"] in manifest_entries(os.path.join(d, "seg-kuseg")),
          "a forced KSEG0 interior of a KUSEG-only record is compiled at KSEG0")


def check_static_fragments(recompiler, compiler, tmp):
    """--static: one entry with no callable boundary, dispatched in two
    segments, gets an isolated fragment in each (the image a demand compiles
    against is its own segment's view)."""
    print("compile_overlays.py --static: isolated fragments per segment")
    data, labels = fragment_image()
    crc = __import__("binascii").crc32(data) & 0xFFFFFFFF
    pi = labels["pi"]
    executed = set(range(labels["root"], labels["end"], 4))
    cap = raw_capture(data, executed, {KSEG0: {labels["root"]}, KUSEG: {pi}, KSEG1: {pi}},
                      function_entries=(labels["root"],))
    out = os.path.join(tmp, "static-frag")
    os.makedirs(out)
    code, log = run_compile(recompiler, compiler, [cap], out,
                            ("--static", "--static-single-file"))
    with open(os.path.join(out, "overlays_static.c"), encoding="latin-1") as f:
        static_c = f.read()
    rows = {int(a, 16) for a in re.findall(
        r"^    \{ 0x([0-9A-F]{8})u, \d+u, \d+u \},$", static_c, re.M)}
    check(code == 0 and "SHARD FAIL" not in log, "every isolated fragment builds")
    for seg in (KUSEG, KSEG1):
        check(seg | pi in rows and
              f"ov_frag_{PHYS:08X}_{crc:08X}_{seg | pi:08X}" in static_c,
              f"{seg:08X}: the entry has its own isolated fragment and row")
    check(KSEG0 | labels["root"] in rows, "the KSEG0 root has its row")


def check_segment_isolation(recompiler, compiler, data, labels, tmp):
    """Each segment's shard reads only its own segment's cache: a KSEG0
    manifest and KSEG0 fragments of the same region never feed or serve a
    KUSEG/KSEG1 view."""
    print("compile_overlays.py: per-segment prior manifests and fragments")
    crc = __import__("binascii").crc32(data) & 0xFFFFFFFF
    stem = f"{PHYS:08X}_{crc:08X}"
    out = os.path.join(tmp, "prior")
    os.makedirs(out)
    leaf = leaf_dir(out, recompiler)
    os.makedirs(leaf)
    # A KSEG0 manifest of these bytes that names an entry no KUSEG or KSEG1
    # capture saw. The prior-manifest reclassification merges only a view's
    # own segment's manifest.
    extra = KSEG0 | labels["ov_after"]
    with open(os.path.join(leaf, stem + ".ranges"), "w", encoding="utf-8") as f:
        f.write(co.overlay_ranges_text(
            [(KSEG0 | labels["ov_run"], 1, [(KSEG0 | labels["ov_run"], 4)]),
             (extra, 1, [(extra, 4)])]))
    code, log = run_compile(recompiler, compiler,
                            [capture(data, labels, {KUSEG, KSEG1})], out)
    check(code == 0 and "built OK : 2" in log and "prior-manifest" not in log,
          "a KUSEG/KSEG1 view merges no KSEG0 manifest")
    for seg, sub in ((KUSEG, "seg-kuseg"), (KSEG1, "seg-kseg1")):
        check(manifest_entries(os.path.join(leaf, sub)) ==
              {seg | labels["ov_run"], seg | labels["ov_getpc"]},
              f"{sub}: roots are the segment's own entries")
    # Empty-primary reconciliation counts only the view's own segment's
    # fragments as serving its roots.
    k0_leaf = leaf_dir(os.path.join(tmp, "cache-v2"), recompiler)     # a KSEG0 shard only
    both_leaf = leaf_dir(os.path.join(tmp, "cache"), recompiler)      # KUSEG and KSEG1 shards
    abi = co.overlay_abi_tag(str(RUNTIME / "include"), 0)
    for seg, cache, served in ((KUSEG, k0_leaf, False), (KSEG0, k0_leaf, True),
                               (KUSEG, both_leaf, True), (KSEG1, both_leaf, True)):
        stats = co.ShardStats()
        pending = [(f"view {seg:08X}", PHYS, seg | PHYS, len(data), data,
                    {seg | labels["ov_run"]})]
        with contextlib.redirect_stdout(io.StringIO()):
            co.reconcile_empty_primary_scans(pending, cache, abi, stats)
        check((stats.skipped, stats.total_fail()) == ((1, 0) if served else (0, 1)),
              f"{seg:08X} roots {'are' if served else 'are not'} served by "
              f"{os.path.basename(os.path.dirname(cache + os.sep))}'s own-segment shards")


def check_config_sites(recompiler, compiler, data, labels, tmp):
    """A mod function-entry hook reaches every segment's shard of the bytes it
    names, however the config spells it (the runtime keys hooks by physical
    address, and the interpreter fires them in every segment)."""
    print("compile_overlays.py: config code sites in every view")
    proj = os.path.join(tmp, "hookproj")
    os.makedirs(proj)
    toml = os.path.join(proj, "game.toml")
    with open(GAME_TOML, encoding="utf-8") as f:
        text = f.read()
    text = text.replace("[recompiler]\n", "[recompiler]\nmod_function_entry_funcs = "
                        f'["0x{KSEG0 | labels["ov_run"]:08X}", '
                        f'"0x{KSEG1 | labels["ov_getpc"]:08X}"]\n', 1)
    with open(toml, "w", encoding="utf-8") as f:
        f.write(text)
    out = os.path.join(tmp, "hooks")
    os.makedirs(out)
    code, _log = run_compile(recompiler, compiler,
                             [capture(data, labels, {KUSEG, KSEG0, KSEG1})], out,
                             toml=toml)
    crc = __import__("binascii").crc32(data) & 0xFFFFFFFF
    leaf = leaf_dir(out, recompiler)
    check(code == 0, "the three views build with the hook config")
    for seg, sub in ((KUSEG, "seg-kuseg"), (KSEG0, ""), (KSEG1, "seg-kseg1")):
        with open(os.path.join(leaf, sub, f"{crc:08X}_patched.c"), encoding="latin-1") as f:
            hooks = {int(a, 16) for a in re.findall(
                r"psx_mod_function_entry\(cpu, 0x([0-9A-F]{8})u\)", f.read())}
        check(hooks == {seg | labels["ov_run"], seg | labels["ov_getpc"]},
              f"{seg:08X}: both hooks fire at the segment's entries "
              f"(got {sorted(f'{h:08X}' for h in hooks)})")


# ---------------------------------------------------------------------------
# 5. the loader
# ---------------------------------------------------------------------------
def check_loader(recompiler, compiler, data, labels, cache, tmp):
    print("overlay loader")
    harness = pathlib.Path(tmp) / ("harness" + (".exe" if os.name == "nt" else ""))
    pair.compile_harness(compiler, harness, codegen_hash=co.codegen_hash(str(RUNTIME / "include"), recompiler))
    # The harness boots the loader for game "PAIR-TEST".
    root = pathlib.Path(tmp) / "hcache"
    shutil.copytree(os.path.join(cache, GAME_ID), root / pair.GAME)
    image = pathlib.Path(tmp) / "image.bin"
    image.write_bytes(data)
    out = subprocess.run([str(harness), str(root), "segment-run", str(image),
                          f"{PHYS:X}"], capture_output=True, text=True)
    check(out.returncode == 0, "harness runs: " + out.stderr.strip())
    runs = {}
    for line in out.stdout.splitlines():
        if line.startswith("seg="):
            fields = dict(item.split("=", 1) for item in line.split())
            runs[int(fields["seg"], 16)] = fields
        elif line.startswith("segment_native="):
            totals = dict(item.split("=", 1) for item in line.split())
    icache = LEDGER.ICacheModel(compiler, tmp)
    after = labels["ov_after"]
    for seg in (KUSEG, KSEG1):
        r = runs.get(seg, {})
        check(r.get("interp_at") == "00000000" and r.get("end") == "0000BEE0",
              f"{seg:08X}: every transfer ran natively, back to the caller")
        check(r.get("v1") == f"{seg | after:08X}",
              f"{seg:08X}: the jal link is in the segment (v1={r.get('v1')})")
        check(r.get("t3") == "4" and r.get("sp") == "001FF000",
              f"{seg:08X}: the body ran whole")
        check(r.get("store_pc") == f"{seg | (labels['ov_run'] + 4):08X}",
              f"{seg:08X}: the store-PC stamp the host saw is in the segment")
        tags = [int(t, 16) for t in r.get("fetch", "").split(",") if t]
        check(tags and all(t & co.SEGMENT_MASK == seg for t in tags),
              f"{seg:08X}: every fetch tag is in the segment")
        want = LEDGER.beetle_cycles([seg | pc for pc in executed_sequence(labels)])
        got = icache.cycles([tags])[0] if tags else -1
        check(got == want, f"{seg:08X}: fetch cycles {got} == Beetle's {want}")
    k0 = runs.get(KSEG0, {})
    check(k0.get("native") == "0" and k0.get("interp_at") == f"{KSEG0 | PHYS:08X}",
          "KSEG0 PC has no shard: interpreted")
    check(totals.get("segment_native") == "6" and
          totals.get("segment_alias_interp") == "0",
          "KUSEG/KSEG1 dispatches all native; a KSEG0 miss is no alias")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler",
                    default=str(ROOT / "recompiler" / "build" / "psxrecomp-game"))
    ap.add_argument("--compiler", default=os.environ.get("CC") or
                    shutil.which("cc") or shutil.which("gcc"))
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler}")
    if not args.compiler:
        raise SystemExit("a C compiler is required")
    data, labels = overlay_image()
    check_views(data, labels)
    check_manifests()
    check_layout()
    with tempfile.TemporaryDirectory(prefix="psx-overlay-segment-",
                                     ignore_cleanup_errors=True) as tmp:
        cache = check_compile(args.recompiler, args.compiler, data, labels, tmp)
        check_demands(args.recompiler, args.compiler, data, labels, tmp)
        check_static_fragments(args.recompiler, args.compiler, tmp)
        check_segment_isolation(args.recompiler, args.compiler, data, labels, tmp)
        check_config_sites(args.recompiler, args.compiler, data, labels, tmp)
        check_loader(args.recompiler, args.compiler, data, labels, cache, tmp)
    if FAILURES:
        print(f"FAIL: {len(FAILURES)} check(s)")
        return 1
    print("PASS: overlay shards are compiled, cached and dispatched per segment")
    return 0


if __name__ == "__main__":
    sys.exit(main())
