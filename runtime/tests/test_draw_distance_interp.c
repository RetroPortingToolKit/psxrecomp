/* [[draw_distance.clamp]] in the dirty-RAM interpreter.
 *
 * Generated main-EXE code clamps a listed site's register before the guard
 * runs (draw_distance_codegen_test covers the emit). Main-EXE code that runs
 * from the interpreter goes through dirty_ram_interp.c instead, which looks
 * the PC up in draw_distance.c's sorted store. This test runs the production
 * code of both files:
 *
 *   - the store: physical addresses, sort, word-guarded lookup, the cap and
 *     its log line, empty and NULL lists;
 *   - the switch: psx_mod_set_draw_distance_clamp's return value and the
 *     interpreter's live gate;
 *   - real interpreted instructions (exec_one_fetched) with the switch off
 *     and on: the sltiu and addiu guard forms, an unlisted twin, a
 *     mismatched word, a site outside the game's text image, and a negative
 *     index that the original guard still rejects.
 *
 * The sources are #included (gpu.c for the widescreen site stores the
 * interpreter also consults) so the test can call the interpreter's static
 * entry point; unreferenced code is dropped at link time (LTO plus section
 * GC / dead_strip), so only the stubs below are needed. */
#define PSX_HAS_GAME_DISPATCH 1
#include "../src/draw_distance.c"
#include "../src/gpu.c"
#include "../src/dirty_ram_interp.c"

#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#else
#include <unistd.h>
#endif

/* ---- link stubs for code reachable from exec_one_fetched ------------------ */
uint64_t s_frame_count;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
uint32_t g_dirty_ram_code_gen;
uint64_t g_dispatch_static_hits;
int      g_rfe_escape_pending;
int      g_exc_escape_reason;
int      g_psx_call_bail;
uint64_t g_psx_bail_first;
uint64_t g_psx_bail_resolved;
uint32_t i_stat;
uint32_t i_mask;
uint64_t psx_cycle_count;
uint32_t g_psx_mod_function_entry_hooks;
uint32_t g_psx_mod_instruction_hooks;
static uint32_t address_calls, shadow_address;
uint32_t psx_mod_memory_address(const CPUState *cpu, uint32_t pc, uint32_t word, uint32_t address) {
    (void)cpu; (void)pc; (void)word;
    ++address_calls;
    return address + 0x1000u;
}
void psx_mod_instruction(CPUState *cpu, uint32_t pc, uint32_t instruction) {
    (void)cpu; (void)pc; (void)instruction;
}
uint32_t g_psx_ram_size = PSX_MAIN_RAM_RETAIL_BYTES;
uint32_t g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_BYTES - 1u;

static uint8_t test_ram[0x00200000u];

uint8_t *memory_get_ram_ptr(void) { return test_ram; }
uint8_t psx_read_byte(uint32_t a) { return test_ram[a & 0x1FFFFFu]; }
uint16_t psx_read_half(uint32_t a) {
    uint16_t v; memcpy(&v, test_ram + (a & 0x1FFFFEu), sizeof v); return v;
}
uint32_t psx_read_word(uint32_t a) {
    uint32_t v; memcpy(&v, test_ram + (a & 0x1FFFFCu), sizeof v); return v;
}
uint8_t psx_cyc_load_byte(CPUState *cpu, uint32_t a, uint32_t rt, uint32_t m) {
    (void)cpu; (void)rt; (void)m; return psx_read_byte(a);
}
uint32_t psx_cyc_lwc2_read(CPUState *cpu, uint32_t a) { (void)cpu; return psx_read_word(a); }
int dirty_ram_is_dirty(uint32_t phys) { (void)phys; return 1; }
int mdec_recently_active(uint32_t f) { (void)f; return 0; }
void gte_execute(CPUState *cpu, uint32_t cmd) { (void)cpu; (void)cmd; }
void gte_precision_store_word(uint32_t a, uint8_t r) { (void)a; (void)r; }
uint32_t gte_read_ctrl(CPUState *cpu, uint8_t r) { (void)cpu; (void)r; return 0; }
uint32_t gte_read_data(CPUState *cpu, uint8_t r) { return cpu->gte_data[r]; }
void gte_write_ctrl(CPUState *cpu, uint8_t r, uint32_t v) { (void)cpu; (void)r; (void)v; }
void gte_write_data(CPUState *cpu, uint8_t r, uint32_t v) { cpu->gte_data[r] = v; }
int overlay_loader_call_native(CPUState *cpu, uint32_t a) { (void)cpu; (void)a; return 0; }
void psx_bail_record(uint32_t ra, uint32_t sp, uint32_t pc, uint32_t gsp) {
    (void)ra; (void)sp; (void)pc; (void)gsp;
}
void psx_break(CPUState *cpu, uint32_t code, uint32_t pc) { (void)cpu; (void)code; (void)pc; }
void psx_check_interrupts(struct CPUState *cpu) { (void)cpu; }
void psx_dispatch_call(CPUState *cpu, uint32_t t, uint32_t r) { (void)cpu; (void)t; (void)r; }
void psx_fatal_halt(const char *reason) { fprintf(stderr, "fatal: %s\n", reason); exit(2); }
void psx_get_freeze_diag(uint64_t *a, uint32_t *b, int *c, int *d, uint64_t *e,
                         uint64_t *f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
}
int psx_get_in_exception(void) { return 0; }
int psx_interrupts_checked_at_current_cycle(uint32_t pc) { (void)pc; return 1; }
void psx_pgxp_alu(struct CPUState *c, uint32_t i, uint32_t r, uint32_t a, uint32_t b) {
    (void)c; (void)i; (void)r; (void)a; (void)b;
}
void psx_pgxp_cop2(struct CPUState *c, uint32_t i, uint32_t v, uint32_t a) {
    (void)c; (void)i; (void)v; shadow_address = a;
}
void psx_pgxp_load(struct CPUState *c, uint32_t i, uint32_t a, uint32_t v) {
    (void)c; (void)i; (void)v; shadow_address = a;
}
void psx_pgxp_muldiv(struct CPUState *c, uint32_t i, uint32_t h, uint32_t l,
                     uint32_t a, uint32_t b) {
    (void)c; (void)i; (void)h; (void)l; (void)a; (void)b;
}
void psx_pgxp_store(struct CPUState *c, uint32_t i, uint32_t a, uint32_t v) {
    (void)c; (void)i; (void)v; shadow_address = a;
}
void psx_rfe_mark_escape(void) {}
int psx_syscall(CPUState *cpu, uint32_t code) { (void)cpu; (void)code; return 0; }
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t a) { (void)cpu; (void)a; return 0; }
int psx_game_is_function_entry(uint32_t a) { (void)a; return 0; }
int psx_game_text_native_ok(uint32_t a) { (void)a; return 0; }
int psx_game_text_native_ok_full(uint32_t a) { (void)a; return 0; }
int psx_mod_function_entry(struct CPUState *cpu, uint32_t a) { (void)cpu; (void)a; return 0; }

/* The game's text image for this test: [TEXT_LO, TEXT_HI). */
#define TEXT_LO 0x80020000u
#define TEXT_HI 0x80030000u
int psx_game_address_in_text(uint32_t addr) {
    const uint32_t phys = addr & 0x1FFFFFFFu;
    return phys >= (TEXT_LO & 0x1FFFFFFFu) && phys < (TEXT_HI & 0x1FFFFFFFu);
}

/* ---- harness --------------------------------------------------------------- */
static int failures;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

enum { AT = 1, V0 = 2, T2 = 10 };
#define SLTIU_T2_V0 0x2C4A01C0u /* sltiu t2, v0, 0x1C0 */
#define ADDIU_AT_V0 0x2441FFFFu /* addiu at, v0, -1    */
#define SLTIU_AT_AT 0x2C2101BFu /* sltiu at, at, 0x1BF */

static void step(CPUState *cpu, uint32_t pc, uint32_t insn) {
    uint32_t next = 0;
    cpu->pc = pc;
    (void)exec_one_fetched(cpu, pc, insn, &next);
}

static void put_word(uint32_t addr, uint32_t v) { memcpy(test_ram + (addr & 0x1FFFFFu), &v, 4); }

static const char *s_capture_path = "draw_distance_interp_stderr.txt";

static void capture_stderr(void (*fn)(void), char *buf, size_t cap) {
    FILE *tmp = fopen(s_capture_path, "w+b");
    buf[0] = '\0';
    if (!tmp) {
        CHECK(0, "cannot open the stderr capture file %s", s_capture_path);
        fn();
        return;
    }
    fflush(stderr);
    int saved = dup(fileno(stderr));
    dup2(fileno(tmp), fileno(stderr));
    fn();
    fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    rewind(tmp);
    size_t n = fread(buf, 1, cap - 1, tmp);
    buf[n] = '\0';
    fclose(tmp);
    remove(s_capture_path);
}

/* ---- store ----------------------------------------------------------------- */
static PSXDrawDistanceClampSite big[300];
static void set_300(void) { (void)psx_draw_distance_set_clamp_sites(big, 300); }

static void test_store(void) {
    const PSXDrawDistanceClampSite sites[] = {
        { 0x80020010u, SLTIU_T2_V0, V0, 0x1BF },
        { 0xA0020000u, ADDIU_AT_V0, V0, 0x1BF },   /* KSEG1 spelling */
        { 0x00020008u, SLTIU_T2_V0, V0, 0x100 },   /* KUSEG spelling */
    };
    CHECK(psx_draw_distance_set_clamp_sites(sites, 3) == 3, "three stored");
    CHECK(psx_draw_distance_clamp_site_count() == 3, "count 3");
    CHECK(s_sites[0].address == 0x00020000u && s_sites[1].address == 0x00020008u &&
              s_sites[2].address == 0x00020010u,
          "stored sorted and physical");
    const PSXDrawDistanceClampSite *s = psx_draw_distance_clamp_find(0x80020000u, ADDIU_AT_V0);
    CHECK(s && s->reg == V0 && s->max == 0x1BF, "KSEG0 lookup of a KSEG1 entry");
    CHECK(psx_draw_distance_clamp_find(0x00020008u, SLTIU_T2_V0) != NULL,
          "KUSEG lookup");
    CHECK(psx_draw_distance_clamp_find(0x80020010u, ADDIU_AT_V0) == NULL,
          "another word at a listed address misses");
    CHECK(psx_draw_distance_clamp_find(0x80020004u, SLTIU_T2_V0) == NULL,
          "between entries misses");
    CHECK(psx_draw_distance_clamp_find(0x80020014u, SLTIU_T2_V0) == NULL,
          "above the last entry misses");
    CHECK(psx_draw_distance_clamp_find(0x8001FFFCu, SLTIU_T2_V0) == NULL,
          "below the first entry misses");

    CHECK(psx_draw_distance_set_clamp_sites(NULL, 5) == 0, "NULL list stores none");
    CHECK(psx_draw_distance_set_clamp_sites(sites, -1) == 0, "negative count stores none");
    CHECK(psx_draw_distance_clamp_find(0x80020010u, SLTIU_T2_V0) == NULL, "empty store misses");

    for (int i = 0; i < 300; i++) {
        big[i].address = 0x80040000u + 4u * (uint32_t)(299 - i);
        big[i].expected = SLTIU_T2_V0;
        big[i].reg = V0;
        big[i].max = 0x1BF;
    }
    char log[512];
    capture_stderr(set_300, log, sizeof log);
    CHECK(psx_draw_distance_clamp_site_count() == PSX_DRAW_DISTANCE_CLAMP_SITES_MAX,
          "over-long list stored %d", psx_draw_distance_clamp_site_count());
    CHECK(strstr(log, "lists 300 sites; keeping the first 256") != NULL,
          "overflow not logged (got \"%s\")", log);
    int hits = 0;
    for (int i = 0; i < 300; i++)
        hits += psx_draw_distance_clamp_find(big[i].address, SLTIU_T2_V0) != NULL;
    CHECK(hits == 256, "%d of the first 256 kept", hits);
}

/* ---- switch ---------------------------------------------------------------- */
static void test_switch(void) {
    (void)psx_draw_distance_set_clamp_sites(NULL, 0);
    CHECK(psx_mod_set_draw_distance_clamp(1) == 0, "no sites: the setter says so");
    CHECK(psx_mod_draw_distance_clamp_enabled(), "the switch is still recorded");
    CHECK(!g_psx_draw_distance_clamp_live, "no sites: interpreter gate stays off");
    const PSXDrawDistanceClampSite one = { 0x80020000u, SLTIU_T2_V0, V0, 0x1BF };
    (void)psx_draw_distance_set_clamp_sites(&one, 1);
    CHECK(g_psx_draw_distance_clamp_live, "sites after the switch: gate on");
    CHECK(psx_mod_set_draw_distance_clamp(0) == 1, "sites: the setter says so");
    CHECK(!g_psx_draw_distance_clamp_live && !g_psx_draw_distance_clamp,
          "off clears both");
    CHECK(psx_mod_set_draw_distance_clamp(7) == 1 && g_psx_draw_distance_clamp == 1u,
          "any nonzero is on");
    (void)psx_mod_set_draw_distance_clamp(0);
}

/* ---- interpreted instructions ---------------------------------------------- */
static void test_interpreter(void) {
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    const uint32_t site = TEXT_LO + 0x100u, twin = TEXT_LO + 0x200u;
    const uint32_t a_site = TEXT_LO + 0x300u, a_guard = a_site + 4u;
    const uint32_t outside = TEXT_HI + 0x100u;
    put_word(site, SLTIU_T2_V0);
    put_word(twin, SLTIU_T2_V0);
    put_word(a_site, ADDIU_AT_V0);
    put_word(a_guard, SLTIU_AT_AT);
    put_word(outside, SLTIU_T2_V0);
    const PSXDrawDistanceClampSite sites[] = {
        { site, SLTIU_T2_V0, V0, 0x1BF },
        { a_site, ADDIU_AT_V0, V0, 0x1BF },
        { outside, SLTIU_T2_V0, V0, 0x1BF },
    };
    (void)psx_draw_distance_set_clamp_sites(sites, 3);

    /* Off: the original guard rejects the far index and keeps the register. */
    (void)psx_mod_set_draw_distance_clamp(0);
    cpu.gpr[V0] = 0x300u;
    step(&cpu, site, SLTIU_T2_V0);
    CHECK(cpu.gpr[T2] == 0u && cpu.gpr[V0] == 0x300u, "off: vanilla reject");

    (void)psx_mod_set_draw_distance_clamp(1);
    /* On: kept in the last slot. */
    cpu.gpr[V0] = 0x300u;
    step(&cpu, site, SLTIU_T2_V0);
    CHECK(cpu.gpr[V0] == 0x1BFu && cpu.gpr[T2] == 1u, "on: clamped and kept (v0=%X t2=%u)",
          cpu.gpr[V0], cpu.gpr[T2]);
    /* Near indices are untouched. */
    cpu.gpr[V0] = 0x40u;
    step(&cpu, site, SLTIU_T2_V0);
    CHECK(cpu.gpr[V0] == 0x40u && cpu.gpr[T2] == 1u, "on: near index unchanged");
    /* The same instruction at an unlisted address stays vanilla. */
    cpu.gpr[V0] = 0x300u;
    step(&cpu, twin, SLTIU_T2_V0);
    CHECK(cpu.gpr[V0] == 0x300u && cpu.gpr[T2] == 0u, "on: unlisted twin vanilla");
    /* Another word at a listed address stays vanilla. */
    cpu.gpr[V0] = 0x300u;
    step(&cpu, site, ADDIU_AT_V0);
    CHECK(cpu.gpr[V0] == 0x300u && cpu.gpr[AT] == 0x2FFu,
          "on: mismatched word vanilla");
    /* Outside the game's text image (captured overlay code): vanilla. */
    cpu.gpr[V0] = 0x300u;
    step(&cpu, outside, SLTIU_T2_V0);
    CHECK(cpu.gpr[V0] == 0x300u && cpu.gpr[T2] == 0u, "on: outside text vanilla");

    /* The addiu form: `addiu at,v0,-1; sltiu at,at,0x1BF` rejects 0 (too
     * near) and >= 0x1C0 (too far). The clamp keeps far, never near. */
    cpu.gpr[V0] = 0x900u;
    step(&cpu, a_site, ADDIU_AT_V0);
    step(&cpu, a_guard, SLTIU_AT_AT);
    CHECK(cpu.gpr[V0] == 0x1BFu && cpu.gpr[AT] == 1u, "on: addiu form keeps far");
    cpu.gpr[V0] = 0u;
    step(&cpu, a_site, ADDIU_AT_V0);
    step(&cpu, a_guard, SLTIU_AT_AT);
    CHECK(cpu.gpr[V0] == 0u && cpu.gpr[AT] == 0u, "on: addiu form still rejects near");
    /* A negative (wrapped) index is left for the original guard. */
    cpu.gpr[V0] = 0xFFFFFFF0u;
    step(&cpu, site, SLTIU_T2_V0);
    CHECK(cpu.gpr[V0] == 0xFFFFFFF0u && cpu.gpr[T2] == 0u, "on: negative index vanilla");

    (void)psx_mod_set_draw_distance_clamp(0);
    cpu.gpr[V0] = 0x900u;
    step(&cpu, a_site, ADDIU_AT_V0);
    step(&cpu, a_guard, SLTIU_AT_AT);
    CHECK(cpu.gpr[V0] == 0x900u && cpu.gpr[AT] == 0u, "off again: vanilla");
}

static void write_byte(uint32_t a, uint8_t v) { test_ram[a & 0x1fffffu] = v; }
static void write_half(uint32_t a, uint16_t v) { memcpy(test_ram + (a & 0x1fffffu), &v, 2); }
static void test_memory_addresses(void) {
    const uint32_t ops[] = {0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x28,0x29,0x2a,0x2b,0x2e,0x32,0x3a};
    /* Compare real interpreted instructions on relocated memory against the
     * same instructions on the original memory, including unaligned lanes. */
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; ++i) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            uint32_t op = ops[i];
            if (lane && op != 0x22 && op != 0x26 && op != 0x2a && op != 0x2e) continue;
            CPUState vanilla = {0}, adapted = {0};
            vanilla.read_word = psx_read_word; vanilla.read_half = psx_read_half;
            vanilla.read_byte = psx_read_byte;
            vanilla.write_word = put_word; vanilla.write_half = write_half; vanilla.write_byte = write_byte;
            vanilla.gpr[5] = 0x80008004u;
            vanilla.gpr[3] = 0x87654321u; vanilla.gte_data[3] = 0x87654321u;
            adapted = vanilla;
            put_word(0x80008000u, 0xabcdef01u); put_word(0x80009000u, 0xabcdef01u);
            const uint32_t word = (op << 26) | (5u << 21) | (3u << 16) | (0xfffcu + lane);
            g_psx_mod_instruction_hooks = 0;
            step(&vanilla, TEXT_LO, word);
            const uint32_t expected = psx_read_word(0x80008000u);
            address_calls = 0; g_psx_mod_instruction_hooks = 1;
            step(&adapted, TEXT_LO, word);
            CHECK(adapted.gpr[3] == vanilla.gpr[3] && adapted.gte_data[3] == vanilla.gte_data[3],
                  "adapted load/merge/GTE result op=%x lane=%u", op, lane);
            CHECK(psx_read_word(0x80009000u) == expected && psx_read_word(0x80008000u) == expected,
                  "adapted store/merge result op=%x lane=%u", op, lane);
            CHECK(adapted.gpr[5] == 0x80008004u && address_calls == 1 && shadow_address == 0x80009000u + lane,
                  "adapter preserves base and PGXP address op=%x lane=%u", op, lane);
        }
    }
    CPUState cpu = {0}; cpu.read_word = psx_read_word;
    cpu.gpr[5] = 0x80008000u; put_word(0x80009000u, 0x12345678u);
    step(&cpu, TEXT_LO, 0x8ca50000u); /* lw a1,0(a1) */
    CHECK(cpu.gpr[5] == 0x12345678u && shadow_address == 0x80009000u,
          "load destination may alias original base register");
    g_psx_mod_instruction_hooks = 0;
}

int main(int argc, char **argv) {
    if (argc > 1) s_capture_path = argv[1];
    test_store();
    test_switch();
    test_interpreter();
    test_memory_addresses();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("PASS draw-distance clamp store, switch and interpreted sites");
    return 0;
}
