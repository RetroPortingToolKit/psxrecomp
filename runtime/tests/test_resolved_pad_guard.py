"""Exercise the production guest reader against actual timed debug getters."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_debug_input_route import function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', required=True)
    args = parser.parse_args()
    runtime = Path(__file__).resolve().parents[1]
    debug = (runtime / 'src/debug_server.c').read_text(encoding="utf-8")
    main_source = (runtime / 'src/main.cpp').read_text(encoding="utf-8")
    reader = re.search(r'extern "C" int psx_mod_read_digital_pad\(.*?^\}',
                       main_source, re.M | re.S).group(0)
    assignment = re.search(r'^    s_digital_pad_injected_frame = .*?;',
                           main_source, re.M).group(0)
    preamble = r'''
#include <cassert>
#include <cstdio>
#include "digital_pad_snapshot.h"
struct Step {unsigned frames; unsigned buttons;};
static Step s_input_route[2]={{2,0xffdf},{1,0xffff}};
static unsigned s_input_route_port=1,s_input_route_count=2,s_input_route_index;
static unsigned s_input_route_remaining=2;
static int s_input_route_active,s_input_route_wait_load;
static uint64_t s_frame_count,s_input_route_start_frame;
static int s_input_override=-1,s_input_frames,s_axis_override,s_host_layer;
static bool s_digital_pad_injected_frame;
static int g_offline_pad_count=1;
static struct {int callback;} g_mod_controller_policy[1];
static int g_mod_controller_mode_override[1]={-1};
static bool local_mouse_live(int){return true;}
static int psx_local_mouse_installed(){return 0;}
static int sio_get_multitap(){return 0;}
static int mod_controller_source_present(unsigned){return 0;}
static int mod_pad_transform_initial_type(unsigned){return -1;}
static int sio_get_resolved_digital_pad(int,PSXResolvedDigitalPad* out){
    *out={};out->generation=1;return 1;
}
'''
    bodies = '\n'.join(function(debug, name) for name in (
        'input_route_next', 'debug_server_input_override_active',
        'debug_server_get_input_override'))
    checks = r'''
static void observe(){
    PSXModDigitalPadReader reader={};reader.struct_size=sizeof reader;
    uint32_t out=0x1234;
    for(int n=0;n<20;++n){
        assert(!psx_mod_read_digital_pad(0,&reader,0,&out));
        assert(out==0x1234&&!reader.valid);
    }
}
int main(){
    s_input_override=0xffdf;s_input_frames=2;
    observe();assert(s_input_frames==2);
    sample();observe();assert(s_input_frames==1);
    sample();assert(s_input_override==-1);observe(); // final sampled frame
    sample();assert(!s_digital_pad_injected_frame&&!debug_server_input_override_active());
    s_input_route_active=1;s_input_route_wait_load=1;
    observe();assert(s_input_route_remaining==2);
    s_input_route_wait_load=0;
    sample();observe();assert(s_input_route_remaining==1);
    sample();observe();assert(s_input_route_index==1&&s_input_route_remaining==1);
    sample();assert(!s_input_route_active);observe(); // final release frame
    sample();assert(!s_digital_pad_injected_frame);
    s_axis_override=1;observe();s_axis_override=0;
    s_host_layer=1;observe();s_host_layer=0;
    PSXModDigitalPadReader reader={};reader.struct_size=sizeof reader;
    uint32_t out=0;assert(!psx_mod_read_digital_pad(0,&reader,0,&out));
    ++s_frame_count;assert(psx_mod_read_digital_pad(0,&reader,0,&out));
    puts("resolved pad guard: timed presses/routes, last frame and repeat reads pass");
}
'''
    source = preamble + bodies + '\n' + reader + '\n' + (
        'static void sample(){int override=debug_server_get_input_override();\n' +
        assignment + '\n++s_frame_count;}\n') + checks
    with tempfile.TemporaryDirectory(prefix='psx-resolved-pad-') as directory:
        path = Path(directory) / 'guard.cpp'
        path.write_text(source)
        for lean in (False, True):
            executable = Path(directory) / ('lean.exe' if lean else 'debug.exe')
            command = [args.cxx, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                       '-I', str(runtime / 'include'), str(path), '-o', str(executable)]
            if lean:
                command.append('-DPSX_NO_DEBUG_TOOLS=1')
            subprocess.run(command, check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
