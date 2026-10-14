#!/usr/bin/env python3
"""The depth24 present hold is guest state, counted down at guest VBlank.

GP1(07h) arms s_d24_present_hold (skip a few Swaps after a depth24 band
retarget). It is in the GPU snap. It used to count down only at host present,
so a headless run never moved it, and a rollback snap load zeroed it: the
replayed save of a tick differed from the live save in the GPU section
(psxrecomp #636). Guards: gpu_vblank_tick counts it down, the present-side
query never touches it, and a rollback snap load keeps the restored value.
"""
from pathlib import Path

from test_netplay_load_apply_flow import function_body

SRC = Path(__file__).resolve().parents[1] / "src"


def main():
    gpu = (SRC / "gpu.c").read_text()
    vblank = function_body(gpu, "void gpu_vblank_tick(void)")
    assert "s_d24_present_hold--" in vblank
    tick = function_body(gpu, "int gpu_depth24_present_hold_tick(void)")
    assert "s_d24_present_hold" not in tick, "the hold must not move at host present"

    main_cpp = (SRC / "main.cpp").read_text()
    rb_loaded = function_body(main_cpp, 'extern "C" void psx_frontend_on_rb_snap_loaded(void)')
    assert "gpu_depth24_on_savestate_loaded" not in rb_loaded
    print("depth24 present hold: ok")


if __name__ == "__main__":
    main()
