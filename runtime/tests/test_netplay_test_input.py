#!/usr/bin/env python3
"""Execute the production QA pad and check its authoritative publish site."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from test_netplay_load_apply_flow import function_body

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    source = (args.source / "runtime/src/main.cpp").read_text(encoding="utf-8")
    signature = "static bool netplay_test_input(PsxNetPad* out)"
    body = function_body(source, signature)
    admit = function_body(source, "static void netplay_barrier_admit(int override)")
    assert admit.index("if (need_sample)") < admit.index("if (netplay_test_input(&local))")
    assert admit.index("if (netplay_test_input(&local))") < admit.index("capture_override_pad(")
    assert admit.index("if (netplay_test_input(&local))") < admit.index("netplay_transform_local_pad(&local,") < admit.index("psx_netplay_stage_local(&local)")
    header = (args.source / "runtime/include/psx_netplay.h").read_text(encoding="utf-8")
    pad = re.search(r"typedef struct PsxNetPad \{.*?\} PsxNetPad;", header, re.S).group(0)
    fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
''' + pad + r'''
static uint32_t tick;
static uint32_t psx_netplay_sim_tick() { return tick; }
''' + signature + "\n{" + body + r'''
}
int main(int argc, char **argv) {
    const bool enabled = argc > 1;
    const uint32_t ticks[] = {0,7,8,15,16,24,48,112};
    for (uint32_t t : ticks) {
        tick = t;
        PsxNetPad pad, before;
        std::memset(&pad,0x5a,sizeof(pad));before=pad;
        assert(netplay_test_input(&pad)==enabled);
        if (!enabled) { assert(!std::memcmp(&pad,&before,sizeof(pad)));continue; }
        assert(pad.lx==0x80 && pad.ly==0x80 && pad.rx==0x80 && pad.ry==0x80);
        assert(pad.analog==1 && pad.connected==1);
        assert((pad.buttons & 0x0009)==0x0009); // never Start/Select
        PsxNetPad again{};
        assert(netplay_test_input(&again));
        assert(!std::memcmp(&pad,&again,sizeof(pad)));
        std::printf("%u ",unsigned(pad.buttons));
    }
    std::puts("");
}
'''
    vectors = {11:[65535,65535,32735,32735,65535,65535,61375,49023],
               22:[61423,61423,32735,32735,65535,49119,65535,65535]}
    env = {k:v for k,v in os.environ.items() if not k.startswith("PSX_NET_TEST_INPUT_")}
    with tempfile.TemporaryDirectory(prefix="psx-netplay-pad-") as directory:
        source_path=Path(directory)/"test.cpp";source_path.write_text(fixture,encoding="utf-8")
        for release in (False,True):
            binary=Path(directory)/("test-release.exe" if release else "test.exe")
            command=[args.compiler,"-std=c++17","-O2",str(source_path),"-o",str(binary)]
            if release:command.insert(1,"-DPSX_NO_DEBUG_TOOLS")
            subprocess.run(command,check=True)
            subprocess.run([str(binary)],env=env,check=True,capture_output=True)
            subprocess.run([str(binary)],env=dict(env,PSX_NET_TEST_INPUT_SEED="0"),check=True,capture_output=True)
            if release:
                subprocess.run([str(binary)],env=dict(env,PSX_NET_TEST_INPUT_SEED="22"),check=True,capture_output=True)
                continue
            for seed,expected in vectors.items():
                output=subprocess.check_output([str(binary),"enabled"],env=dict(env,PSX_NET_TEST_INPUT_SEED=str(seed)),text=True)
                assert [int(x) for x in output.split()]==expected,(seed,output)
            output=subprocess.check_output([str(binary),"enabled"],env=dict(env,PSX_NET_TEST_INPUT_SEED="22",PSX_NET_TEST_INPUT_FROM="8"),text=True)
            assert [int(x) for x in output.split()]==[65535,65535]+vectors[22][2:]
            output=subprocess.check_output([str(binary),"enabled"],env=dict(env,PSX_NET_TEST_INPUT_SEED="22",PSX_NET_TEST_INPUT_FROM="8",PSX_NET_TEST_INPUT_HOLD="1"),text=True)
            assert [int(x) for x in output.split()]==[65535,65535]+[49151]*6
    print("production QA input: exact published vectors, tick stability, start/hold and default/release off PASS")


if __name__ == "__main__":
    main()
