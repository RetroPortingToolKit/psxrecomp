#!/usr/bin/env python3
"""Record evidence of native scene passes during a caller-selected gameplay case.

This observes an already running debug build. It never boots, drives, pauses or
changes settings. A passing sample is one gameplay window, not title acceptance.
Run with PSX_RENDER_PASS_VERIFY=1 to check that replay restores guest state.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time

from debug_client import connect, send_cmd


COUNTERS = ("passes", "pass_presents", "verify_checks", "verify_mismatch",
            "vram_leaks", "watchdog", "nesting_repairs", "span_failures",
            "aborted", "blended_presents")


def assess(before, after):
    reasons, delta = [], {}
    for key in COUNTERS:
        a, b = before.get(key), after.get(key)
        if type(a) is not int or type(b) is not int or b < a:
            reasons.append("missing, invalid or reset counter: " + key)
        else:
            delta[key] = b - a
    for key in ("passes", "pass_presents", "verify_checks"):
        if key in delta and not delta[key]:
            reasons.append("no evidence during this window: " + key)
    for key in ("verify_mismatch", "vram_leaks", "watchdog", "nesting_repairs",
                "span_failures", "aborted", "blended_presents"):
        if delta.get(key, 0):
            reasons.append("unexpected replay result: " + key)
    if after.get("disabled") != 0:
        reasons.append("replay disabled or status missing")
    return delta, reasons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--case", required=True, help="actual gameplay path being observed")
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 0 < args.seconds <= 60:
        parser.error("--seconds must be greater than zero and at most 60")
    executable = args.executable.resolve(strict=True)
    with executable.open("rb") as stream:
        binary_sha = hashlib.file_digest(stream, "sha256").hexdigest()
    manifest_path = executable.with_suffix(".execution.json")
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else None
    receipt = dict(schema=1, case=args.case, executable=str(executable),
                   executable_sha256=binary_sha, execution=manifest,
                   started_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                   port=args.port, sample_passed=False)

    def query():
        with connect(port=args.port) as sock:
            response = send_cmd(sock, {"cmd": "render_pass_stats"})
        if not isinstance(response, dict) or response.get("ok") is not True:
            raise RuntimeError("render_pass_stats refused: " + repr(response))
        return response

    try:
        receipt["before"] = query()
        start = time.monotonic()
        time.sleep(args.seconds)
        receipt["after"] = query()
        receipt["elapsed_seconds"] = time.monotonic() - start
        receipt["delta"], receipt["reasons"] = assess(receipt["before"], receipt["after"])
        receipt["sample_passed"] = not receipt["reasons"]
    except (OSError, ValueError, RuntimeError) as error:
        receipt["reasons"] = [str(error)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: receipt.get(key) for key in ("case", "sample_passed", "delta", "reasons")}))
    return 0 if receipt["sample_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
