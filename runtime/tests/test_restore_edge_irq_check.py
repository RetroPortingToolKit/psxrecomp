#!/usr/bin/env python3
"""A state restore re-runs the interrupt check of the block edge it resumes at.

Snapshots (savestate, rewind, selfcheck, rollback ring) are taken by polls
inside psx_check_interrupts, and the live run then evaluates IRQ delivery at
that same edge. A pending VBlank at a tick boundary is delivered there. The
resumed run must check that edge before the block runs, or it takes the IRQ
one block later at a different PC, and a rollback replay differs from the
live run of the same tick (psxrecomp #636: EPC and the TCB in RAM).

Guards: every psx_scheduler_resume_at arms the check; the scheduler consumes
it on the RESUME_CURRENT path with the same deduped dispatch-entry check that
compiled entries run, before psx_dispatch; the recovery path that sets
RESUME_CURRENT directly does not arm it. Also guards the depth24 present hold
(#636): it counts down at guest VBlank, and a rollback snap load keeps the
restored value instead of zeroing it.
"""
from pathlib import Path

from test_netplay_load_apply_flow import function_body

SRC = Path(__file__).resolve().parents[1] / "src"


def main():
    traps = (SRC / "traps.c").read_text()
    resume = function_body(traps, "void psx_scheduler_resume_at(uint32_t resume_pc)")
    assert "g_sched_restore_edge_check = 1;" in resume
    assert resume.index("g_sched_restore_edge_check = 1;") < resume.index("longjmp(")
    loop_at = traps.index("if (from_top_resume) {")
    loop = traps[loop_at:traps.index("psx_dispatch(cpu, run_pc);", loop_at)]
    assert "if (g_sched_restore_edge_check) {" in loop
    assert "g_sched_restore_edge_check = 0;" in loop
    assert "psx_check_interrupts_dispatch_entry(cpu, run_pc);" in loop
    # Armed only by resume_at (all restores), never by the recovery path.
    assert traps.count("g_sched_restore_edge_check = 1;") == 1
    print("restore edge irq check: ok")


if __name__ == "__main__":
    main()
