#!/usr/bin/env python3
"""Build actual audio_trace.c in debug and PSX_NO_DEBUG_TOOLS modes."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_GCC = pathlib.Path(r"C:\msys64\mingw64\bin\gcc.exe")

DRIVER_C = r"""
#include "audio_trace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PCM_RING_FRAMES (1u << 22)

static void fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t rd16s(const unsigned char *p) {
    uint16_t u = (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
    return (int16_t)u;
}

static void push_small_taps(void) {
    int16_t t0[] = {
        0, 0,
        1, -1,
        256, -256,
        257, -257,
        32767, -32768,
    };
    int16_t t1[] = { -300, 0, 0, 300, 0, 0 };
    int16_t t2[] = { 42, -42, 512, -512 };
    int16_t t3[] = { 0, 0, 999, -999, -32768, 32767 };
    audio_trace_pcm(AUDIO_TAP_SPU_OUT, t0, 5);
    audio_trace_pcm(AUDIO_TAP_CD_IN, t1, 3);
    audio_trace_pcm(AUDIO_TAP_HOST, t2, 2);
    audio_trace_pcm(AUDIO_TAP_VOICES, t3, 3);
}

static void check_small_stats(void) {
    AudioTraceStats st;
    audio_trace_get_stats(&st);
    if (st.tap_frames[0] != 5 || st.tap_nonzero[0] != 4 ||
        st.tap_audible[0] != 2 || st.tap_peak[0] != 32768)
        fail("tap0 stats");
    if (st.tap_frames[1] != 3 || st.tap_nonzero[1] != 2 ||
        st.tap_audible[1] != 2 || st.tap_peak[1] != 300)
        fail("tap1 stats");
    if (st.tap_frames[2] != 2 || st.tap_nonzero[2] != 2 ||
        st.tap_audible[2] != 1 || st.tap_peak[2] != 512)
        fail("tap2 stats");
    if (st.tap_frames[3] != 3 || st.tap_nonzero[3] != 2 ||
        st.tap_audible[3] != 2 || st.tap_peak[3] != 32768)
        fail("tap3 stats");
}

static void print_stats(const char *label) {
    AudioTraceStats st;
    audio_trace_get_stats(&st);
    printf("%s", label);
    for (int t = 0; t < AUDIO_TAP_COUNT; t++) {
        printf(" tap%d=%llu/%llu/%llu/%d/%u",
               t,
               (unsigned long long)st.tap_frames[t],
               (unsigned long long)st.tap_nonzero[t],
               (unsigned long long)st.tap_audible[t],
               st.tap_peak[t],
               audio_trace_tap_rate(t));
    }
    printf(" pump=%llu/%llu/%llu/%u/%u/%llu/%llu events=%llu total=%llu\n",
           (unsigned long long)st.pump_calls,
           (unsigned long long)st.pump_skips,
           (unsigned long long)st.underruns,
           st.queue_hiwater,
           st.queue_lowater,
           (unsigned long long)st.mute_events,
           (unsigned long long)st.unmute_events,
           (unsigned long long)st.events_total,
           (unsigned long long)audio_trace_events_total());
}

static void push_events(void) {
    audio_trace_note_frame(77);
    audio_trace_event(AUDIO_EV_RENDER, 2048, 1234);
    audio_trace_event(AUDIO_EV_PUMP_SKIP, 222, 0);
    audio_trace_event(AUDIO_EV_UNDERRUN, 3, 1);
    audio_trace_event(AUDIO_EV_MUTE, 64, 0);
    audio_trace_event(AUDIO_EV_UNMUTE, 128, 0);
    audio_trace_event(AUDIO_EV_CD_PUSH, 0xABCDEF01u, 17);
}

static void check_events(void) {
    AudioTraceStats st;
    AudioTraceEvent ev[8];
    audio_trace_get_stats(&st);
    if (st.pump_calls != 1 || st.pump_skips != 1 || st.underruns != 1 ||
        st.mute_events != 1 || st.unmute_events != 1 ||
        st.queue_hiwater != 1234 || st.queue_lowater != 1234 ||
        st.events_total != 6)
        fail("event stats");
    if (audio_trace_events_total() != 6) fail("event total");
    if (audio_trace_events_get(ev, 8) != 6) fail("event get count");
    if (ev[0].seq != 0 || ev[0].sample_idx != 5 || ev[0].frame != 77 ||
        ev[0].kind != AUDIO_EV_RENDER || ev[0].a != 2048 || ev[0].b != 1234)
        fail("render event payload");
    if (ev[5].kind != AUDIO_EV_CD_PUSH || ev[5].a != 0xABCDEF01u ||
        ev[5].b != 17)
        fail("cd event payload");
}

static void check_post_wrap_event(uint64_t expected_sample_idx) {
    AudioTraceStats st;
    AudioTraceEvent ev[8];
    audio_trace_note_frame(88);
    audio_trace_event(AUDIO_EV_DMA_READ, 12, 0x12345678u);
    audio_trace_get_stats(&st);
    if (st.events_total != 7 || audio_trace_events_total() != 7)
        fail("post-wrap event count");
    if (audio_trace_events_get(ev, 8) != 7) fail("post-wrap event get count");
    if (ev[6].seq != 6 || ev[6].sample_idx != expected_sample_idx ||
        ev[6].frame != 88 || ev[6].kind != AUDIO_EV_DMA_READ ||
        ev[6].a != 12 || ev[6].b != 0x12345678u)
        fail("post-wrap event payload");
}

static void print_events(const char *label) {
    AudioTraceEvent ev[8];
    uint32_t got = audio_trace_events_get(ev, 8);
    printf("%s count=%u", label, got);
    for (uint32_t i = 0; i < got; i++) {
        printf(" e%u=%llu/%llu/%u/%u/%u/%u",
               i,
               (unsigned long long)ev[i].seq,
               (unsigned long long)ev[i].sample_idx,
               ev[i].frame,
               ev[i].kind,
               ev[i].a,
               ev[i].b);
    }
    printf("\n");
}

static int file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

static void push_wrap_tail(void) {
    int16_t frame[2];
    for (uint32_t i = 0; i < PCM_RING_FRAMES - 5u; i++) {
        frame[0] = (int16_t)(i & 0x7FFFu);
        frame[1] = (int16_t)(-(int32_t)(i & 0x7FFFu));
        audio_trace_pcm(AUDIO_TAP_SPU_OUT, frame, 1);
    }
    for (int i = 0; i < 4; i++) {
        frame[0] = (int16_t)(1000 + i);
        frame[1] = (int16_t)(-2000 - i);
        audio_trace_pcm(AUDIO_TAP_SPU_OUT, frame, 1);
    }
}

static const int16_t *tap_expected(int tap, uint32_t *frames) {
    static const int16_t t0[] = {
        0, 0, 1, -1, 256, -256, 257, -257, 32767, -32768,
    };
    static const int16_t t1[] = { -300, 0, 0, 300, 0, 0 };
    static const int16_t t2[] = { 42, -42, 512, -512 };
    static const int16_t t3[] = { 0, 0, 999, -999, -32768, 32767 };
    switch (tap) {
    case AUDIO_TAP_SPU_OUT: *frames = 5; return t0;
    case AUDIO_TAP_CD_IN:   *frames = 3; return t1;
    case AUDIO_TAP_HOST:    *frames = 2; return t2;
    default:                *frames = 3; return t3;
    }
}

static void check_wav_samples(const char *path, uint32_t rate,
                              const int16_t *expected, uint32_t frames) {
    FILE *fp = fopen(path, "rb");
    if (!fp) fail("wav open");
    unsigned char data[44 + 64];
    size_t want = 44u + (size_t)frames * 4u;
    if (want > sizeof(data)) fail("wav test buffer too small");
    size_t n = fread(data, 1, want, fp);
    fclose(fp);
    if (n != want) fail("wav size");
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVEfmt ", 8) != 0 ||
        memcmp(data + 36, "data", 4) != 0)
        fail("wav tags");
    if (rd32(data + 24) != rate) fail("wav rate");
    if (rd32(data + 40) != frames * 4u) fail("wav data bytes");
    for (uint32_t i = 0; i < frames * 2u; i++) {
        if (rd16s(data + 44u + i * 2u) != expected[i])
            fail("wav data");
    }
}

static void check_short_wavs(const char *base_path, int production) {
    char path[512];
    for (int tap = 0; tap < AUDIO_TAP_COUNT; tap++) {
        uint32_t frames = 0;
        const int16_t *expected = tap_expected(tap, &frames);
        snprintf(path, sizeof(path), "%s_tap%d.wav", base_path, tap);
        remove(path);
        int64_t wrote = audio_trace_dump_wav(tap, path, 0, frames);
        if (production) {
            if (wrote != -1) fail("production short wav should be unavailable");
            if (file_exists(path)) fail("production short wav created file");
        } else {
            if (wrote != (int64_t)frames) fail("debug short wav frame count");
            check_wav_samples(path, audio_trace_tap_rate(tap), expected, frames);
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 3) fail("usage");
    const char *path = argv[1];
    int production = strcmp(argv[2], "production") == 0;

    audio_trace_init();
    audio_trace_set_tap_rate(AUDIO_TAP_SPU_OUT, 48000);
    audio_trace_set_tap_rate(AUDIO_TAP_CD_IN, 44101);
    audio_trace_set_tap_rate(AUDIO_TAP_HOST, 32000);
    audio_trace_set_tap_rate(AUDIO_TAP_VOICES, 22050);
    push_small_taps();
    check_small_stats();
    print_stats("small");
    push_events();
    check_events();
    print_events("events");
    check_short_wavs(path, production);
    push_wrap_tail();

    AudioTraceStats st;
    audio_trace_get_stats(&st);
    if (st.tap_frames[AUDIO_TAP_SPU_OUT] != PCM_RING_FRAMES + 4ull)
        fail("wrapped tap frame total");
    check_post_wrap_event(st.tap_frames[AUDIO_TAP_SPU_OUT]);
    print_stats("wrap");
    print_events("events_after_wrap");

    int64_t wrote = audio_trace_dump_wav(AUDIO_TAP_SPU_OUT, path,
                                         (int64_t)PCM_RING_FRAMES, 4);
    if (production) {
        if (wrote != -1) fail("production wav should be unavailable");
        if (file_exists(path)) fail("production wav created file");
    } else {
        int16_t wrapped[] = {
            1000, -2000, 1001, -2001, 1002, -2002, 1003, -2003,
        };
        if (wrote != 4) fail("debug wav frame count");
        check_wav_samples(path, 48000u, wrapped, 4);
        {
            int16_t oldest[] = { 32767, -32768 };
            if (audio_trace_dump_wav(AUDIO_TAP_SPU_OUT, path, 0, 1) != 1)
                fail("debug evicted prefix should clamp");
            check_wav_samples(path, 48000u, oldest, 1);
        }
    }

    audio_trace_init();
    audio_trace_get_stats(&st);
    for (int tap = 0; tap < AUDIO_TAP_COUNT; tap++) {
        if (st.tap_frames[tap] || st.tap_nonzero[tap] || st.tap_audible[tap] ||
            st.tap_peak[tap] || audio_trace_tap_total(tap))
            fail("reset tap");
    }
    if (st.events_total || st.pump_calls || st.pump_skips || st.underruns ||
        st.queue_hiwater || st.queue_lowater || st.mute_events ||
        st.unmute_events || audio_trace_events_total())
        fail("reset events");
    print_stats("reset");
    return 0;
}
"""


def default_cc() -> str:
    if DEFAULT_GCC.exists():
        return str(DEFAULT_GCC).replace("\\", "/")
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return str(DEFAULT_GCC).replace("\\", "/")


def normalize_cc(cc: str) -> str:
    if "\\" in cc and len(cc) >= 3 and cc[1] == ":":
        return cc.replace("\\", "/")
    return cc


def build(cc: str, work: pathlib.Path, production: bool) -> pathlib.Path:
    source = work / "audio_trace_guard_driver.c"
    source.write_text(DRIVER_C, encoding="utf-8")
    exe = work / ("audio_trace_prod.exe" if production else "audio_trace_debug.exe")
    cmd = [
        cc,
        "-std=c11",
        "-O2",
        "-fno-lto",
        "-I",
        str(ROOT / "runtime" / "include"),
    ]
    if production:
        cmd.append("-DPSX_NO_DEBUG_TOOLS=1")
    cmd.extend([
        str(source),
        str(ROOT / "runtime" / "src" / "audio_trace.c"),
        "-o",
        str(exe),
    ])
    subprocess.run(cmd, cwd=work, check=True)
    return exe


def run(exe: pathlib.Path, path: pathlib.Path, mode: str) -> str:
    return subprocess.run(
        [str(exe), str(path), mode],
        cwd=exe.parent,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=default_cc())
    args = parser.parse_args()
    cc = normalize_cc(args.cc)

    with tempfile.TemporaryDirectory(prefix="psx-audio-trace-") as td:
        work = pathlib.Path(td)
        debug = build(cc, work, False)
        prod = build(cc, work, True)
        debug_out = run(debug, work / "debug.wav", "debug")
        prod_out = run(prod, work / "prod.wav", "production")
    if debug_out != prod_out:
        raise AssertionError(f"stats/event output changed:\n{debug_out}\n{prod_out}")
    print(debug_out)
    print("PASS: audio trace PCM history is debug-tool only; stats/events match")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
    except subprocess.CalledProcessError as exc:
        print(f"FAIL: command exited {exc.returncode}: {' '.join(map(str, exc.cmd))}")
        sys.exit(1)
