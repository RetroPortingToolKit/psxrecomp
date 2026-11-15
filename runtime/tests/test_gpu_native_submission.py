"""Compare native GP0 packet submission with the production word parser.

Reuse the existing real-GPU fixture's renderer seams. Full GPU state and VRAM
are compared after every node, including incomplete commands and transfers.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

import test_gpu_gp0_history_guard as fixture

DRIVER = r'''
#include "gpu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern uint64_t s_frame_count;
uint32_t gpu_snapshot_bytes(void);
void gpu_snapshot_write(uint8_t *state);
static FILE *trace;
static int native;
static void node(uint32_t address, const uint32_t *words, uint32_t count) {
    if (native) gpu_submit_gp0_node(address, count, words);
    else {
        gpu_set_gp0_linked_list_node(address, count);
        for (uint32_t i = 0; i < count; ++i) {
            gpu_set_gp0_source(address + 4u + i * 4u);
            gpu_write_gp0(words[i]);
        }
    }
    uint32_t size = gpu_snapshot_bytes();
    uint8_t *state = malloc(size);
    if (!state) exit(2);
    gpu_snapshot_write(state);
    if (fwrite(state, size, 1, trace) != 1) exit(3);
    free(state);
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 1024; ++x) {
            uint16_t pixel = gpu_vram_peek(x, y);
            if (fwrite(&pixel, 2, 1, trace) != 1) exit(4);
        }
    ++s_frame_count;
}
int main(int argc, char **argv) {
    if (argc != 3 || !(trace = fopen(argv[1], "wb"))) return 1;
    native = atoi(argv[2]);
    gpu_init();
    gpu_ws_census_set(0);
    gpu_ws_begin_linked_list();
    const uint32_t env[] = {0xE10003FF, 0xE3000000, 0xE4013FFF, 0xE5000000, 0xE6000001};
    const uint32_t fill[] = {0x020000FF, 0x00050010, 0x00010010};
    const uint32_t upload_header[] = {0xA0000000, 0x00090028};
    const uint32_t upload_tail[] = {0x00010002, 0x9ABC1234};
    const uint32_t copy[] = {0x80000000, 0x00090028, 0x000A002A, 0x00010002};
    const uint32_t flat[] = {0x20010203, 0x00100010, 0x00200020, 0x00300030};
    const uint32_t poly1[] = {0x4800007F, 0x0014000A, 0x0019000F};
    const uint32_t poly2[] = {0x001A0010, 0x55555555, 0xE6000000};
    const uint32_t shaded1[] = {0x5800007F, 0x0014000A, 0x00007F00};
    const uint32_t shaded2[] = {0x0019000F, 0x55555555, 0xE1000200};
    node(0x1000, env, 0);
    node(0x1010, env, 5);
    node(0x1100, fill, 3);
    node(0x1200, upload_header, 2);
    node(0x1300, upload_tail, 2);
    node(0x1400, copy, 4);
    node(0x1500, flat, 4);
    node(0x1600, flat, 1);
    node(0x1700, flat + 1, 3);
    node(0x1800, poly1, 3);
    node(0x1900, poly2, 3);
    node(0x1A00, shaded1, 3);
    node(0x1B00, shaded2, 3);
    gpu_ws_end_linked_list();
    if (fclose(trace)) return 5;
    return 0;
}
'''

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default=fixture.default_cc())
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='psx-native-gp0-') as td:
        work = Path(td)
        fixture.write_sources(work)
        (work / 'driver.c').write_text(DRIVER, encoding='utf-8')
        for production in (False, True):
            exe = fixture.build(args.cc, work, 'production' if production else 'debug', production)
            paths = [work / 'words.state', work / 'native.state']
            for native, path in enumerate(paths):
                subprocess.run([str(exe), str(path), str(native)], check=True)
            assert paths[0].read_bytes() == paths[1].read_bytes(), 'native GPU state/VRAM differs'
    print('PASS: native GP0 nodes equal word-parser GPU/VRAM state, debug and production')

if __name__ == '__main__':
    main()
