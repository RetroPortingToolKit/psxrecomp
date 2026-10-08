"""Execute production screen-tag guards without a retail game or GPU context."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_netplay_load_apply_flow import function_body


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    runtime = Path(__file__).resolve().parents[1]
    source = (runtime / "src/gpu.c").read_text(encoding="utf-8")
    start = source.index("typedef struct {\n    uint32_t key, stamp;")
    declaration = source[start:source.index("} WsTag;", start) + len("} WsTag;")]
    signatures = [
        "static int gp0_command_word_count(uint8_t opcode)",
        "static int ws_hud_command_words(uint32_t command_addr, uint32_t *words,\n                                uint32_t *out_count)",
        "static WsTag *ws_tag_screen_packet(uint32_t prim, int32_t anchor,\n                                 int32_t period, int32_t source_width)",
        "void gpu_ws_tag_tiled_strip(uint32_t prim, int32_t anchor,\n                            int32_t period, int32_t source_width)",
        "void gpu_ws_tag_screen_prim(uint32_t prim, int32_t anchor)",
        "static int ws_screen_packet_matches(const WsTag *tag)",
        "void gpu_ws_tag_stretched_prim(uint32_t prim, int32_t left, int32_t right,\n                               int32_t left_anchor, int32_t right_anchor)",
        "static const WsTag *ws_screen_tag(void)",
        "static void ws_screen_transform_quad(int32_t vx[4])",
    ]
    functions = "\n".join(signature + " {" + function_body(source, signature) + "}\n"
                          for signature in signatures)
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "ws_prepass_guard.h"
#define WS_TAG_BUCKETS 4096
#define WS_TAG_PROBES 8
#define GPU_RAM_KEY(a) ((a) & 0x1ffffcu)
static uint32_t ram[0x200000 / 4], reads;
static uint64_t s_frame_count;
static int engaged = 1, active = 1, ws_mode = 1;
static uint32_t gp0_cmd_source_addr, gp0_cmd_buf[16];
static int gp0_words_needed = 5;
static int ws_engaged(void) { return engaged; }
static int ws_active(void) { return active; }
static uint32_t psx_ram_live_bytes(void) { return sizeof(ram); }
static uint32_t psx_read_word(uint32_t address) {
    uint32_t phys = address & 0x1fffffffu;
    assert(!(phys & 3) && phys < sizeof(ram));
    reads++;
    return ram[phys / 4];
}
static int32_t ws_scale_about(int32_t x, int32_t anchor) {
    return anchor + (x - anchor) * 3 / 4;
}
''' + declaration + "\nstatic WsTag ws_tags[WS_TAG_BUCKETS];\n" + functions + r'''
int main(void) {
    const uint32_t prim = 0x80001000u;
    const uint32_t packet[] = {0x280000ffu, 0, 320, 0x00f00000u, 0x00f00140u};
    memcpy(&ram[0x1004 / 4], packet, sizeof(packet));
    memcpy(gp0_cmd_buf, packet, sizeof(packet));
    gp0_cmd_source_addr = prim + 4;
    int32_t x[] = {0, 320, 0, 320};
    ws_screen_transform_quad(x);
    assert(x[0] == 0 && x[1] == 320); /* untagged/default identity */
    gpu_ws_tag_screen_prim(prim, 160);
    assert(ws_screen_tag());
    ws_screen_transform_quad(x);
    assert(x[0] == 40 && x[1] == 280);
    active = 0;
    assert(!ws_screen_tag()); /* 4:3 / pure-2D identity */
    active = 1; ws_mode = 2;
    assert(!ws_screen_tag()); /* native-wide uses its existing tag path */
    ws_mode = 1;
    gp0_cmd_buf[1]++;
    assert(!ws_screen_tag()); /* address reused with a different packet */
    gp0_cmd_buf[1]--;
    s_frame_count = 3;
    assert(!ws_screen_tag()); /* stale tag */
    s_frame_count = 0;
    uint32_t old_reads = reads;
    gpu_ws_tag_screen_prim(prim + 1, 160);
    gpu_ws_tag_screen_prim(UINT32_MAX, 160);
    gpu_ws_tag_screen_prim(0x1f801810u, 160);
    assert(reads == old_reads); /* reject before touching invalid memory */
    ram[(sizeof(ram) - 4) / 4] = packet[0];
    gpu_ws_tag_screen_prim(0x801ffff8u, 160); /* complete packet would overrun */
    assert(reads == old_reads + 1);
    const WsTag *tag = ws_screen_tag();
    assert(tag);
    WsTag before = *tag;
    engaged = 0;
    gpu_ws_tag_stretched_prim(prim, 0, 320, 0, 320);
    assert(!memcmp(&before, tag, sizeof(before))); /* failed insertion cannot mutate stale slot */
    engaged = 1;
    gpu_ws_tag_tiled_strip(prim, 160, 1, 256);
    assert(!memcmp(&before, tag, sizeof(before)));
    gpu_ws_tag_stretched_prim(prim, 0, 320, 0, 320);
    assert(ws_screen_tag()->stretch_right == 320);
    gpu_ws_tag_screen_prim(prim, 160);
    assert(!ws_screen_tag()->stretch_right); /* retag clears stretch metadata */
    assert(!memcmp(packet, &ram[0x1004 / 4], sizeof(packet))); /* guest packet unchanged */
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="ws-screen-tags-") as directory:
        path = Path(directory)
        (path / "test.c").write_text(fixture, encoding="utf-8")
        subprocess.run([args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(runtime / "include"), str(path / "test.c"),
                        "-o", str(path / "test.exe")], check=True)
        subprocess.run([str(path / "test.exe")], check=True)
    print("production screen tags: guarded reads, freshness, mode isolation and no guest writes PASS")


if __name__ == "__main__":
    main()
