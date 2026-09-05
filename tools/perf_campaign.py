#!/usr/bin/env python3
"""Repeatable fixed-input performance campaign runner for psxrecomp.

This is P0-1 campaign infrastructure, not a production qualification oracle.
It drives the native debug TCP server with a preloaded digital input route,
captures raw before/after evidence, and can summarize interleaved A/B samples.

The timed window is observed through a debug-tools build, so the report labels
the result diagnostic. Use it to make runs repeatable and reviewable; final
acceptance still needs the HOST_OPTIMIZATION_CONTRACT quiet-host procedure.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
import platform
import socket
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


REPORT_SCHEMA_VERSION = 1
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 4370
PHASE_RING_MAX_WINDOW = 62
PC_SAMPLE_KIND = 15
STARV_RING_MAX_COUNT = 2048
REQUIRED_SNAPSHOT_COMMANDS = (
    ("freeze_check", {"window": 256}),
    ("overlay_loader_status", {}),
    ("autocompile_status", {}),
    ("dirty_ram_stats", {}),
    ("kernel_bless", {}),
)
OPTIONAL_PROVENANCE_COMMANDS = (
    ("ping", {}),
    ("bios_info", {}),
    ("game_options", {}),
    ("pace_state", {}),
    ("turbo_state", {}),
    ("gpu_state", {}),
)
OPTIONAL_SNAPSHOT_COMMANDS = (
    ("frame_perf", {}),
)
TARGET_PROVENANCE_KEYS = (
    "artifact_sha256",
    "settings_sha256",
    "bios_sha256",
    "overlay_inventory_sha256",
    "host_id",
    "cpu_budget",
    "instrumentation",
)
TARGET_ENV_KEYS = (
    "settings_sha256",
    "bios_sha256",
    "overlay_inventory_sha256",
    "host_id",
    "cpu_budget",
    "instrumentation",
)
TARGET_SIDE_KEYS = (
    "artifact_sha256",
    "variant_selector",
)
SHA_KEYS = (
    "artifact_sha256",
    "settings_sha256",
    "bios_sha256",
    "overlay_inventory_sha256",
)
RESETTABLE_COUNTER_PATHS = (
    ("freeze_check", "psx_cycle_count"),
    ("freeze_check", "frame_count"),
    ("freeze_check", "dirty_ram_insns"),
    ("freeze_check", "dirty_ram_blocks"),
    ("freeze_check", "dirty_ram_aborts"),
    ("overlay_loader_status", "loads"),
    ("overlay_loader_status", "invalidations"),
    ("overlay_loader_status", "revalidations"),
    ("overlay_loader_status", "dispatch_native"),
    ("overlay_loader_status", "dispatch_interp_fallback"),
    ("overlay_loader_status", "stale_blocked"),
    ("dirty_ram_stats", "blocks_run"),
    ("dirty_ram_stats", "insns_run"),
    ("dirty_ram_stats", "native_handoffs"),
)
AUTOCOMPILE_COUNTER_PATHS = (
    ("compile", "runs"),
    ("compile", "fails"),
    ("compile", "shard_fail_total"),
)


class CampaignError(RuntimeError):
    """The campaign evidence is incomplete or unsafe to compare."""


def _load_debug_client():
    path = Path(__file__).resolve().with_name("debug_client.py")
    spec = importlib.util.spec_from_file_location("psxrecomp_debug_client", path)
    module = importlib.util.module_from_spec(spec)
    if spec.loader is None:
        raise CampaignError(f"cannot load debug_client.py from {path}")
    spec.loader.exec_module(module)
    return module


DEBUG_CLIENT = _load_debug_client()


class Client:
    """Strict one-command-per-connection client.

    The native server accepts one request per connection for this workflow.
    Transport parsing is delegated to tools/debug_client.py's balanced JSON
    reader; this wrapper adds ids, timeout plumbing, and response validation.
    """

    def __init__(self, host: str, port: int, timeout: float = 8.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.next_id = 1

    def cmd(self, name: str, **params: Any) -> dict[str, Any]:
        request_id = self.next_id
        self.next_id += 1
        request = {"id": request_id, "cmd": name}
        request.update(params)
        try:
            sock = DEBUG_CLIENT.connect(self.host, self.port, timeout=self.timeout)
            try:
                reply = DEBUG_CLIENT.send_cmd(sock, request)
            finally:
                sock.close()
        except (OSError, socket.timeout, json.JSONDecodeError) as exc:
            return {"ok": False, "error": f"{type(exc).__name__}: {exc}"}
        if not isinstance(reply, dict):
            return {"ok": False, "error": "reply is not an object"}
        reply_id = reply.get("id")
        ping_fast_path = (
            name == "ping"
            and reply_id == 0
            and reply.get("pong") is True
            and reply.get("io_thread") is True
        )
        if reply_id != request_id and not ping_fast_path:
            return {
                "ok": False,
                "error": f"response id mismatch: expected {request_id}, got {reply_id}",
            }
        return reply


def require_object(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise CampaignError(f"missing object: {context}")
    return value


def require_int(obj: dict[str, Any], key: str, context: str) -> int:
    if key not in obj:
        raise CampaignError(f"missing field: {context}.{key}")
    value = obj[key]
    if isinstance(value, bool):
        raise CampaignError(f"invalid numeric field: {context}.{key}")
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 16) if value.startswith("0x") else int(value)
        except ValueError as exc:
            raise CampaignError(f"invalid numeric field: {context}.{key}") from exc
    raise CampaignError(f"invalid numeric field: {context}.{key}")


def require_real(obj: dict[str, Any], key: str, context: str) -> float:
    if key not in obj:
        raise CampaignError(f"missing field: {context}.{key}")
    value = obj[key]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CampaignError(f"invalid real field: {context}.{key}")
    if not math.isfinite(float(value)):
        raise CampaignError(f"invalid real field: {context}.{key}")
    return float(value)


def command(client: Any, name: str, **params: Any) -> dict[str, Any]:
    reply = client.cmd(name, **params)
    if not isinstance(reply, dict):
        raise CampaignError(f"{name} failed: invalid reply")
    if not reply.get("ok", False):
        raise CampaignError(f"{name} failed: {reply.get('error', 'unknown error')}")
    return reply


def command_or_warning(client: Any, name: str, **params: Any) -> tuple[dict[str, Any] | None, str | None]:
    reply = client.cmd(name, **params)
    if isinstance(reply, dict) and reply.get("ok", False):
        return reply, None
    detail = reply.get("error", "invalid reply") if isinstance(reply, dict) else "invalid reply"
    return None, f"optional {name} unavailable: {detail}"


@dataclass(frozen=True)
class RouteStep:
    frames: int
    buttons: int
    lx: int | None = None
    ly: int | None = None
    rx: int | None = None
    ry: int | None = None

    def append_params(self) -> dict[str, int]:
        axes = {k: v for k, v in {
            "lx": self.lx, "ly": self.ly, "rx": self.rx, "ry": self.ry,
        }.items() if v is not None}
        if axes:
            raise CampaignError(
                "input_route_append is digital-only in the native server; "
                "analog route steps cannot be replayed faithfully"
            )
        return {"frames": self.frames, "buttons": self.buttons}


def _int_from_json(value: Any, context: str) -> int:
    if isinstance(value, bool):
        raise CampaignError(f"invalid integer: {context}")
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 16) if value.startswith("0x") else int(value)
        except ValueError as exc:
            raise CampaignError(f"invalid integer: {context}") from exc
    raise CampaignError(f"invalid integer: {context}")


def load_route(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise CampaignError(f"cannot read route {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise CampaignError(f"invalid route JSON {path}: {exc}") from exc
    raw_steps = data.get("steps") if isinstance(data, dict) else data
    if not isinstance(raw_steps, list) or not raw_steps:
        raise CampaignError("route must be a non-empty list or object with steps")
    steps: list[RouteStep] = []
    for index, raw in enumerate(raw_steps):
        if not isinstance(raw, dict):
            raise CampaignError(f"route.steps[{index}] is not an object")
        frames = _int_from_json(raw.get("frames"), f"route.steps[{index}].frames")
        buttons = _int_from_json(raw.get("buttons"), f"route.steps[{index}].buttons")
        if frames <= 0:
            raise CampaignError(f"route.steps[{index}].frames must be positive")
        if buttons < 0 or buttons > 0xFFFF:
            raise CampaignError(f"route.steps[{index}].buttons must be 0..0xFFFF")
        axes = {}
        for axis in ("lx", "ly", "rx", "ry"):
            if axis in raw:
                value = _int_from_json(raw[axis], f"route.steps[{index}].{axis}")
                if value < 0 or value > 255:
                    raise CampaignError(f"route.steps[{index}].{axis} must be 0..255")
                axes[axis] = value
        steps.append(RouteStep(frames=frames, buttons=buttons, **axes))
    canonical = {
        "name": data.get("name", path.stem) if isinstance(data, dict) else path.stem,
        "source": str(path),
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "steps": [step.__dict__ for step in steps],
        "total_frames": sum(step.frames for step in steps),
    }
    for key in (
        "title",
        "bios",
        "state",
        "state_sha256",
        "restore_witness",
        "scene",
        "notes",
    ):
        if isinstance(data, dict) and key in data:
            canonical[key] = data[key]
    return canonical


def load_target_provenance(path: Path | None) -> dict[str, Any] | None:
    if path is None:
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise CampaignError(f"cannot read provenance {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise CampaignError(f"invalid provenance JSON {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise CampaignError("provenance JSON must be an object")
    validate_target_shape(data, "provenance")
    data = dict(data)
    data["source"] = str(path)
    data["source_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    return data


def is_hex64(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(c in "0123456789abcdefABCDEF" for c in value)
    )


def validate_cpu_budget(value: Any, context: str) -> None:
    if not isinstance(value, dict):
        raise CampaignError(f"{context} cpu_budget must be an object")
    if value.get("unrestricted") is True:
        return
    for key in ("cap", "denominator", "affinity"):
        if key not in value:
            raise CampaignError(
                f"{context} cpu_budget must contain unrestricted:true or cap, denominator, affinity"
            )
    cap = value["cap"]
    denominator = value["denominator"]
    if isinstance(cap, bool) or not isinstance(cap, int) or cap < 0:
        raise CampaignError(f"{context} cpu_budget.cap must be a non-negative integer")
    if isinstance(denominator, bool) or not isinstance(denominator, int) or denominator <= 0:
        raise CampaignError(f"{context} cpu_budget.denominator must be a positive integer")
    affinity = value["affinity"]
    if isinstance(affinity, str):
        if not affinity:
            raise CampaignError(f"{context} cpu_budget.affinity must be non-empty")
    elif isinstance(affinity, list):
        if not affinity:
            raise CampaignError(f"{context} cpu_budget.affinity must be non-empty")
    else:
        raise CampaignError(f"{context} cpu_budget.affinity must be a string or list")


def validate_target_shape(target: dict[str, Any], context: str) -> None:
    missing = [key for key in TARGET_PROVENANCE_KEYS if key not in target]
    if missing:
        raise CampaignError(f"{context} target provenance missing required keys: " + ", ".join(missing))
    for key in SHA_KEYS:
        if not is_hex64(target.get(key)):
            raise CampaignError(f"{context}.{key} must be a 64-character hex SHA256")
    host_id = target.get("host_id")
    if not isinstance(host_id, str) or not host_id:
        raise CampaignError(f"{context}.host_id must be a non-empty string")
    instrumentation = target.get("instrumentation")
    if not isinstance(instrumentation, dict) or not instrumentation:
        raise CampaignError(f"{context}.instrumentation must be a non-empty object")
    validate_cpu_budget(target.get("cpu_budget"), context)


def hash_file(path: Path) -> dict[str, Any]:
    try:
        h = hashlib.sha256()
        size = 0
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                size += len(chunk)
                h.update(chunk)
    except OSError as exc:
        raise CampaignError(f"cannot hash state file {path}: {exc}") from exc
    return {"path": str(path), "sha256": h.hexdigest(), "size": size}


def append_and_start_route(client: Any, route: dict[str, Any]) -> dict[str, Any]:
    command(client, "clear_input")
    command(client, "input_route_clear")
    for index, step_data in enumerate(route["steps"]):
        step = RouteStep(**step_data)
        reply = command(client, "input_route_append", **step.append_params())
        steps = require_int(reply, "steps", "input_route_append")
        if steps != index + 1:
            raise CampaignError(f"input_route_append returned steps={steps}, expected {index + 1}")
    return command(client, "input_route_start")


def wait_restore_done(client: Any, slot: int, timeout_s: float, poll_s: float) -> dict[str, Any]:
    before = command(client, "savestate_status")
    before_generation = require_int(before, "generation", "savestate_status")
    staged = command(client, "savestate", op="load", slot=slot)
    deadline = time.monotonic() + timeout_s
    last: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        last = command(client, "savestate_status")
        generation = require_int(last, "generation", "savestate_status")
        pending = require_int(last, "pending", "savestate_status")
        last_ok = require_int(last, "last_ok", "savestate_status")
        last_slot = require_int(last, "last_slot", "savestate_status")
        if generation != before_generation and pending == 0:
            if last_ok != 1 or last.get("last_op") != "load" or last_slot != slot:
                raise CampaignError(f"savestate load did not complete successfully: {last}")
            return {"before": before, "staged": staged, "after": last}
        time.sleep(poll_s)
    raise CampaignError(f"savestate load timeout after {timeout_s:.1f}s; last_status={last}")


def wait_route_done(client: Any, timeout_s: float, poll_s: float, expected_steps: int) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    last: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        last = command(client, "input_route_status")
        if last.get("active") is False:
            steps = require_int(last, "steps", "input_route_status")
            index = require_int(last, "index", "input_route_status")
            remaining = require_int(last, "remaining", "input_route_status")
            if steps != expected_steps or index != expected_steps or remaining != 0:
                raise CampaignError(
                    "input route stopped before consuming all steps: "
                    f"steps={steps} index={index} remaining={remaining} expected={expected_steps}"
                )
            return last
        time.sleep(poll_s)
    try:
        command(client, "input_route_stop")
        command(client, "clear_input")
    finally:
        pass
    raise CampaignError(f"route timeout after {timeout_s:.1f}s; last_status={last}")


def snapshot(client: Any, phase_window: int, hot_top: int) -> dict[str, Any]:
    snap: dict[str, Any] = {
        "wall": time.time(),
        "monotonic": time.monotonic(),
        "phase_window_requested_s": phase_window,
        "phase_hot_requested_top": hot_top,
        "commands": {},
        "warnings": [],
    }
    for name, params in REQUIRED_SNAPSHOT_COMMANDS:
        snap["commands"][name] = command(client, name, **params)
    for name, params in OPTIONAL_SNAPSHOT_COMMANDS:
        reply, warning = command_or_warning(client, name, **params)
        if reply is not None:
            snap["commands"][name] = reply
        elif warning:
            snap["warnings"].append(warning)
    snap["commands"]["phase_profile"] = command(client, "phase_profile", window=phase_window)
    snap["commands"]["phase_hot:native"] = command(client, "phase_hot", set="native", top=hot_top)
    snap["commands"]["phase_hot:static"] = command(client, "phase_hot", set="static", top=hot_top)
    provenance = {}
    for name, params in OPTIONAL_PROVENANCE_COMMANDS:
        reply, warning = command_or_warning(client, name, **params)
        if reply is not None:
            provenance[name] = reply
        elif warning:
            snap["warnings"].append(warning)
    snap["provenance"] = provenance
    return snap


def best_effort_snapshot(client: Any, phase_window: int, hot_top: int, max_errors: int = 3) -> dict[str, Any]:
    snap: dict[str, Any] = {
        "wall": time.time(),
        "monotonic": time.monotonic(),
        "phase_window_requested_s": phase_window,
        "phase_hot_requested_top": hot_top,
        "commands": {},
        "errors": {},
    }
    commands = (
        *REQUIRED_SNAPSHOT_COMMANDS,
        *OPTIONAL_SNAPSHOT_COMMANDS,
        ("phase_profile", {"window": phase_window}),
        ("phase_hot:native", {"cmd": "phase_hot", "set": "native", "top": hot_top}),
        ("phase_hot:static", {"cmd": "phase_hot", "set": "static", "top": hot_top}),
    )
    for key, params_src in commands:
        params = dict(params_src)
        name = params.pop("cmd", key)
        try:
            snap["commands"][key] = command(client, name, **params)
        except Exception as exc:
            snap["errors"][key] = str(exc)
            if len(snap["errors"]) >= max_errors:
                snap["aborted"] = f"stopped after {max_errors} capture errors"
                return snap
    for name, params in OPTIONAL_PROVENANCE_COMMANDS:
        try:
            snap["commands"][name] = command(client, name, **params)
        except Exception as exc:
            snap["errors"][name] = str(exc)
            if len(snap["errors"]) >= max_errors:
                snap["aborted"] = f"stopped after {max_errors} capture errors"
                return snap
    return snap


def _cmd(snap: dict[str, Any], name: str) -> dict[str, Any]:
    return require_object(require_object(snap.get("commands"), "commands").get(name), name)


def counter_delta(before: dict[str, Any], after: dict[str, Any], command_name: str, key: str) -> int:
    b = require_int(_cmd(before, command_name), key, command_name)
    a = require_int(_cmd(after, command_name), key, command_name)
    delta = a - b
    if delta < 0:
        raise CampaignError(f"counter reset: {command_name}.{key}")
    return delta


def nested_counter_delta(
    before: dict[str, Any], after: dict[str, Any], command_name: str, object_name: str, key: str
) -> int:
    bobj = require_object(_cmd(before, command_name).get(object_name), f"{command_name}.{object_name}")
    aobj = require_object(_cmd(after, command_name).get(object_name), f"{command_name}.{object_name}")
    delta = require_int(aobj, key, f"{command_name}.{object_name}") - require_int(
        bobj, key, f"{command_name}.{object_name}"
    )
    if delta < 0:
        raise CampaignError(f"counter reset: {command_name}.{object_name}.{key}")
    return delta


def summarize_window(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    deltas: dict[str, Any] = {}
    for command_name, key in RESETTABLE_COUNTER_PATHS:
        deltas[f"{command_name}.{key}"] = counter_delta(before, after, command_name, key)
    for object_name, key in AUTOCOMPILE_COUNTER_PATHS:
        deltas[f"autocompile_status.{object_name}.{key}"] = nested_counter_delta(
            before, after, "autocompile_status", object_name, key
        )
    wall_s = require_real(after, "monotonic", "after") - require_real(before, "monotonic", "before")
    if wall_s <= 0.0:
        raise CampaignError("non-positive measured wall interval")
    cycle_delta = deltas["freeze_check.psx_cycle_count"]
    frame_delta = deltas["freeze_check.frame_count"]
    guest_mcyc_per_s = cycle_delta / wall_s / 1_000_000.0
    summary = {
        "observed_counter_window_wall_s": wall_s,
        "frame_delta": frame_delta,
        "cycle_delta": cycle_delta,
        "guest_mcyc_per_s": guest_mcyc_per_s,
        "guest_x_realtime": guest_mcyc_per_s / 33.8688,
        "frames_per_host_s": frame_delta / wall_s,
        "frame_perf_available": False,
        "counter_deltas": deltas,
    }
    commands = require_object(after.get("commands"), "commands")
    frame_perf = commands.get("frame_perf")
    if isinstance(frame_perf, dict) and frame_perf.get("ok", False):
        frame_perf_all = require_object(frame_perf.get("all"), "frame_perf.all")
        summary.update({
            "frame_perf_available": True,
            "frame_perf_samples": require_int(frame_perf, "samples", "frame_perf"),
            "frame_perf_all_total_ms_avg": require_real(
                frame_perf_all, "total_ms_avg", "frame_perf.all"
            ),
            "frame_perf_all_total_ms_max": require_real(
                frame_perf_all, "total_ms_max", "frame_perf.all"
            ),
        })
    return summary


def top_delta(before: dict[str, Any], after: dict[str, Any], key: str) -> dict[str, Any]:
    def as_map(reply: dict[str, Any]) -> dict[str, int]:
        out = {}
        for index, row in enumerate(reply.get("top", []) or []):
            obj = require_object(row, f"{key}.top[{index}]")
            addr = obj.get("addr")
            if not isinstance(addr, str) or not addr:
                raise CampaignError(f"invalid address field: {key}.top[{index}].addr")
            out[addr] = require_int(obj, "samples", f"{key}.top[{index}]")
        return out

    before_reply = _cmd(before, key)
    after_reply = _cmd(after, key)
    bmap = as_map(before_reply)
    amap = as_map(after_reply)
    rows = []
    for addr in sorted(set(bmap) & set(amap)):
        value = amap[addr] - bmap[addr]
        if value < 0:
            raise CampaignError(f"counter reset: {key}.top[{addr}]")
        if value:
            rows.append({"addr": addr, "samples": value})
    rows.sort(key=lambda row: -row["samples"])
    total_delta = require_int(after_reply, "phase_samples_total", key) - require_int(
        before_reply, "phase_samples_total", key
    )
    if total_delta < 0:
        raise CampaignError(f"counter reset: {key}.phase_samples_total")
    return {
        "phase_samples_total_delta": total_delta,
        "top_delta": rows[:12],
        "before_only": len(set(bmap) - set(amap)),
        "after_only": len(set(amap) - set(bmap)),
        "hash_drops_end": require_int(after_reply, "hash_drops", key),
    }


def starv_ring(client: Any, count: int) -> dict[str, Any]:
    reply = command(client, "starv_ring", count=count, kind=PC_SAMPLE_KIND)
    entries = reply.get("entries", [])
    if not isinstance(entries, list):
        raise CampaignError("starv_ring.entries is not a list")
    gaps = []
    previous = None
    for index, entry in enumerate(entries):
        obj = require_object(entry, f"starv_ring.entries[{index}]")
        row = {
            "seq": require_int(obj, "seq", f"starv_ring.entries[{index}]"),
            "us": require_int(obj, "us", f"starv_ring.entries[{index}]"),
            "cyc": require_int(obj, "cyc", f"starv_ring.entries[{index}]"),
            "func": obj.get("func", "?"),
            "in_exc": require_int(obj, "in_exc", f"starv_ring.entries[{index}]"),
        }
        if previous is not None:
            if row["seq"] <= previous["seq"] or row["us"] <= previous["us"] or row["cyc"] < previous["cyc"]:
                raise CampaignError("non-monotonic PC-sample ring")
            d_us = row["us"] - previous["us"]
            d_cyc = row["cyc"] - previous["cyc"]
            mcyc_s = d_cyc / d_us
            gaps.append({
                "ms": d_us / 1000.0,
                "guest_cyc": d_cyc,
                "mcyc_per_s": mcyc_s,
                "x_realtime": mcyc_s / 33.8688,
                "func_from": previous["func"],
                "func_to": row["func"],
                "in_exc": row["in_exc"],
            })
        previous = row
    gaps.sort(key=lambda row: -row["ms"])
    total = require_int(reply, "total", "starv_ring")
    return {
        "total_events": total,
        "returned": len(entries),
        "requested": count,
        "ring_has_wrapped": total > 16384,
        "scope": "last returned PC samples only",
        "entries": entries,
        "largest_gaps": gaps[:12],
    }


def tool_identity() -> dict[str, str]:
    path = Path(__file__).resolve()
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def git_identity(repo: Path) -> dict[str, Any]:
    def run(args: list[str]) -> str | None:
        try:
            proc = subprocess.run(
                ["git", *args],
                cwd=repo,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                check=False,
            )
        except OSError:
            return None
        return proc.stdout.strip() if proc.returncode == 0 else None

    return {
        "commit": run(["rev-parse", "HEAD"]),
        "branch": run(["branch", "--show-current"]),
        "dirty": bool(run(["status", "--porcelain"])),
    }


def base_provenance(args: argparse.Namespace, route: dict[str, Any] | None = None) -> dict[str, Any]:
    repo = Path(__file__).resolve().parents[1]
    return {
        "tool": tool_identity(),
        "repo": git_identity(repo),
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "python": sys.version.split()[0],
        },
        "tcp": {
            "host": getattr(args, "host", None),
            "port": getattr(args, "port", None),
            "timeout_s": getattr(args, "timeout", None),
        },
        "route": route,
        "target": getattr(args, "target_provenance", None),
        "labels": {
            "sample": getattr(args, "label", None),
            "baseline": getattr(args, "a_label", None),
            "candidate": getattr(args, "b_label", None),
        },
    }


def run_sample(
    client: Any,
    route: dict[str, Any],
    args: argparse.Namespace,
    partial_out: Path | None = None,
) -> dict[str, Any]:
    phase_window = min(PHASE_RING_MAX_WINDOW, max(1, int(math.ceil(args.timeout)) + 1))
    artifact: dict[str, Any] = {
        "schema_version": REPORT_SCHEMA_VERSION,
        "kind": "psxrecomp-perf-campaign-sample",
        "outcome": "diagnostic",
        "label": args.label,
        "primary_metric": {
            "name": args.primary_metric,
            "unit": "derived",
            "lower_is_better": args.lower_is_better,
        },
        "provenance": base_provenance(args, route),
        "limitations": [
            "debug-tools TCP run; not production acceptance timing",
            "frame_perf is a recent-ring aggregate, not a resettable production counter",
            "route replay currently uses native input_route_* digital pad words only",
            "route_wall_s includes debug route start/status polling overhead",
            "savestate restore is a precondition only unless a separate restore witness is recorded",
        ],
        "warnings": [],
    }
    def checkpoint() -> None:
        if partial_out is not None:
            write_json(partial_out, artifact)

    if getattr(args, "restore_slot", None) is not None:
        restore: dict[str, Any] = {
            "slot": args.restore_slot,
            "timeout_s": args.restore_timeout,
        }
        if getattr(args, "state_file", None) is not None:
            state_file = hash_file(args.state_file)
            expected = route.get("state_sha256")
            if expected is not None and expected != state_file["sha256"]:
                artifact.update({
                    "success": False,
                    "error": (
                        "route state_sha256 does not match --state-file hash: "
                        f"{expected} != {state_file['sha256']}"
                    ),
                    "restore": {"slot": args.restore_slot, "state_file": state_file},
                })
                checkpoint()
                return artifact
            restore["state_file"] = state_file
        artifact["restore"] = restore
    checkpoint()
    try:
        command(client, "ping")
        if getattr(args, "restore_slot", None) is not None:
            witness = wait_restore_done(
                client,
                slot=args.restore_slot,
                timeout_s=args.restore_timeout,
                poll_s=args.poll,
            )
            artifact["restore"]["witness"] = witness
            checkpoint()
        before = snapshot(client, phase_window=phase_window, hot_top=args.hot_top)
        artifact["before"] = before
        checkpoint()
        route_start_mono = time.monotonic()
        start = append_and_start_route(client, route)
        artifact["route_start"] = start
        checkpoint()
        route_status = wait_route_done(
            client,
            timeout_s=args.timeout,
            poll_s=args.poll,
            expected_steps=len(route["steps"]),
        )
        route_end_mono = time.monotonic()
        artifact["route_status"] = route_status
        checkpoint()
        after = snapshot(client, phase_window=phase_window, hot_top=args.hot_top)
        artifact["after"] = after
        checkpoint()
        ring = starv_ring(client, args.starv_count)
        artifact["starv_ring"] = ring
        checkpoint()
        summary = summarize_window(before, after)
        summary["route_wall_s"] = route_end_mono - route_start_mono
        summary["route_start_monotonic"] = route_start_mono
        summary["route_end_monotonic"] = route_end_mono
        summary["route_start_wall"] = before["wall"] + (route_start_mono - before["monotonic"])
        summary["route_end_wall"] = before["wall"] + (route_end_mono - before["monotonic"])
        summary["phase_hot_native"] = top_delta(before, after, "phase_hot:native")
        summary["phase_hot_static"] = top_delta(before, after, "phase_hot:static")
        artifact.update({
            "success": True,
            "summary": summary,
        })
        artifact["warnings"].extend(before.get("warnings", []))
        artifact["warnings"].extend(after.get("warnings", []))
    except Exception as exc:
        try:
            command(client, "input_route_stop")
            command(client, "clear_input")
        except Exception:
            pass
        artifact.update({"success": False, "error": str(exc)})
        artifact["partial_after"] = best_effort_snapshot(
            client, phase_window=1, hot_top=min(args.hot_top, 8)
        )
    checkpoint()
    return artifact


def metric_from_sample(sample: dict[str, Any], metric: str) -> float:
    if not sample.get("success"):
        raise CampaignError(f"sample {sample.get('label', '?')} failed: {sample.get('error')}")
    value: Any = sample
    for part in metric.split("."):
        if not isinstance(value, dict) or part not in value:
            raise CampaignError(f"missing metric {metric}")
        value = value[part]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise CampaignError(f"metric {metric} is not numeric")
    result = float(value)
    if not math.isfinite(result):
        raise CampaignError(f"metric {metric} is not finite")
    return result


def percent_change(a: float, b: float, lower_is_better: bool) -> float:
    if a == 0:
        raise CampaignError("cannot compute percent change from zero baseline")
    raw = ((a - b) / a) * 100.0 if lower_is_better else ((b - a) / a) * 100.0
    return raw


def sample_route_identity(sample: dict[str, Any]) -> dict[str, Any]:
    route = require_object(require_object(sample.get("provenance"), "provenance").get("route"), "provenance.route")
    return {
        "name": route.get("name"),
        "sha256": route.get("sha256"),
        "total_frames": route.get("total_frames"),
        "state_sha256": route.get("state_sha256"),
    }


def sample_metric_identity(sample: dict[str, Any]) -> dict[str, Any]:
    metric = require_object(sample.get("primary_metric"), "primary_metric")
    return {
        "name": metric.get("name"),
        "lower_is_better": metric.get("lower_is_better"),
    }


def require_target(sample: dict[str, Any], label: str) -> dict[str, Any]:
    provenance = require_object(sample.get("provenance"), f"{label}.provenance")
    target = require_object(provenance.get("target"), f"{label}.provenance.target")
    validate_target_shape(target, f"{label}.provenance.target")
    return target


def validate_ab_samples(
    a_samples: list[dict[str, Any]],
    b_samples: list[dict[str, Any]],
    metric: str,
    lower_is_better: bool,
) -> dict[str, Any]:
    route0 = sample_route_identity(a_samples[0])
    metric0 = {"name": metric, "lower_is_better": lower_is_better}
    env0 = {key: require_target(a_samples[0], "A1").get(key) for key in TARGET_ENV_KEYS}
    side_identity: dict[str, Any] = {}
    for label, samples in (("A", a_samples), ("B", b_samples)):
        first_provenance = require_object(samples[0].get("provenance"), "provenance")
        repo0 = first_provenance.get("repo")
        if isinstance(repo0, dict):
            repo0 = {k: repo0.get(k) for k in ("commit", "branch", "dirty")}
        else:
            repo0 = None
        endpoints = []
        target0 = require_target(samples[0], f"{label}1")
        target_side0 = {key: target0.get(key) for key in TARGET_SIDE_KEYS}
        for index, sample in enumerate(samples):
            sample_label = f"{label}{index + 1}"
            route = sample_route_identity(sample)
            if route != route0:
                raise CampaignError(f"{sample_label} route identity differs")
            sample_metric = sample_metric_identity(sample)
            if sample_metric != metric0:
                raise CampaignError(f"{sample_label} primary metric declaration differs")
            target = require_target(sample, sample_label)
            env = {key: target.get(key) for key in TARGET_ENV_KEYS}
            if env != env0:
                raise CampaignError(f"{sample_label} target environment differs")
            target_side = {key: target.get(key) for key in TARGET_SIDE_KEYS}
            if target_side != target_side0:
                raise CampaignError(f"{sample_label} target binary/selector differs within side {label}")
            provenance = require_object(sample.get("provenance"), f"{sample_label}.provenance")
            repo = provenance.get("repo")
            if isinstance(repo, dict):
                repo = {k: repo.get(k) for k in ("commit", "branch", "dirty")}
                if repo0 is not None and repo != repo0:
                    raise CampaignError(f"{sample_label} tools repo identity differs within side {label}")
            tcp = provenance.get("tcp")
            endpoints.append({k: tcp.get(k) for k in ("host", "port")} if isinstance(tcp, dict) else None)
        side_identity[label] = {
            "tools_repo": repo0,
            "tcp_endpoints": endpoints,
            "target_binary": target_side0,
        }
    return {
        "route": route0,
        "primary_metric": metric0,
        "environment": env0,
        "sides": side_identity,
    }


def route_time_bounds(sample: dict[str, Any]) -> tuple[float, float] | None:
    summary = sample.get("summary")
    if not isinstance(summary, dict):
        return None
    start = summary.get("route_start_wall")
    end = summary.get("route_end_wall")
    if not isinstance(start, (int, float)) or isinstance(start, bool):
        return None
    if not isinstance(end, (int, float)) or isinstance(end, bool):
        return None
    start_f = float(start)
    end_f = float(end)
    if not math.isfinite(start_f) or not math.isfinite(end_f) or end_f < start_f:
        return None
    return start_f, end_f


def interleaving_proof(a_samples: list[dict[str, Any]], b_samples: list[dict[str, Any]]) -> dict[str, Any]:
    bounds = []
    for index, (a_sample, b_sample) in enumerate(zip(a_samples, b_samples)):
        a_bounds = route_time_bounds(a_sample)
        b_bounds = route_time_bounds(b_sample)
        if a_bounds is None or b_bounds is None:
            return {"status": "not_proven", "reason": "missing route wall timestamps"}
        bounds.append((f"A{index + 1}", *a_bounds))
        bounds.append((f"B{index + 1}", *b_bounds))
    for left, right in zip(bounds, bounds[1:]):
        if left[2] > right[1]:
            return {
                "status": "not_proven",
                "reason": f"{left[0]} overlaps or follows {right[0]}",
                "observed_order": [row[0] for row in bounds],
            }
    return {"status": "proven", "observed_order": [row[0] for row in bounds]}


def summarize_ab(
    a_samples: list[dict[str, Any]],
    b_samples: list[dict[str, Any]],
    metric: str,
    lower_is_better: bool,
    discards: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    if len(a_samples) != len(b_samples):
        raise CampaignError("A/B sample counts differ")
    if len(a_samples) < 3:
        raise CampaignError("at least three accepted A/B pairs are required")
    identity = validate_ab_samples(a_samples, b_samples, metric, lower_is_better)
    proof = interleaving_proof(a_samples, b_samples)
    if proof.get("status") != "proven":
        raise CampaignError(f"interleaving not proven: {proof.get('reason')}")
    raw_a = [metric_from_sample(sample, metric) for sample in a_samples]
    raw_b = [metric_from_sample(sample, metric) for sample in b_samples]
    best = min if lower_is_better else max
    best_a = best(raw_a)
    best_b = best(raw_b)
    med_a = statistics.median(raw_a)
    med_b = statistics.median(raw_b)
    best_summary = {
        "a": best_a,
        "b": best_b,
        "change_percent": percent_change(best_a, best_b, lower_is_better),
        "selector": "min" if lower_is_better else "max",
    }
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "kind": "psxrecomp-perf-campaign-ab",
        "outcome": "diagnostic",
        "primary_metric": {
            "name": metric,
            "lower_is_better": lower_is_better,
            "unit": "derived",
        },
        "accepted_pairs": len(a_samples),
        "declared_pair_order": [item for i in range(len(a_samples)) for item in (f"A{i + 1}", f"B{i + 1}")],
        "interleaving_proof": proof,
        "identity": identity,
        "sample_artifacts": [
            {"pair": i + 1, "a": a_samples[i].get("artifact_path"),
             "b": b_samples[i].get("artifact_path")}
            for i in range(len(a_samples))
        ],
        "raw_a": raw_a,
        "raw_b": raw_b,
        "best_of_n": best_summary,
        "min_of_n": best_summary if lower_is_better else None,
        "medians": {
            "a": med_a,
            "b": med_b,
            "change_percent": percent_change(med_a, med_b, lower_is_better),
        },
        "discarded_pairs": discards or [],
        "limitations": [
            "debug-tools TCP artifacts; not production acceptance timing",
            "correctness checkpoint result must be supplied separately",
        ],
    }


def load_sample(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise CampaignError(f"cannot read sample {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise CampaignError(f"invalid sample JSON {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise CampaignError(f"sample {path} is not an object")
    data.setdefault("artifact_path", str(path))
    return data


def parse_discard_specs(items: list[str]) -> list[dict[str, Any]]:
    discards = []
    for item in items:
        pair, sep, reason = item.partition(":")
        if not sep:
            raise CampaignError(f"discard must be INDEX:reason, got {item!r}")
        try:
            index = int(pair, 10)
        except ValueError as exc:
            raise CampaignError(f"discard index must be an integer, got {pair!r}") from exc
        if index < 1:
            raise CampaignError("discard index must be >= 1")
        if not reason:
            raise CampaignError("discard reason must be non-empty")
        discards.append({"pair": index, "reason": reason})
    return discards


def apply_discards(
    a_samples: list[dict[str, Any]],
    b_samples: list[dict[str, Any]],
    discards: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]]:
    if len(a_samples) != len(b_samples):
        raise CampaignError("A/B sample counts differ")
    by_pair = {row["pair"]: row for row in discards}
    bad = [index for index in by_pair if index > len(a_samples)]
    if bad:
        raise CampaignError(f"discard index out of range: {bad[0]}")
    kept_a = []
    kept_b = []
    discarded = []
    for index, (a_sample, b_sample) in enumerate(zip(a_samples, b_samples), start=1):
        row = by_pair.get(index)
        if row is None:
            kept_a.append(a_sample)
            kept_b.append(b_sample)
        else:
            discarded.append({
                "pair": index,
                "reason": row["reason"],
                "a": a_sample.get("artifact_path"),
                "b": b_sample.get("artifact_path"),
            })
    return kept_a, kept_b, discarded


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".tmp-{os.getpid()}")
    with tmp.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(data, handle, indent=2, sort_keys=True)
        handle.write("\n")
    os.replace(tmp, path)


def print_sample(sample: dict[str, Any]) -> None:
    label = sample.get("label") or "sample"
    if not sample.get("success"):
        print(f"{label}: FAILED - {sample.get('error')}")
        if sample.get("partial_after_error"):
            print(f"partial snapshot failed: {sample['partial_after_error']}")
        return
    summary = sample["summary"]
    print(f"{label}: diagnostic sample")
    print(f"  route_wall_s={summary['route_wall_s']:.3f}")
    print(f"  observed_counter_window_wall_s={summary['observed_counter_window_wall_s']:.3f}")
    print(f"  frames={summary['frame_delta']}  frames/s={summary['frames_per_host_s']:.2f}")
    print(f"  guest={summary['guest_mcyc_per_s']:.2f} Mcyc/s  xRT={summary['guest_x_realtime']:.2f}")
    if summary.get("frame_perf_available"):
        print(
            "  frame_perf all avg/max="
            f"{summary['frame_perf_all_total_ms_avg']:.3f}/"
            f"{summary['frame_perf_all_total_ms_max']:.3f} ms"
        )
    else:
        print("  frame_perf unavailable")
    if sample.get("warnings"):
        print("  warnings:")
        for warning in sample["warnings"]:
            print(f"    {warning}")


def print_ab(report: dict[str, Any]) -> None:
    metric = report["primary_metric"]["name"]
    direction = "lower is better" if report["primary_metric"]["lower_is_better"] else "higher is better"
    print(f"A/B diagnostic report: {metric} ({direction})")
    print(f"  accepted pairs: {report['accepted_pairs']}")
    print(f"  declared pair order: {' '.join(report['declared_pair_order'])}")
    print(f"  interleaving proof: {report['interleaving_proof']['status']}")
    print(f"  raw A: {', '.join(f'{v:.6g}' for v in report['raw_a'])}")
    print(f"  raw B: {', '.join(f'{v:.6g}' for v in report['raw_b'])}")
    print(
        f"  best-of-N ({report['best_of_n']['selector']}): "
        f"A={report['best_of_n']['a']:.6g} "
        f"B={report['best_of_n']['b']:.6g} "
        f"change={report['best_of_n']['change_percent']:.2f}%"
    )
    print(
        f"  medians: A={report['medians']['a']:.6g} "
        f"B={report['medians']['b']:.6g} "
        f"change={report['medians']['change_percent']:.2f}%"
    )
    if report["discarded_pairs"]:
        print("  discarded pairs:")
        for row in report["discarded_pairs"]:
            print(f"    pair {row.get('pair')}: {row.get('reason')}")


def build_parser() -> argparse.ArgumentParser:
    def common_parser(with_defaults: bool) -> argparse.ArgumentParser:
        parser = argparse.ArgumentParser(
            add_help=False,
            argument_default=None if with_defaults else argparse.SUPPRESS,
        )
        parser.add_argument("--host", default=DEFAULT_HOST if with_defaults else argparse.SUPPRESS)
        parser.add_argument("--port", type=int, default=DEFAULT_PORT if with_defaults else argparse.SUPPRESS)
        parser.add_argument("--timeout", type=float, default=120.0 if with_defaults else argparse.SUPPRESS)
        parser.add_argument("--poll", type=float, default=0.1 if with_defaults else argparse.SUPPRESS)
        parser.add_argument("--hot-top", type=int, default=64 if with_defaults else argparse.SUPPRESS)
        parser.add_argument(
            "--starv-count",
            type=int,
            default=STARV_RING_MAX_COUNT if with_defaults else argparse.SUPPRESS,
        )
        parser.add_argument(
        "--primary-metric",
        default="summary.route_wall_s" if with_defaults else argparse.SUPPRESS,
        help="dot path in each sample artifact (default: summary.route_wall_s)",
        )
        parser.add_argument(
            "--higher-is-better",
            dest="lower_is_better",
            action="store_false",
            default=True if with_defaults else argparse.SUPPRESS,
            help="interpret positive change as B higher than A",
        )
        return parser

    common = common_parser(True)
    common_no_defaults = common_parser(False)
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        parents=[common],
    )
    sub = parser.add_subparsers(dest="mode", required=True)

    sample = sub.add_parser(
        "sample",
        parents=[common_no_defaults],
        help="run one route sample against a live debug server",
    )
    sample.add_argument("--route", type=Path, required=True)
    sample.add_argument("--out", type=Path, required=True)
    sample.add_argument("--label", default="sample")
    sample.add_argument("--provenance", type=Path, help="target provenance JSON for later A/B comparability")
    sample.add_argument("--restore-slot", type=int, help="stage savestate load from this slot before sampling")
    sample.add_argument("--restore-timeout", type=float, default=10.0)
    sample.add_argument("--state-file", type=Path, help="state file to hash and store beside the restore witness")

    ab = sub.add_parser(
        "ab",
        parents=[common_no_defaults],
        help="summarize existing interleaved A/B sample artifacts",
    )
    ab.add_argument("--a", type=Path, action="append", required=True, help="baseline sample JSON; repeat >=3")
    ab.add_argument("--b", type=Path, action="append", required=True, help="candidate sample JSON; repeat >=3")
    ab.add_argument("--discard", action="append", default=[], help="discard note, e.g. 2:compiler process")
    ab.add_argument("--out", type=Path)
    ab.add_argument("--a-label", default="A")
    ab.add_argument("--b-label", default="B")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    if not math.isfinite(args.poll) or args.poll <= 0:
        parser.error("--poll must be finite and positive")
    if args.hot_top < 1 or args.hot_top > 64:
        parser.error("--hot-top must be 1..64")
    if args.starv_count < 1 or args.starv_count > STARV_RING_MAX_COUNT:
        parser.error(f"--starv-count must be 1..{STARV_RING_MAX_COUNT}")
    try:
        if args.mode == "sample":
            if not math.isfinite(args.restore_timeout) or args.restore_timeout <= 0:
                parser.error("--restore-timeout must be finite and positive")
            if args.restore_slot is not None and args.restore_slot < 0:
                parser.error("--restore-slot must be >= 0")
            if args.state_file is not None and args.restore_slot is None:
                parser.error("--state-file requires --restore-slot")
            args.target_provenance = load_target_provenance(args.provenance)
            route = load_route(args.route)
            client = Client(args.host, args.port, timeout=args.timeout)
            sample = run_sample(client, route, args, partial_out=args.out)
            write_json(args.out, sample)
            print_sample(sample)
            print(f"raw JSON -> {args.out}")
            return 0 if sample.get("success") else 3
        if args.mode == "ab":
            discards = parse_discard_specs(args.discard)
            loaded_a = [load_sample(path) for path in args.a]
            loaded_b = [load_sample(path) for path in args.b]
            kept_a, kept_b, discarded = apply_discards(loaded_a, loaded_b, discards)
            report = summarize_ab(
                kept_a,
                kept_b,
                args.primary_metric,
                args.lower_is_better,
                discarded,
            )
            report["provenance"] = base_provenance(args)
            report["labels"] = {"a": args.a_label, "b": args.b_label}
            if args.out:
                write_json(args.out, report)
            print_ab(report)
            if args.out:
                print(f"raw JSON -> {args.out}")
            return 0
        parser.error(f"unknown mode {args.mode}")
    except CampaignError as exc:
        print(f"perf_campaign: {exc}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    sys.exit(main())
