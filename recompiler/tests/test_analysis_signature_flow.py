"""Infer incoming arguments from reachable paths and R3000 load timing."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--analyzer", required=True)
    args = ap.parse_args()
    load = 0x80010000
    # Expected masks describe reads of the ORIGINAL a0..a3 register values.
    cases = [
        ("forward_join", [0x10800003, 0, 0x24050009, 0,
                          0x00A01021, 0x03E00008, 0], 3, True),
        ("branch_delay_defines", [0x10800003, 0x24050009, 0, 0,
                                 0x00A01021, 0x03E00008, 0], 1, True),
        ("load_delay_reads_old_argument", [0x8C850000, 0x00A01021,
                                          0x03E00008, 0], 3, True),
        ("load_commits_after_nop", [0x8C850000, 0, 0x00A01021,
                                    0x03E00008, 0], 1, True),
        ("return_delay_reads_argument", [0x00001021, 0x03E00008,
                                         0x00C01021], 4, True),
        ("jump_skips_dead_read", [0x08000000 | (((load + 0x90C) >> 2) & 0x3FFFFFF), 0,
                                  0x00C01821, 0x24020001, 0x03E00008, 0], 0, True),
        ("outgoing_branch_fragment", [0x10800007, 0, 0x03E00008, 0x24020000,
                                      0, 0, 0, 0, 0x00A01021, 0x03E00008, 0], 1, False),
        ("loop_reads_initial_argument", [0x00A01021, 0x24050009, 0x1480FFFD,
                                         0, 0x03E00008, 0], 3, False),
        ("join_then_load_commit", [0x10800004, 0, 0x24050009,
                                   0x08000000 | (((load + 0xC18) >> 2) & 0x3FFFFFF), 0,
                                   0x8CC50000, 0, 0x00A01021, 0x03E00008, 0], 5, True),
        ("scheduled_frame", [0x8F820010, 0x27BDFFE8, 0xAFBF0010,
                             0x8FBF0010, 0, 0x03E00008, 0x27BD0018], 0, True),
        ("variadic_frame", [0xAFA40000, 0xAFA50004, 0xAFA60008, 0xAFA7000C,
                            0x27BDFFE8, 0xAFBF0010, 0x8FBF0010, 0,
                            0x03E00008, 0x27BD0018], 15, True),
        ("coprocessor_load_delay", [0x48050000, 0x00A01021,
                                    0x03E00008, 0], 2, True),
        ("pending_load_cancelled_by_write", [0x8C850000, 0x24050009,
                                             0x00A01021, 0x03E00008, 0], 1, True),
        ("indirect_tail_delay_reads_argument", [0x27BDFFE8, 0xAFBF0010,
                                               0x03200009, 0x00A01021,
                                               0x00C01821, 0x03E00008, 0x27BD0018], 2, False),
        ("branch_delay_frame", [0x14800002, 0x27BDFFE8, 0,
                                0x03E00008, 0x27BD0018], 1, True),
        ("jump_delay_frame", [0x08000000 | (((load + 0x1308) >> 2) & 0x3FFFFFF),
                              0x27BDFFE8, 0x03E00008, 0x27BD0018], 0, True),
        ("return_before_other_frame", [0x03E00008, 0, 0x27BDFFE8,
                                       0x03E00008, 0x27BD0018], 0, True),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        image = bytearray(b"\xff" * (2048 + 0x2000))
        image[:2048] = bytes(2048)
        image[:8] = b"PS-X EXE"
        struct.pack_into("<IIII", image, 16, load, 0, load, 0x2000)
        entries = {}
        for i, (name, words, mask, confident) in enumerate(cases):
            pc = load + 0x400 + i * 0x100
            entries[name] = (pc, mask, confident)
            struct.pack_into("<II", image, 2048 + i * 8,
                             0x0C000000 | ((pc >> 2) & 0x3FFFFFF), 0)
            for j, word in enumerate(words):
                struct.pack_into("<I", image, 2048 + pc - load + j * 4, word)
        struct.pack_into("<II", image, 2048 + len(cases) * 8, 0x03E00008, 0)
        exe = root / "flow.exe"
        exe.write_bytes(image)
        subprocess.run([args.analyzer, str(exe), "--out", str(root / "out"),
                        "--no-refs", "--quiet"], check=True, capture_output=True)
        functions = {f["addr"]: f for f in json.loads(
            (root / "out/analysis.json").read_text())["functions"]}
        failures = []
        for name, (pc, mask, confident) in entries.items():
            f = functions.get(pc)
            if not f or f["is_data"]:
                failures.append(f"{name}: entry missing or classified as data")
            elif f["sig"]["arg_mask"] != mask or f["sig"]["confident"] != confident:
                failures.append(f"{name}: expected mask={mask}, confident={confident}; {f['sig']}")
            if f and name in ("scheduled_frame", "variadic_frame",
                               "branch_delay_frame", "jump_delay_frame"):
                if f["sig"]["stack_frame"] != 24 or not f["has_prologue"]:
                    failures.append(f"{name}: scheduled 24-byte frame omitted: {f['sig']}")
            if f and name == "return_before_other_frame" and f["sig"]["stack_frame"] != 0:
                failures.append(f"{name}: frame from another body: {f['sig']}")
        if failures:
            raise AssertionError("\n".join(failures))
    print("PASS: path-sensitive arguments and delayed GPR writes")


if __name__ == "__main__":
    main()
