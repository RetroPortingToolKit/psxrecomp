"""Compile the production ReadN/ReadS cases against a synthetic drive.

No retail media required. Arguments: a C compiler and a private output folder.

A data read on audio-only media must answer stat|error and 40h for every
Setmode byte, the CDDA bit included. A disc with any data track keeps the data
path in every mode, and no disc keeps the plain error.
"""
from pathlib import Path
import subprocess, sys

source = (Path(__file__).parents[1] / 'src/cdrom.c').read_text(encoding='utf-8')
marker = 'static int reject_audio_disc_data_read(void) {'
helper = ''
if marker in source:
    start = source.index(marker)
    end = source.index('\nstatic void exec_command(uint8_t cmd) {', start)
    helper = source[start:end]
    assert 'mode_reg' not in helper, 'audio-disc rejection must not depend on the transfer mode'
cases = []
for case in ['    case 0x06: /* ReadN */', '    case 0x1B: /* ReadS */']:
    start = source.index(case)
    cases.append(source[start:source.index('\n    case ', start + len(case))])

harness = r'''
#include <stdint.h>
#include <stdio.h>
#define CDSTAT_ERROR 1
#define CDIRQ_ERROR 5
#define CDIRQ_ACK 3
static void *iso_handle = (void*)1;
static uint8_t mode_reg, stat_reg = 2;
static int track_count, data_track, present, bytes[8], count, irq, reads;
static int iso_track_count(void *p) { (void)p; return track_count; }
static int iso_track_is_audio(void *p, int t) { (void)p; return t != data_track; }
static int has_disc(void) { return present; }
static void response_push(int v) { bytes[count++] = v; }
static void set_irq(int v) { irq = v; }
static int read_continues_current_stream(void) { return 0; }
static void start_read_stream(int cmd) { (void)cmd; reads++; }
''' + helper + '\nstatic void execute(uint8_t cmd) { (void)mode_reg; (void)iso_handle; (void)iso_track_count; (void)iso_track_is_audio; switch(cmd) {\n' + ''.join(cases) + r'''
} }
int main(void) {
  const int commands[] = {0x06, 0x1B};
  int bad = 0;
  for (unsigned i = 0; i < 2; ++i) {
    /* 1-track and 12-track audio-only media: every mode byte errors. */
    for (int n = 1; n <= 12; n += 11) for (int mode = 0; mode < 256; ++mode) {
      track_count = n; data_track = 0; present = 1; mode_reg = (uint8_t)mode;
      count = irq = reads = 0; execute((uint8_t)commands[i]);
      if (!(reads == 0 && irq == 5 && count == 2 && bytes[0] == 3 && bytes[1] == 0x40)) {
        if (!bad++) printf("FAIL: command %02X on a %d-track audio disc, mode %02X: "
                           "read started=%d irq=%d bytes=%d\n", commands[i], n, mode, reads, irq, count);
      }
    }
    /* Every data-track position keeps the mixed-mode data path, in every mode. */
    for (int t = 1; t <= 8; ++t) for (int mode = 0; mode < 256; ++mode) {
      track_count = 8; data_track = t; present = 1; mode_reg = (uint8_t)mode;
      count = irq = reads = 0; execute((uint8_t)commands[i]);
      if (!(reads == 1 && irq == 3 && count == 1 && bytes[0] == 2)) {
        if (!bad++) printf("FAIL: command %02X with data track %d, mode %02X: no data read\n",
                           commands[i], t, mode);
      }
    }
    present = 0; count = irq = reads = 0; execute((uint8_t)commands[i]);
    if (!(reads == 0 && irq == 5 && count == 1)) {
      if (!bad++) printf("FAIL: command %02X with no disc\n", commands[i]);
    }
  }
  if (bad) { printf("%d cases failed\n", bad); return 1; }
  puts("PASS: production ReadN/ReadS audio-disc errors, all mode bytes, mixed-mode and no-disc controls");
  return 0;
}
'''
out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
fixture = out / 'audio_disc_read_fixture.c'
fixture.write_text(harness, encoding='utf-8')
exe = out / 'audio_disc_read_fixture.exe'
subprocess.run([sys.argv[1], '-std=c11', '-Wall', '-Wextra', '-Werror', str(fixture), '-o', str(exe)], check=True)
sys.exit(subprocess.run([str(exe)]).returncode)
