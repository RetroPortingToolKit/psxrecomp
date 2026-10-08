#!/usr/bin/env python3
"""Guard the mixed-hash LOAD reuse decision at its runtime call site."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:pos]
    raise AssertionError(f"unterminated function: {signature}")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    netplay = (ROOT / "runtime/src/psx_netplay.c").read_text()
    savestate = (ROOT / "runtime/src/savestate.c").read_text()

    apply_state = function_body(netplay, "static void np_apply_ready_state(void)\n{")
    reuse = apply_state.index("psx_netplay_load_probe_can_reuse")
    stage_blob = apply_state.index("savestate_request_load_blob_protocol(data, size)")
    begin_apply = apply_state.index("np_begin_load_apply((int)slot)")
    require(reuse < stage_blob < begin_apply,
            "verified reuse must run before staging and starting a blob restore")
    for evidence in (
        "g_np.xfer == NP_XFER_LOAD_READY",
        "g_np.load_applied_local",
        "rnet_checksum((const rnet_u8 *)data, size)",
        "np_slot_crc((int)slot, &local_size, &local_crc)",
        "savestate_write_slot((int)slot, data, size)",
        "rnet_session_state_finish(g_np.session, 0)",
        "return; /* preserve LOAD_READY and its existing apply receipt */",
    ):
        require(evidence in apply_state, f"missing reuse guard evidence: {evidence}")

    guest_probe = function_body(netplay, "static void np_guest_handle_probe(void)\n{")
    record = guest_probe.index("psx_netplay_load_probe_record")
    local_begin = guest_probe.index("np_begin_load_apply((int)slot)")
    require(local_begin < record,
            "record the matched local checkpoint only after the apply stage is reset")

    enter_ready = function_body(netplay, "static void np_enter_load_ready(int slot)\n{")
    require("psx_netplay_load_probe_mark_applied(&g_np.load_probe, slot)" in enter_ready,
            "a local match becomes reusable only after LOAD_READY")
    begin = function_body(netplay, "static void np_begin_load_apply(int slot)\n{")
    require("psx_netplay_load_probe_clear(&g_np.load_probe)" in begin,
            "a new state restore invalidates the earlier local apply receipt")

    for stage in ("before_apply", "after_restore", "before_resume"):
        require(f'savestate_load_trace("{stage}"' in savestate,
                f"missing opt-in load digest stage: {stage}")
    for partition in ("cpu", "clk", "timers", "ram", "dirty", "av", "cd", "spu", "mdec", "aux"):
        require(f"{partition}=%08x" in savestate,
                f"missing load digest partition: {partition}")
    require('getenv("PSX_NETPLAY_LOAD_TRACE")' in savestate,
            "digest tracing must be opt-in")
    print("mixed-hash LOAD apply flow and opt-in digest instrumentation: PASS")


if __name__ == "__main__":
    main()
