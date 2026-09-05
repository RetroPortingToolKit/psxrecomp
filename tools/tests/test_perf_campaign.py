#!/usr/bin/env python3
"""Focused tests for perf_campaign.py evidence semantics."""

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("perf_campaign", ROOT / "perf_campaign.py")
PERF = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = PERF
SPEC.loader.exec_module(PERF)


def base_reply(name, params):
    if name == "ping":
        return {"id": 1, "ok": True, "frame": 100, "dispatch_miss_total": 0,
                "dispatch_miss_unique": 0}
    if name == "freeze_check":
        return {"id": 1, "ok": True, "psx_cycle_count": 1_000_000,
                "frame_count": 100, "dirty_ram_insns": 10,
                "dirty_ram_blocks": 2, "dirty_ram_aborts": 0}
    if name == "overlay_loader_status":
        return {"id": 1, "ok": True, "loads": 1, "invalidations": 0,
                "revalidations": 0, "dispatch_native": 100,
                "dispatch_interp_fallback": 5, "stale_blocked": 0}
    if name == "autocompile_status":
        return {"id": 1, "ok": True, "compile": {"runs": 1, "fails": 0,
                "shard_fail_total": 0, "configured": 1, "degraded": 0}}
    if name == "dirty_ram_stats":
        return {"id": 1, "ok": True, "blocks_run": 2, "insns_run": 10,
                "native_handoffs": 1, "per_pc": []}
    if name == "kernel_bless":
        return {"id": 1, "ok": True, "entries": 0, "clean": 0,
                "mismatch": 0, "native_hits": 0}
    if name == "frame_perf":
        return {"id": 1, "ok": True, "samples": 30,
                "all": {"total_ms_avg": 16.0, "total_ms_max": 20.0}}
    if name == "phase_profile":
        return {"id": 1, "ok": True, "window_s": params["window"], "samples": 100,
                "interp_share": 0.1, "native_share": 0.2, "static_share": 0.3,
                "gpu_share": 0.1, "other_share": 0.3, "exc_share": 0.0}
    if name == "phase_hot":
        return {"id": 1, "ok": True, "set": params.get("set", "native"),
                "phase_samples_total": 10, "hash_drops": 0,
                "top": [{"addr": "0x80010000", "samples": 10}]}
    if name in ("bios_info", "game_options", "pace_state", "turbo_state", "gpu_state"):
        return {"id": 1, "ok": True, "name": name}
    if name in ("clear_input", "input_route_clear", "input_route_stop"):
        return {"id": 1, "ok": True}
    if name == "input_route_append":
        return {"id": 1, "ok": True, "steps": params["_step"]}
    if name == "input_route_start":
        return {"id": 1, "ok": True, "steps": 2, "start_frame": 100}
    if name == "input_route_status":
        return {"id": 1, "ok": True, "active": False, "steps": 2,
                "index": 2, "remaining": 0}
    if name == "starv_ring":
        return {"id": 1, "ok": True, "total": 2, "returned": 2, "entries": [
            {"seq": 0, "kind": 15, "us": 1000, "cyc": 100, "func": "0x1",
             "store_pc": "0x0", "in_exc": 0},
            {"seq": 1, "kind": 15, "us": 2000, "cyc": 200, "func": "0x2",
             "store_pc": "0x0", "in_exc": 0},
        ]}
    raise AssertionError(name)


class FakeClient:
    def __init__(self, overrides=None):
        self.overrides = overrides or {}
        self.calls = []
        self.append_count = 0
        self.sample_index = 0

    def cmd(self, name, **params):
        self.calls.append((name, params))
        if name == "input_route_append":
            self.append_count += 1
            params = dict(params)
            params["_step"] = self.append_count
        if name in self.overrides:
            value = self.overrides[name]
            if callable(value):
                return value(name, params)
            return value
        reply = base_reply(name, params)
        if name == "freeze_check":
            self.sample_index += 1
            reply = dict(reply)
            reply["psx_cycle_count"] += self.sample_index * 1_000_000
            reply["frame_count"] += self.sample_index * 60
            reply["dirty_ram_insns"] += self.sample_index * 10
            reply["dirty_ram_blocks"] += self.sample_index
        elif name in ("ping", "overlay_loader_status", "dirty_ram_stats"):
            reply = json.loads(json.dumps(reply))
            bump = self.sample_index
            if name == "ping":
                reply["frame"] += bump * 60
            if name == "overlay_loader_status":
                reply["dispatch_native"] += bump * 100
            if name == "dirty_ram_stats":
                reply["insns_run"] += bump * 10
                reply["blocks_run"] += bump
        elif name == "phase_hot":
            reply = json.loads(json.dumps(reply))
            reply["phase_samples_total"] += self.sample_index * 10
            reply["top"][0]["samples"] += self.sample_index * 10
        return reply


class FakeSocket:
    def __init__(self, reply):
        self.reply = reply
        self.closed = False

    def sendall(self, _body):
        pass

    def recv(self, _size):
        if self.reply is None:
            return b""
        data = self.reply
        self.reply = None
        return data

    def close(self):
        self.closed = True


class PerfCampaignTests(unittest.TestCase):
    def test_subparser_does_not_reset_root_common_options(self):
        args = PERF.build_parser().parse_args([
            "--port", "4680", "sample", "--route", "r.json", "--out", "o.json"])
        self.assertEqual(args.port, 4680)

    def test_subparser_can_override_root_common_options(self):
        args = PERF.build_parser().parse_args([
            "--port", "4370", "sample", "--port", "4680",
            "--route", "r.json", "--out", "o.json"])
        self.assertEqual(args.port, 4680)

    def test_client_rejects_response_id_mismatch(self):
        with mock.patch.object(PERF.DEBUG_CLIENT, "connect",
                               return_value=FakeSocket(b'{"id":99,"ok":true}')):
            reply = PERF.Client("127.0.0.1", 1).cmd("ping")
        self.assertFalse(reply["ok"])
        self.assertIn("response id mismatch", reply["error"])

    def test_load_route_validates_and_hashes_json(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "route.json"
            path.write_text(json.dumps({"name": "crabby", "steps": [
                {"frames": 2, "buttons": "0xFFFF"},
                {"frames": 3, "buttons": 0xBFFF},
            ]}), encoding="utf-8")
            route = PERF.load_route(path)
        self.assertEqual(route["name"], "crabby")
        self.assertEqual(route["total_frames"], 5)
        self.assertEqual(route["steps"][1]["buttons"], 0xBFFF)
        self.assertEqual(len(route["sha256"]), 64)

    def test_analog_route_fails_closed_because_native_route_is_digital_only(self):
        step = PERF.RouteStep(frames=1, buttons=0xFFFF, lx=0)
        with self.assertRaisesRegex(PERF.CampaignError, "digital-only"):
            step.append_params()

    def test_snapshot_marks_frame_perf_unavailable_when_optional_command_fails(self):
        client = FakeClient({"frame_perf": {"id": 1, "ok": False, "error": "no samples"}})
        snap = PERF.snapshot(client, phase_window=5, hot_top=10)
        self.assertNotIn("frame_perf", snap["commands"])
        self.assertIn("optional frame_perf unavailable", snap["warnings"][0])

    def test_snapshot_keeps_optional_provenance_warnings(self):
        client = FakeClient({"bios_info": {"id": 1, "ok": False, "error": "missing"}})
        snap = PERF.snapshot(client, phase_window=5, hot_top=10)
        self.assertIn("optional bios_info unavailable", snap["warnings"][0])

    def test_counter_reset_fails_closed(self):
        before = PERF.snapshot(FakeClient(), phase_window=5, hot_top=10)
        after = PERF.snapshot(FakeClient(), phase_window=5, hot_top=10)
        after["commands"]["dirty_ram_stats"]["insns_run"] = 0
        with self.assertRaisesRegex(PERF.CampaignError, "counter reset"):
            PERF.summarize_window(before, after)

    def test_missing_counter_fails_closed(self):
        before = PERF.snapshot(FakeClient(), phase_window=5, hot_top=10)
        after = PERF.snapshot(FakeClient(), phase_window=5, hot_top=10)
        del after["commands"]["freeze_check"]["psx_cycle_count"]
        with self.assertRaisesRegex(PERF.CampaignError, "missing field"):
            PERF.summarize_window(before, after)

    def test_run_sample_records_raw_evidence_and_summary(self):
        args = argparse_like(timeout=10.0, poll=0.0, hot_top=10, starv_count=4,
                             label="A1", primary_metric="summary.route_wall_s",
                             lower_is_better=True, host="127.0.0.1", port=4370)
        route = {"name": "unit", "steps": [
            {"frames": 1, "buttons": 0xFFFF, "lx": None, "ly": None, "rx": None, "ry": None},
            {"frames": 1, "buttons": 0xBFFF, "lx": None, "ly": None, "rx": None, "ry": None},
        ], "total_frames": 2}
        with mock.patch.object(PERF.time, "monotonic", side_effect=monotonic_clock()):
            sample = PERF.run_sample(FakeClient(), route, args)
        self.assertTrue(sample["success"], sample.get("error"))
        self.assertIn("before", sample)
        self.assertIn("after", sample)
        self.assertGreater(sample["summary"]["cycle_delta"], 0)
        self.assertGreaterEqual(sample["summary"]["route_wall_s"], 0.0)
        self.assertIn("observed_counter_window_wall_s", sample["summary"])
        self.assertIn("debug-tools TCP", sample["limitations"][0])

    def test_run_sample_keeps_partial_report_on_failure(self):
        args = argparse_like(timeout=10.0, poll=0.0, hot_top=10, starv_count=4,
                             label="A1", primary_metric="summary.route_wall_s",
                             lower_is_better=True, host="127.0.0.1", port=4370)
        route = {"name": "unit", "steps": [
            {"frames": 1, "buttons": 0xFFFF, "lx": 0, "ly": None, "rx": None, "ry": None},
        ], "total_frames": 1}
        sample = PERF.run_sample(FakeClient(), route, args)
        self.assertFalse(sample["success"])
        self.assertIn("digital-only", sample["error"])
        self.assertIn("partial_after", sample)

    def test_run_sample_allows_missing_frame_perf(self):
        args = argparse_like(timeout=10.0, poll=0.0, hot_top=10, starv_count=4,
                             label="A1", primary_metric="summary.route_wall_s",
                             lower_is_better=True, host="127.0.0.1", port=4370)
        route = {"name": "unit", "steps": [
            {"frames": 1, "buttons": 0xFFFF, "lx": None, "ly": None, "rx": None, "ry": None},
            {"frames": 1, "buttons": 0xBFFF, "lx": None, "ly": None, "rx": None, "ry": None},
        ], "total_frames": 2}
        with mock.patch.object(PERF.time, "monotonic", side_effect=monotonic_clock()):
            sample = PERF.run_sample(
                FakeClient({"frame_perf": {"id": 1, "ok": False, "error": "disabled"}}),
                route, args)
        self.assertTrue(sample["success"], sample.get("error"))
        self.assertFalse(sample["summary"]["frame_perf_available"])
        self.assertIn("optional frame_perf unavailable", "\n".join(sample["warnings"]))

    def test_route_done_requires_all_steps_consumed(self):
        client = FakeClient({"input_route_status": {"id": 1, "ok": True,
                             "active": False, "steps": 2, "index": 1,
                             "remaining": 0}})
        with self.assertRaisesRegex(PERF.CampaignError, "before consuming"):
            PERF.wait_route_done(client, timeout_s=1.0, poll_s=0.01, expected_steps=2)

    def test_restore_wait_requires_completion_generation_and_success(self):
        class RestoreClient:
            def __init__(self):
                self.statuses = [
                    {"id": 1, "ok": True, "generation": 1, "pending": 0,
                     "last_ok": 1, "last_op": "save", "last_slot": 0},
                    {"id": 3, "ok": True, "generation": 2, "pending": 0,
                     "last_ok": 1, "last_op": "load", "last_slot": 2},
                ]

            def cmd(self, name, **params):
                if name == "savestate_status":
                    return self.statuses.pop(0)
                if name == "savestate":
                    self.staged = params
                    return {"id": 2, "ok": True, "op": "load", "slot": 2}
                raise AssertionError(name)

        client = RestoreClient()
        witness = PERF.wait_restore_done(client, slot=2, timeout_s=1.0, poll_s=0.01)
        self.assertEqual(client.staged, {"op": "load", "slot": 2})
        self.assertEqual(witness["after"]["generation"], 2)

    def test_state_file_hash_must_match_route_manifest(self):
        args = argparse_like(timeout=10.0, poll=0.0, hot_top=10, starv_count=4,
                             label="A1", primary_metric="summary.route_wall_s",
                             lower_is_better=True, host="127.0.0.1", port=4370,
                             restore_slot=1, restore_timeout=1.0)
        with tempfile.TemporaryDirectory() as td:
            state = Path(td) / "state.pst"
            state.write_bytes(b"state")
            args.state_file = state
            route = {"name": "unit", "state_sha256": "wrong", "steps": [
                {"frames": 1, "buttons": 0xFFFF, "lx": None, "ly": None, "rx": None, "ry": None},
            ], "total_frames": 1}
            client = FakeClient()
            sample = PERF.run_sample(client, route, args)
        self.assertFalse(sample["success"])
        self.assertIn("state_sha256", sample["error"])
        self.assertEqual(client.calls, [])

    def test_ab_requires_three_pairs(self):
        sample = successful_sample(1.0)
        with self.assertRaisesRegex(PERF.CampaignError, "at least three"):
            PERF.summarize_ab([sample, sample], [sample, sample], "summary.route_wall_s", True)

    def test_ab_reports_min_median_and_interleaving(self):
        a = [successful_sample(v) for v in (10.0, 12.0, 11.0)]
        b = [successful_sample(v) for v in (9.0, 9.5, 10.0)]
        stamp_interleaved(a, b)
        report = PERF.summarize_ab(a, b, "summary.route_wall_s", True)
        self.assertEqual(report["accepted_pairs"], 3)
        self.assertEqual(report["declared_pair_order"], ["A1", "B1", "A2", "B2", "A3", "B3"])
        self.assertEqual(report["interleaving_proof"]["status"], "proven")
        self.assertAlmostEqual(report["best_of_n"]["change_percent"], 10.0)
        self.assertAlmostEqual(report["medians"]["change_percent"], (11.0 - 9.5) / 11.0 * 100.0)

    def test_metric_path_must_exist(self):
        with self.assertRaisesRegex(PERF.CampaignError, "missing metric"):
            PERF.metric_from_sample(successful_sample(1.0), "summary.nope")

    def test_ab_rejects_mismatched_route_identity(self):
        a = [successful_sample(v) for v in (1.0, 2.0, 3.0)]
        b = [successful_sample(v) for v in (1.0, 2.0, 3.0)]
        b[1]["provenance"]["route"]["sha256"] = "different"
        with self.assertRaisesRegex(PERF.CampaignError, "route identity differs"):
            PERF.summarize_ab(a, b, "summary.route_wall_s", True)

    def test_ab_rejects_non_finite_metric(self):
        sample = successful_sample(float("nan"))
        with self.assertRaisesRegex(PERF.CampaignError, "not finite"):
            PERF.metric_from_sample(sample, "summary.route_wall_s")

    def test_ab_rejects_unproven_interleaving(self):
        a = [successful_sample(v) for v in (1.0, 2.0, 3.0)]
        b = [successful_sample(v) for v in (1.0, 2.0, 3.0)]
        with self.assertRaisesRegex(PERF.CampaignError, "interleaving not proven"):
            PERF.summarize_ab(a, b, "summary.route_wall_s", True)

    def test_ab_rejects_missing_target_provenance(self):
        a = [successful_sample(v) for v in (1.0, 2.0, 3.0)]
        b = [successful_sample(v) for v in (1.0, 2.0, 3.0)]
        stamp_interleaved(a, b)
        del b[0]["provenance"]["target"]
        with self.assertRaisesRegex(PERF.CampaignError, "target"):
            PERF.summarize_ab(a, b, "summary.route_wall_s", True)

    def test_ab_allows_different_target_between_sides_but_not_within_side(self):
        a = [successful_sample(v) for v in (1.0, 2.0, 3.0)]
        b = [successful_sample(v) for v in (1.0, 2.0, 3.0)]
        for sample in b:
            sample["provenance"]["target"]["artifact_sha256"] = sha("b")
            sample["provenance"]["target"]["variant_selector"] = "candidate"
        stamp_interleaved(a, b)
        report = PERF.summarize_ab(a, b, "summary.route_wall_s", True)
        self.assertEqual(
            report["identity"]["sides"]["B"]["target_binary"]["artifact_sha256"],
            sha("b"))
        b[2]["provenance"]["target"]["artifact_sha256"] = sha("c")
        with self.assertRaisesRegex(PERF.CampaignError, "within side B"):
            PERF.summarize_ab(a, b, "summary.route_wall_s", True)

    def test_discards_exclude_pairs_before_minimum_rule(self):
        a = [successful_sample(v) for v in (10.0, 100.0, 12.0, 11.0)]
        b = [successful_sample(v) for v in (9.0, 100.0, 9.5, 10.0)]
        stamp_interleaved(a, b)
        kept_a, kept_b, discarded = PERF.apply_discards(
            a, b, [{"pair": 2, "reason": "compiler"}])
        report = PERF.summarize_ab(kept_a, kept_b, "summary.route_wall_s", True, discarded)
        self.assertEqual(report["accepted_pairs"], 3)
        self.assertEqual(report["raw_a"], [10.0, 12.0, 11.0])
        self.assertEqual(report["discarded_pairs"][0]["a"], None)

    def test_load_target_provenance_requires_explicit_keys(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "target.json"
            path.write_text(json.dumps(target_provenance()), encoding="utf-8")
            loaded = PERF.load_target_provenance(path)
            self.assertEqual(loaded["artifact_sha256"], sha("a"))
            self.assertEqual(len(loaded["source_sha256"]), 64)
            bad = Path(td) / "bad.json"
            bad.write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(PERF.CampaignError, "missing required keys"):
                PERF.load_target_provenance(bad)

    def test_target_provenance_rejects_empty_or_malformed_values(self):
        target = target_provenance()
        target["artifact_sha256"] = ""
        with self.assertRaisesRegex(PERF.CampaignError, "artifact_sha256"):
            PERF.validate_target_shape(target, "target")
        target = target_provenance()
        target["host_id"] = None
        with self.assertRaisesRegex(PERF.CampaignError, "host_id"):
            PERF.validate_target_shape(target, "target")
        target = target_provenance()
        target["instrumentation"] = {}
        with self.assertRaisesRegex(PERF.CampaignError, "instrumentation"):
            PERF.validate_target_shape(target, "target")
        target = target_provenance()
        target["cpu_budget"] = {"cap": 1, "denominator": 1}
        with self.assertRaisesRegex(PERF.CampaignError, "affinity"):
            PERF.validate_target_shape(target, "target")


def argparse_like(**kwargs):
    class Obj:
        pass
    obj = Obj()
    for key, value in kwargs.items():
        setattr(obj, key, value)
    return obj


def successful_sample(value):
    return {
        "success": True,
        "label": "x",
        "primary_metric": {"name": "summary.route_wall_s", "lower_is_better": True},
        "provenance": {
            "tcp": {"host": "127.0.0.1", "port": 4370},
            "repo": {"commit": "tools", "branch": "b", "dirty": False},
            "target": target_provenance(),
            "route": {"name": "unit", "sha256": "abc", "total_frames": 2,
                      "state_sha256": "state", "restore_witness": {"generation": 1}},
        },
        "summary": {
            "route_wall_s": value,
            "route_start_wall": 0.0,
            "route_end_wall": 0.5,
        },
    }


def target_provenance():
    return {
        "artifact_sha256": sha("a"),
        "settings_sha256": sha("1"),
        "bios_sha256": sha("b"),
        "overlay_inventory_sha256": sha("0"),
        "host_id": "host",
        "cpu_budget": {"cap": 1, "denominator": 1, "affinity": "all"},
        "instrumentation": {"debug_tcp": True, "gl_perf": True},
        "variant_selector": "baseline",
    }


def monotonic_clock(start=1000.0, step=0.01):
    current = start
    while True:
        current += step
        yield current


def sha(seed):
    return (seed * 64)[:64]


def stamp_interleaved(a, b):
    t = 0.0
    for pair_a, pair_b in zip(a, b):
        pair_a["summary"]["route_start_wall"] = t
        pair_a["summary"]["route_end_wall"] = t + 0.4
        t += 1.0
        pair_b["summary"]["route_start_wall"] = t
        pair_b["summary"]["route_end_wall"] = t + 0.4
        t += 1.0


if __name__ == "__main__":
    unittest.main()
