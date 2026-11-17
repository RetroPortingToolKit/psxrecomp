/* Checkpoint capture at frame N and resume, through the real boot_state
 * writer and loader with the handoff test's hardware doubles: no disc, no
 * BIOS. The capture is staged at the requested frame and taken at a later
 * boundary; the resume reproduces frame, cycle, RAM and the resume sections,
 * and refuses a state that does not belong to this run. */
#define main boot_state_handoff_tests
#include "test_boot_state_handoff.c"
#undef main
#include "savestate.h"
#include "source_tas_stateio.h"
#include "tas_checkpoint.h"
#include <stdio.h>
#include <stdlib.h>

#define BIOS 123u
#define ENTRY 0x801b0000u
#define TCB_BASE 0x800F0000u

uint64_t s_frame_count;

/* savestate doubles: the boundary is driven by the test. */
static savestate_checkpoint_fn staged;
int savestate_request_checkpoint(savestate_checkpoint_fn fn) {
    if (!fn || staged) return 0;
    staged = fn;
    return 1;
}
void savestate_get_integrity(uint32_t *bios, uint32_t *entry) { *bios = BIOS; *entry = ENTRY; }

static int failures;
static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

#ifndef PSX_NO_DEBUG_TOOLS
/* The two load resyncs; one can be told to move the clock. */
static int resync_moves_clock;
void psx_cycles_resync_after_restore(CPUState *cpu) { (void)cpu; if (resync_moves_clock) ++psx_cycle_count; }
void interrupts_resync_after_restore(void) {}

/* Card 1 inserted, card 2 empty. */
static uint8_t card[2][PSX_TAS_STATEIO_CARD_BYTES];
static int card_present[2] = {1, 0};
int memcard_is_present(int c) { return c >= 0 && c < 2 && card_present[c]; }
int memcard_export_raw(int c, uint8_t *dst) {
    if (!memcard_is_present(c)) return -1;
    memcpy(dst, card[c], sizeof card[c]);
    return 0;
}

static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void seed(CPUState *cpu) {
    for (uint32_t i = 0; i < ram_bytes; i++) ram[i] = (uint8_t)(i * 7u + 3u);
    put_u32(ram + 0x110, TCB_BASE);
    put_u32(ram + 0x114, 4u * 0xC0u);
    memset(cpu, 0, sizeof *cpu);
    cpu->pc = 0x80012340u;
    cpu->gpr[29] = 0x801FFF00u;
    psx_cycle_count = 987654321u;
    fixture_return_tcb = TCB_BASE + 0xC0u;
    memset(&fixture_irq_timing, 0, sizeof fixture_irq_timing);
    fixture_irq_timing.cooldown_until = 987654000u;
    fixture_irq_timing.sio_seq_seen = 9;
    for (uint32_t i = 0; i < PSX_MEMORY_CONTROL_WORDS; i++) fixture_memctrl[i] = 0x1F000000u + i;
    for (size_t i = 0; i < sizeof card[0]; i++) card[0][i] = (uint8_t)(i ^ 0x5Au);
}

static void scramble(CPUState *cpu) {
    memset(ram, 0xEE, ram_bytes);
    memset(cpu, 0, sizeof *cpu);
    psx_cycle_count = 1;
    fixture_return_tcb = 0;
    memset(&fixture_irq_timing, 0, sizeof fixture_irq_timing);
    memset(fixture_memctrl, 0, sizeof fixture_memctrl);
    s_frame_count = 0;
}

static int resume(CPUState *cpu, const char *state, char *reason) {
    return tas_checkpoint_resume(cpu, state, NULL, reason, 160);
}

/* Refused before anything is loaded: the live state stays scrambled. */
static int refused_before_load(CPUState *cpu, const char *state, const char *needle) {
    char reason[160];
    scramble(cpu);
    return !resume(cpu, state, reason) && strstr(reason, needle) && ram[1000] == 0xEE &&
           s_frame_count == 0 && psx_cycle_count == 1;
}

static void copy_file(const char *from, const char *to, long flip_at) {
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    int c; long at = 0;
    assert(in && out);
    while ((c = fgetc(in)) != EOF) { fputc(at == flip_at ? c ^ 1 : c, out); ++at; }
    fclose(in); fclose(out);
}

int main(void) {
    static CPUState cpu, want;
    const char *state = "tas_checkpoint_test-000007.pst";
    const char *files[] = {state, "tas_checkpoint_test-000007.pst.json",
                           "tas_checkpoint_test-000007.pst.card1.mcd", "tas_checkpoint_bad.pst",
                           "tas_checkpoint_bad.pst.json", "tas_checkpoint_bad.pst.card1.mcd"};
    char reason[160];
    uint64_t digest;
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) remove(files[i]);
    set_env("PSX_TAS_SAVE_STATE_AT", "5");
    set_env("PSX_TAS_SAVE_STATE_PATH", "tas_checkpoint_test-{frame}.pst");
    seed(&cpu);

    /* Staged at the requested frame only, once. */
    tas_checkpoint_on_frame(4);
    check(!staged, "no capture is staged before the requested frame");
    tas_checkpoint_on_frame(5);
    check(staged != NULL, "a capture is staged at the requested frame");
    tas_checkpoint_on_frame(5);

    /* The boundary arrives two frames later. */
    s_frame_count = 7;
    {
        savestate_checkpoint_fn fn = staged;
        staged = NULL;
        CPUState snap = cpu;
        fn(&snap, BIOS, ENTRY);
    }
    want = cpu;
    digest = source_tas_stateio_ram_digest(ram, 2097152u);
    {
        static char text[32768];
        TasStateManifest m;
        check(source_tas_stateio_read_text("tas_checkpoint_test-000007.pst.json", text, sizeof text) &&
              source_tas_stateio_manifest_parse(text, &m, NULL, 0), "the manifest is written and parses");
        check(m.frame == 7 && m.requested_frame == 5, "the manifest names the frame taken and the one requested");
        check(m.cycle == 987654321u && m.ram_digest == digest, "the manifest holds the cycle and RAM digest");
        check(!strcmp(m.route_sha256, "none") && !strcmp(m.card_sha256[1], "none") &&
              strlen(m.card_sha256[0]) == 64, "no route; card 1 hashed, card 2 none");
    }
    check(!tas_checkpoint_capture(&cpu, BIOS, ENTRY, 7, 7, state), "an existing manifest is never overwritten");

    /* Resume reproduces the run. */
    scramble(&cpu);
    check(resume(&cpu, state, reason), "the checkpoint resumes");
    check(s_frame_count == 7 && psx_cycle_count == 987654321u, "frame and cycle are restored");
    check(source_tas_stateio_ram_digest(ram, 2097152u) == digest, "RAM is restored");
    check(cpu.pc == want.pc && cpu.gpr[29] == want.gpr[29], "the CPU is restored");
    check(fixture_return_tcb == TCB_BASE + 0xC0u && fixture_irq_timing.cooldown_until == 987654000u &&
          fixture_memctrl[3] == 0x1F000003u, "the resume sections are restored");

    /* Refusals. */
    card[0][100] ^= 1;
    check(refused_before_load(&cpu, state, "card 1 differs"), "a changed card is refused before loading");
    card[0][100] ^= 1;
    card_present[1] = 1;
    check(refused_before_load(&cpu, state, "card 2 presence"), "an extra card is refused before loading");
    card_present[1] = 0;
    set_env("PSX_TAS_CHECKPOINT_TEST_MODEL", "1");
    check(refused_before_load(&cpu, state, "configuration"), "another configuration is refused before loading");
    set_env("PSX_TAS_CHECKPOINT_TEST_MODEL", NULL);
    set_env("PSX_INPUT_ROUTE_FILE", state);
    check(refused_before_load(&cpu, state, "input route"), "another input route is refused before loading");
    set_env("PSX_INPUT_ROUTE_FILE", NULL);
    copy_file(state, "tas_checkpoint_bad.pst", 40);
    copy_file("tas_checkpoint_test-000007.pst.json", "tas_checkpoint_bad.pst.json", -1);
    check(!tas_checkpoint_resume(&cpu, "tas_checkpoint_bad.pst", "tas_checkpoint_test-000007.pst.json",
                                 reason, sizeof reason) && strstr(reason, "state file"),
          "a state that is not the manifest's is refused");
    resync_moves_clock = 1;
    check(!resume(&cpu, state, reason) && strstr(reason, "cycle mismatch"),
          "a resume that does not reproduce the cycle is refused");
    resync_moves_clock = 0;
    set_env("PSX_TAS_PERTURB_RESTORE", "pc");
    check(refused_before_load(&cpu, state, "PERTURB"), "an unknown perturbation is refused before loading");

    /* A perturbation changes one restored value after an accepted resume. */
    set_env("PSX_TAS_PERTURB_RESTORE", "ram");
    check(resume(&cpu, state, reason) && source_tas_stateio_ram_digest(ram, 2097152u) != digest,
          "a ram perturbation changes the restored RAM");
    set_env("PSX_TAS_PERTURB_RESTORE", "sched");
    check(resume(&cpu, state, reason) && fixture_return_tcb != TCB_BASE + 0xC0u,
          "a sched perturbation changes the restored TCB");
    set_env("PSX_TAS_PERTURB_RESTORE", NULL);

    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) remove(files[i]);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: checkpoint capture at a frame and resume");
    return 0;
}
#else
int main(void) {
    static CPUState cpu;
    set_env("PSX_TAS_SAVE_STATE_AT", "5");
    set_env("PSX_TAS_SAVE_STATE_PATH", "tas_checkpoint_nodebug-{frame}.pst");
    set_env("PSX_TAS_RESUME_STATE", "tas_checkpoint_missing.pst");
    tas_checkpoint_on_frame(5);
    check(!staged, "no capture is staged without debug tools");
    check(tas_checkpoint_start(&cpu) == 0, "a resume request is ignored without debug tools");
    if (failures) return 1;
    puts("PASS: checkpoints are absent without debug tools");
    return 0;
}
#endif
