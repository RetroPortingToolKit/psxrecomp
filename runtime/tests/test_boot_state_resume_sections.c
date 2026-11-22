/* Required CPU_EXEC plus checkpoint-only SCHED, IRQ_TIMING and MEMCTRL through
 * the real boot_state writer and loader, on the handoff fixture's hardware
 * doubles. Switch off: CPU_EXEC remains in ordinary player snapshots.
 * Switch on: three additional sections round-trip, and a malformed or
 * repeated one refuses the whole state before anything changes. */
#define main boot_state_handoff_tests
#include "test_boot_state_handoff.c"
#undef main

#define TCB_BASE 0x800F0000u
#define TCB_SIZE (4u * 0xC0u)

static const uint32_t resume_tags[4] = {
    BS_SEC_CPU_EXEC, BS_SEC_SCHED, BS_SEC_IRQ_TIMING, BS_SEC_MEMCTRL
};

static void env_set(const char *k, const char *v) {
#if defined(_WIN32)
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void put32(uint8_t *p, uint32_t v) { u32(p, v); }

static void seed_live(void) {
    fixture_ld.old = 0x11223344u; fixture_ld.next = 0x80050044u;
    fixture_ld.rt = 7; fixture_ld.val = 0xCAFEF00Du; fixture_ld.age = 1; fixture_ld.armed = 1;
    fixture_return_tcb = TCB_BASE + 2u * 0xC0u;
    fixture_irq_timing.cooldown_until = 0x123456789ull;
    fixture_irq_timing.sio_seq_seen = 41;
    fixture_irq_timing.sio_progress_cycle = 0x1000;
    fixture_irq_timing.defer_from = TCB_BASE;
    fixture_irq_timing.defer_target = TCB_BASE + 0xC0u;
    fixture_irq_timing.defer_pending = 1;
    for (uint32_t i = 0; i < PSX_MEMORY_CONTROL_WORDS; i++) fixture_memctrl[i] = 0x1F000000u + i;
    put32(ram + 0x110, TCB_BASE);
    put32(ram + 0x114, TCB_SIZE);
}

static uint8_t *save(CPUState *cpu, int compressed, size_t *len) {
    uint8_t *out = NULL;
    cpu->pc = 0x80050044; ram[0x100] = 0xab;
    assert(compressed ? boot_state_save_buffer(cpu, 123, 0x801b0000, &out, len)
                      : boot_state_save_buffer_raw(cpu, 123, 0x801b0000, &out, len));
    return out;
}

#ifndef PSX_NO_DEBUG_TOOLS
static void switch_off(void) {
    env_set("PSX_TAS_SAVE_STATE_AT", NULL);
    env_set("PSX_TAS_SAVE_STATE_PATH", NULL);
}

/* Different live values, sentinels on CPU and RAM. */
static void clobber_live(CPUState *cpu) {
    memset(&fixture_ld, 0, sizeof fixture_ld);
    fixture_return_tcb = 0x11111111u;
    memset(&fixture_irq_timing, 0, sizeof fixture_irq_timing);
    memset(fixture_memctrl, 0, sizeof fixture_memctrl);
    fixture_ld_from_state = 0;
    fixture_irq_timing_set = 0;
    cpu->pc = 0xdeadbeef;
    ram[0x100] = 0x55;
}

static int live_is_clobbered(const CPUState *cpu) {
    uint32_t zero[PSX_MEMORY_CONTROL_WORDS] = {0};
    return cpu->pc == 0xdeadbeef && ram[0x100] == 0x55 && !fixture_ld_from_state &&
           !fixture_irq_timing_set && fixture_return_tcb == 0x11111111u &&
           !memcmp(fixture_memctrl, zero, sizeof zero) && !fixture_ld.armed;
}

static int live_is_seeded(void) {
    if (fixture_ld.rt != 7 || fixture_ld.val != 0xCAFEF00Du || fixture_ld.age != 1 ||
        fixture_ld.armed != 1 || !fixture_ld_from_state)
        return 0;
    if (fixture_return_tcb != TCB_BASE + 2u * 0xC0u) return 0;
    if (!fixture_irq_timing_set || fixture_irq_timing.cooldown_until != 0x123456789ull ||
        fixture_irq_timing.sio_seq_seen != 41 || fixture_irq_timing.sio_progress_cycle != 0x1000 ||
        fixture_irq_timing.defer_from != TCB_BASE ||
        fixture_irq_timing.defer_target != TCB_BASE + 0xC0u ||
        fixture_irq_timing.defer_pending != 1)
        return 0;
    for (uint32_t i = 0; i < PSX_MEMORY_CONTROL_WORDS; i++)
        if (fixture_memctrl[i] != 0x1F000000u + i) return 0;
    return 1;
}

static int load(uint8_t *image, size_t len, CPUState *cpu) {
    return boot_state_load_buffer(image, len, 123, 0x801b0000, cpu);
}

/* Copy of image without the sections whose tags are in drop[]. */
static uint8_t *strip(const uint8_t *image, size_t len, const uint32_t *drop, unsigned n,
                      size_t *out_len) {
    uint8_t *out = malloc(len);
    size_t at = BOOT_STATE_HEADER_WIRE_BYTES, w = at;
    uint32_t count = test_u32(image + 28), kept = 0;
    assert(out);
    memcpy(out, image, at);
    for (uint32_t i = 0; i < count; i++) {
        PstR r; uint32_t tag, flags; uint64_t bytes; int dropped = 0;
        pst_r_init(&r, image + at, 16);
        assert(pst_r_u32(&r, &tag) && pst_r_u32(&r, &flags) && pst_r_u64(&r, &bytes));
        for (unsigned k = 0; k < n; k++) dropped |= tag == drop[k];
        if (!dropped) { memcpy(out + w, image + at, 16 + (size_t)bytes); w += 16 + (size_t)bytes; kept++; }
        at += 16 + (size_t)bytes;
    }
    assert(at == len);
    put32(out + 28, kept);
    *out_len = w;
    return out;
}

/* Copy of image with one more copy of the section tagged `tag`. */
static uint8_t *duplicate(const uint8_t *image, size_t len, uint32_t tag, size_t *out_len) {
    size_t off, framed;
    uint8_t *out = malloc(len + 256);
    assert(out && test_find_section((uint8_t *)image, len, tag, &off, &framed) && framed <= 256);
    memcpy(out, image, len);
    memcpy(out + len, image + off, framed);
    put32(out + 28, test_u32(image + 28) + 1);
    *out_len = len + framed;
    return out;
}

/* Load must refuse `image` and leave every live value as it was. */
static void refused(uint8_t *image, size_t len, CPUState *cpu, const char *what) {
    clobber_live(cpu);
    if (load(image, len, cpu) || !live_is_clobbered(cpu)) {
        fprintf(stderr, "FAIL: not refused before mutation: %s\n", what);
        exit(1);
    }
}

/* Rewrite one u32 of a resume section in a copy, check refusal. */
static void refused_word(const uint8_t *image, size_t len, uint32_t tag, size_t word,
                         uint32_t value, CPUState *cpu, const char *what) {
    uint8_t *copy = malloc(len);
    uint8_t *p;
    assert(copy);
    memcpy(copy, image, len);
    p = test_find_section(copy, len, tag, NULL, NULL);
    assert(p);
    put32(p + 4 * word, value);
    refused(copy, len, cpu, what);
    free(copy);
}

#endif

int main(void) {
    CPUState cpu = {0};
    fntrace_set_game_range(0x801b0000, 0);
    fntrace_restore_game_started(1);
    seed_live();
#ifdef PSX_NO_DEBUG_TOOLS
    env_set("PSX_TAS_SAVE_STATE_AT", "5");
    assert(!boot_state_resume_sections_enabled());
    {
        size_t len; uint8_t *image = save(&cpu, 0, &len);
        assert(test_find_section(image, len, BS_SEC_CPU_EXEC, NULL, NULL));
        for (unsigned k = 1; k < 4; k++)
            assert(!test_find_section(image, len, resume_tags[k], NULL, NULL));
        free(image);
    }
    puts("PASS: player pipeline remains; optional resume sections compiled out");
    return 0;
#else
    switch_off();
    assert(!boot_state_resume_sections_enabled());
    env_set("PSX_TAS_SAVE_STATE_AT", "");
    assert(!boot_state_resume_sections_enabled());
    env_set("PSX_TAS_SAVE_STATE_PATH", "cp.pst");
    assert(boot_state_resume_sections_enabled());
    switch_off();

    for (int compressed = 0; compressed < 2; ++compressed) {
        size_t off_len, on_len, cut_len, x_len;
        uint8_t *off, *on, *cut, *x;

        /* Off: the player pipeline restores without the three debug seams. */
        seed_live();
        off = save(&cpu, compressed, &off_len);
        assert(test_find_section(off, off_len, BS_SEC_CPU_EXEC, NULL, NULL));
        for (unsigned k = 1; k < 4; k++)
            assert(!test_find_section(off, off_len, resume_tags[k], NULL, NULL));
        clobber_live(&cpu);
        assert(load(off, off_len, &cpu));
        assert(cpu.pc == 0x80050044 && ram[0x100] == 0xab);
        assert(fixture_ld_from_state && fixture_ld.old == 0x11223344u &&
               fixture_ld.next == 0x80050044u && !fixture_irq_timing_set &&
               fixture_return_tcb == 0x11111111u);

        /* On: the same player state plus exactly three debug sections. */
        seed_live();
        env_set("PSX_TAS_SAVE_STATE_AT", "5");
        on = save(&cpu, compressed, &on_len);
        switch_off();
        assert(test_u32(on + 28) == test_u32(off + 28) + 3);
        for (unsigned k = 0; k < 4; k++)
            assert(test_find_section(on, on_len, resume_tags[k], NULL, NULL));
        cut = strip(on, on_len, resume_tags + 1, 3, &cut_len);
        assert(cut_len == off_len && !memcmp(cut, off, off_len));
        free(cut);

        /* Round trip: every value comes back. */
        clobber_live(&cpu);
        assert(load(on, on_len, &cpu));
        assert(cpu.pc == 0x80050044 && ram[0x100] == 0xab && live_is_seeded());

        /* A state may hold a debug subset: SCHED plus mandatory CPU_EXEC. */
        {
            const uint32_t two[2] = {BS_SEC_IRQ_TIMING, BS_SEC_MEMCTRL};
            x = strip(on, on_len, two, 2, &x_len);
            clobber_live(&cpu);
            assert(load(x, x_len, &cpu));
            assert(fixture_return_tcb == TCB_BASE + 2u * 0xC0u &&
                   fixture_ld_from_state && !fixture_irq_timing_set);
            free(x);
        }

        /* Malformed payloads. */
        refused_word(on, on_len, BS_SEC_CPU_EXEC, 2, 1, &cpu, "CPU_EXEC reserved word 2");
        refused_word(on, on_len, BS_SEC_CPU_EXEC, 4, 1, &cpu, "CPU_EXEC reserved word 4");
        refused_word(on, on_len, BS_SEC_CPU_EXEC, 5, 32, &cpu, "CPU_EXEC rt 32");
        refused_word(on, on_len, BS_SEC_CPU_EXEC, 7, 2, &cpu, "CPU_EXEC age 2");
        refused_word(on, on_len, BS_SEC_CPU_EXEC, 8, 2, &cpu, "CPU_EXEC armed 2");
        /* A retired load leaves age 1 with armed 0; the next load re-arms
         * with age 0, so the state is live and must load as it was saved. */
        x = malloc(on_len); assert(x); memcpy(x, on, on_len);
        put32(test_find_section(x, on_len, BS_SEC_CPU_EXEC, NULL, NULL) + 4 * 8, 0);
        clobber_live(&cpu);
        assert(load(x, on_len, &cpu));
        assert(fixture_ld.age == 1 && fixture_ld.armed == 0 && fixture_ld.rt == 7 &&
               fixture_ld_from_state);
        free(x);
        refused_word(on, on_len, BS_SEC_SCHED, 1, 1, &cpu, "SCHED reserved word");
        refused_word(on, on_len, BS_SEC_SCHED, 0, TCB_BASE + 0x10u, &cpu, "SCHED TCB misaligned");
        refused_word(on, on_len, BS_SEC_SCHED, 0, TCB_BASE + TCB_SIZE, &cpu, "SCHED TCB past table");
        refused_word(on, on_len, BS_SEC_SCHED, 0, TCB_BASE - 0xC0u, &cpu, "SCHED TCB before table");
        refused_word(on, on_len, BS_SEC_IRQ_TIMING, 0, 1, &cpu, "IRQ_TIMING reserved word 0");
        refused_word(on, on_len, BS_SEC_IRQ_TIMING, 10, 1, &cpu, "IRQ_TIMING reserved word 10");
        refused_word(on, on_len, BS_SEC_IRQ_TIMING, 15, 2, &cpu, "IRQ_TIMING defer_pending 2");
        for (unsigned k = 0; k < 4; k++) {
            x = duplicate(on, on_len, resume_tags[k], &x_len);
            refused(x, x_len, &cpu, "a repeated resume section");
            free(x);
        }

        /* SCHED is judged by the state's RAM, not the live one. RAM is a
         * raw payload only in uncompressed images. */
        if (!compressed) {
            uint8_t *state_ram;
            x = malloc(on_len); assert(x); memcpy(x, on, on_len);
            state_ram = test_find_section(x, on_len, BS_SEC_RAM, NULL, NULL);
            assert(state_ram);
            put32(state_ram + 0x110, 0);          /* the state has no TCB table */
            refused(x, on_len, &cpu, "SCHED TCB outside the state's table");
            put32(state_ram + 0x110, TCB_BASE);
            put32(ram + 0x110, 0);                /* the live machine has none */
            clobber_live(&cpu);
            assert(load(x, on_len, &cpu) && fixture_return_tcb == TCB_BASE + 2u * 0xC0u);
            free(x);
        }

        /* An ordinary current state restores its pipeline after a debug load. */
        clobber_live(&cpu);
        assert(load(off, off_len, &cpu));
        assert(fixture_ld_from_state && !fixture_irq_timing_set &&
               fixture_return_tcb == 0x11111111u);
        free(off); free(on);
    }
    puts("PASS: required player pipeline, optional checkpoint sections, atomic malformed rejection");
    return 0;
#endif
}
