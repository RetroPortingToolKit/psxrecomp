#!/usr/bin/env python3
"""A/B the declared kernel install-slot ranges: what do they buy this game?

Boots the game twice against the SAME binary, to the same frame, once with the
profile's [[recompiler.install_slots]] ranges live and once with
PSX_KERNEL_PATCH_RANGES=0, which drops them and restores the whole-body
kernel-bless memcmp - the behaviour before the ranges existed. One binary
measures both sides, so the overlays, the codegen and the disc are held
constant and only the bless verdict moves.

Run from a game project root:

    python psxrecomp/tools/kernel_patch_ab.py
    python psxrecomp/tools/kernel_patch_ab.py --bios psxrecomp/bios/SCPH1001.BIN
    python psxrecomp/tools/kernel_patch_ab.py --frames 20000

Set PSX_BIOS_HLE=0 to isolate this change from the kernel-call HLE tier. That
matters on an image exporting a DeliverEvent anchor (retail SCPH-1001);
OpenBIOS has none, so its kernel calls are LLE either way.

Headless runs uncapped, so a frame is a unit of guest work rather than wall
time, and the two sides reach slightly different frames in the same wall
budget - the report normalises per frame.

Read "kernel" and "all" as the result. The bodies/vectors split underneath is
diagnosis: whether the A0/B0/C0 vectors interpret on both sides is
title-dependent (see kernel_patch_common.VECTOR_STUBS), so they are shown but
never discounted.
"""
import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kernel_patch_common as kp


def wait_port(host, port, secs=180):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            r = kp.q("frame", host, port, timeout=30.0)
            if r.get("ok") or "frame" in r:
                return
        except Exception:
            pass
        time.sleep(1)
    raise SystemExit("debug port never came up")


def run_side(a, root, ranges_on):
    env = dict(os.environ, PSX_STARVATION_TIMEOUT_US="0")
    env.pop("PSX_LOAD_SLOT", None)
    if ranges_on:
        env.pop("PSX_KERNEL_PATCH_RANGES", None)
    else:
        env["PSX_KERNEL_PATCH_RANGES"] = "0"
    argv = [os.path.join(root, a.exe), "--game", a.game, "--no-launcher",
            "--headless", "--debug-port", str(a.port)]
    if a.bios:
        argv += ["--bios", os.path.join(root, a.bios)]
    proc = subprocess.Popen(argv, cwd=root, env=env,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    try:
        wait_port(a.host, a.port)
        t0 = time.time()
        # A wedge shows up as a frame counter that stops advancing while the
        # debug server still answers, so watch for STALL as well as the overall
        # timeout - a boot that hangs at frame 0 should not burn the whole
        # budget before saying so. It did once: a dispatch key inside a
        # declared range spun the dispatch loop forever.
        last_fr, last_move = -1, time.time()
        while True:
            try:
                fr = kp.q("frame", a.host, a.port, timeout=30.0).get("frame") or 0
            except Exception:
                fr = last_fr        # transient; the stall check still applies
            if fr >= a.frames:
                break
            if fr != last_fr:
                last_fr, last_move = fr, time.time()
            elif time.time() - last_move > a.stall:
                raise SystemExit("WEDGED: frame stuck at %s for %ds (target %d)"
                                 % (fr, a.stall, a.frames))
            if time.time() - t0 > a.timeout:
                raise SystemExit("only reached frame %s of %d in %ds"
                                 % (fr, a.frames, a.timeout))
            time.sleep(1.0)
        kb = kp.q("kernel_bless", a.host, a.port)
        st = kp.q("dirty_ram_stats", a.host, a.port)
        rows = [r for r in st.get("per_pc", [])
                if int(r["pc"], 16) < kp.KERNEL_WINDOW_END]
        kernel = sum(r["insns"] for r in rows)
        tramp = sum(r["insns"] for r in rows
                    if int(r["pc"], 16) in kp.VECTOR_STUBS)
        top = sorted(rows, key=lambda r: -r["insns"])[:12]
        return {
            "ranges_on": ranges_on,
            "frame": fr,
            "wall_s": round(time.time() - t0, 1),
            "bless_entries": kb.get("entries"),
            "bless_clean": kb.get("clean"),
            "bless_mismatch": kb.get("mismatch"),
            "bless_native_hits": kb.get("native_hits"),
            "patch_ranges": kb.get("patch_ranges"),
            "patch_skips": kb.get("patch_skips"),
            "interp_insns_total": st.get("insns_run"),
            "interp_insns_kernel": kernel,
            "interp_insns_vectors": tramp,
            "interp_insns_kernel_bodies": kernel - tramp,
            "native_handoffs": st.get("native_handoffs"),
            "top_kernel_pcs": [{"pc": r["pc"], "insns": r["insns"],
                                "entries": r["entries"]} for r in top],
        }
    finally:
        proc.kill()
        time.sleep(2)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split(chr(10) + chr(10), 1)[0])
    kp.add_project_args(ap, default_port=4372)
    ap.add_argument("--frames", type=int, default=11000,
                    help="frame target for both sides (default 11000)")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--stall", type=int, default=90,
                    help="fail if the frame counter does not advance for this long")
    ap.add_argument("--out", default="analysis/kernel_patch_ab.json")
    a = ap.parse_args()
    root = kp.resolve(a)

    pranges = kp.parse_patch_ranges(os.path.join(root, a.dispatch))
    print("%s declares %d install-slot range(s): %s"
          % (os.path.basename(a.dispatch), len(pranges),
             ", ".join("0x%04X..0x%04X" % r for r in pranges) or "none"), flush=True)
    if not pranges:
        raise SystemExit("nothing to A/B: this backend declares no ranges. Add "
                         "[[recompiler.install_slots]] to the BIOS profile and "
                         "regenerate (see docs/dynamic_handler_install.md).")

    rows = []
    for ranges_on in (False, True):
        print("=== %s" % ("ranges ON (declared install slots)" if ranges_on else
                          "ranges OFF (PSX_KERNEL_PATCH_RANGES=0, pre-change "
                          "behaviour)"), flush=True)
        r = run_side(a, root, ranges_on)
        rows.append(r)
        print("    frame %(frame)s in %(wall_s)ss  bless: %(bless_clean)s clean / "
              "%(bless_mismatch)s mismatch of %(bless_entries)s  "
              "ranges=%(patch_ranges)s skips=%(patch_skips)s" % r, flush=True)
        print("    interpreted: %(interp_insns_total)s total, "
              "%(interp_insns_kernel)s kernel RAM" % r, flush=True)
        for t in r["top_kernel_pcs"][:6]:
            print("      %s insns=%-10s entries=%s"
                  % (t["pc"], t["insns"], t["entries"]), flush=True)

    off, on = rows[0], rows[1]
    print()
    print("frames: OFF %s, ON %s; counts below are PER FRAME, since headless "
          "runs uncapped" % (off["frame"], on["frame"]))
    print("%-30s %14s %14s %9s" % ("", "ranges OFF", "ranges ON", "change"))

    def line(label, key, per_frame=True):
        o, n = off.get(key) or 0, on.get(key) or 0
        if per_frame:
            o = o / float(off["frame"] or 1)
            n = n / float(on["frame"] or 1)
            os_, ns_ = "%.1f" % o, "%.1f" % n
        else:
            os_, ns_ = str(o), str(n)
        ch = ("%+.1f%%" % (100.0 * (n - o) / o)) if o else "n/a"
        print("%-30s %14s %14s %9s" % (label, os_, ns_, ch))

    line("kernel-bless mismatch", "bless_mismatch", False)
    line("kernel-bless clean", "bless_clean", False)
    line("interp insns / frame, kernel", "interp_insns_kernel")
    line("interp insns / frame, all", "interp_insns_total")
    print("  of which (diagnostic, not discounted):")
    line("  kernel bodies", "interp_insns_kernel_bodies")
    line("  A0/B0/C0 vectors", "interp_insns_vectors")

    out = os.path.join(root, a.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    json.dump({"frames": a.frames, "bios": a.bios, "dispatch": a.dispatch,
               "declared_ranges": ["0x%04X..0x%04X" % r for r in pranges],
               "sides": rows},
              open(out, "w", encoding="utf-8"), indent=1)
    print(chr(10) + "wrote " + a.out)


if __name__ == "__main__":
    main()
