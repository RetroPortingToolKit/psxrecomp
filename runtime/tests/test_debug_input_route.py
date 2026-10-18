"""Exercise the actual debug route handlers/getters without SDL or a game.

Extract the unchanged function bodies from debug_server.c; stub only JSON and
responses. Live game qualification must additionally check native pad input.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    found = re.search(r"^(?:static )?(?:int|void) " + name + r"\([^\n]*\)\n\{.*?^\}",
                      source, re.M | re.S)
    if not found:
        raise ValueError("Missing source function: " + name)
    return found[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    args = parser.parse_args()
    source = (Path(__file__).resolve().parents[1] / 'src/debug_server.c').read_text()
    declarations = source.split('#define INPUT_ROUTE_MAX_STEPS', 1)[1].split(
        'static uint64_t s_input_route_start_frame = 0;', 1)[0]
    preamble = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define PSX_BSS
static uint64_t s_frame_count;
static int s_input_override=-1,s_input_override_p2=-1;
static int s_input_frames,s_input_frames_p2,s_axis_override,s_pad_type_override=-1;
static unsigned char s_axis_st[4],s_host_axes[4],s_host_lt,s_host_rt;
static int s_host_layer;
static uint16_t s_host_buttons;
static char response[512];
static void send_ok(int id){(void)id;strcpy(response,"ok");}
static void send_err(int id,const char *text){(void)id;snprintf(response,sizeof response,"ERROR:%s",text);}
static void send_fmt(const char *format,...){va_list v;va_start(v,format);vsnprintf(response,sizeof response,format,v);va_end(v);}
static int json_get_int(const char *json,const char *key,int fallback){
    char name[80];snprintf(name,sizeof name,"\"%s\"",key);
    const char *p=strstr(json,name);if(!p)return fallback;
    p=strchr(p,':');assert(p);return atoi(p+1);
}
'''
    body = '\n'.join(function(source, name) for name in (
        'handle_clear_input', 'handle_input_route_clear', 'handle_input_route_append',
        'handle_input_route_start', 'handle_input_route_stop', 'handle_input_route_status',
        'input_route_next', 'debug_server_get_input_override',
        'debug_server_get_input_override_port2'))
    checks = r'''
static void queue(void){
    handle_input_route_clear(1,"{}");
    handle_input_route_append(1,"{\"frames\":2,\"buttons\":32767}");
    handle_input_route_append(1,"{\"frames\":1,\"buttons\":65535}");
}
int main(void){
    queue();s_input_override=123;s_input_override_p2=456;
    handle_input_route_start(1,"{\"port\":2,\"at_frame\":10}");
    assert(strstr(response,"\"port\":2"));
    assert(s_input_override==123&&s_input_override_p2==-1);
    s_frame_count=9;
    assert(debug_server_get_input_override_port2()==-1);
    assert(s_input_route_remaining==2);
    s_frame_count=10;
    assert(debug_server_get_input_override()==123);
    assert(s_input_route_remaining==2);
    assert(debug_server_get_input_override_port2()==32767);
    assert(s_input_route_remaining==1);
    ++s_frame_count;
    assert(debug_server_get_input_override()==123);
    assert(debug_server_get_input_override_port2()==32767);
    assert(s_input_route_index==1&&s_input_route_remaining==1);
    ++s_frame_count;
    assert(debug_server_get_input_override_port2()==65535);
    assert(!s_input_route_active);
    assert(debug_server_get_input_override_port2()==-1);
    queue();handle_input_route_start(1,"{\"port\":2}");
    handle_clear_input(1,"{\"port\":1}");assert(s_input_route_active);
    handle_clear_input(1,"{\"port\":2}");assert(!s_input_route_active);
    queue();handle_input_route_start(1,"{}");
    assert(s_input_route_port==1);s_input_override_p2=456;
    assert(debug_server_get_input_override_port2()==456&&s_input_route_remaining==2);
    assert(debug_server_get_input_override()==32767);
    handle_clear_input(1,"{\"port\":2}");assert(s_input_route_active);
    handle_input_route_status(1,"{}");assert(strstr(response,"\"port\":1"));
    handle_input_route_append(1,"{\"frames\":1,\"buttons\":0}");
    assert(strstr(response,"ERROR"));
    handle_input_route_stop(1,"{}");assert(!s_input_route_active);
    handle_input_route_start(1,"{\"port\":3}");
    assert(strstr(response,"ERROR")&&!s_input_route_active);
    handle_input_route_start(1,"{\"port\":2}");
    handle_clear_input(1,"{}");assert(!s_input_route_active);
    handle_input_route_clear(1,"{}");handle_input_route_start(1,"{}");
    assert(strstr(response,"empty"));
    puts("debug input route: P2 timing, isolation, deferred start, clearing and legacy P1 pass");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='psx-input-route-') as directory:
        path = Path(directory) / 'route.c'
        path.write_text(preamble + '\n#define INPUT_ROUTE_MAX_STEPS' + declarations +
                        'static uint64_t s_input_route_start_frame = 0;\n' + body + checks)
        executable = Path(directory) / 'route.exe'
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(path),
                        '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
