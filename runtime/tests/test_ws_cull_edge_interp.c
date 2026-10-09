/* Explicit [widescreen.cull] sites in the dirty-RAM interpreter.
 *
 * Native code compiles bltz_sites, bgez_sites, branch_keep_sites and
 * clip_edge_x_load_sites in (ws_cull_edge_codegen_test covers the emit). A
 * listed site that runs from dirty RAM goes through dirty_ram_interp.c
 * instead, which looks the PC up in gpu.c's sorted explicit-site store. This
 * test runs the production code of both files:
 *
 *   - the store: masking to physical addresses, sort, de-duplication, lookups
 *     at both ends and between entries, the 256-entry cap and its log line,
 *     empty and negative-count lists, the clip-edge width default;
 *   - psx_ws_cull_bgez / psx_ws_clip_edge_x through a forced margin;
 *   - real interpreted instructions (exec_one_fetched) at margin 0 and at a
 *     forced margin: BGEZ and BLTZ sites, BNE and BLEZ branch-keep sites, and
 *     LH / LHU / LW clip-edge loads, each next to an unlisted twin that must
 *     keep the vanilla result.
 *
 * Both sources are #included so the test can call the interpreter's static
 * entry point and read the store's static arrays; unreferenced code is
 * dropped at link time (LTO plus section GC / dead_strip), so only the stubs
 * below are needed. */
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
uint32_t g_psx_mod_instruction_hooks;
void psx_mod_instruction(CPUState *cpu, uint32_t pc, uint32_t instruction) {
    (void)cpu; (void)pc; (void)instruction; abort();
}
int      g_rfe_escape_pending;
int      g_exc_escape_reason;
int      g_psx_call_bail;
uint64_t g_psx_bail_first;
uint64_t g_psx_bail_resolved;
uint32_t i_stat;
uint32_t i_mask;
uint64_t psx_cycle_count;
/* Live RAM geometry (memory.c): retail 2 MiB, as the stub RAM below. */
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
static int precise_sign_valid, precise_sign;
static int previous_sign_valid, previous_sign;
int gte_nclip_native_wide_sign(int32_t mac0, int* sign) {
    (void)mac0; *sign = precise_sign; return precise_sign_valid;
}
int gte_nclip_native_wide_previous_sign(int32_t mac0, int* sign) {
    (void)mac0; *sign = previous_sign; return previous_sign_valid;
}
void gte_precision_store_word(uint32_t a, uint8_t r) { (void)a; (void)r; }
uint32_t gte_read_ctrl(CPUState *cpu, uint8_t r) { (void)cpu; (void)r; return 0; }
uint32_t gte_read_data(CPUState *cpu, uint8_t r) { (void)cpu; (void)r; return 0; }
void gte_write_ctrl(CPUState *cpu, uint8_t r, uint32_t v) { (void)cpu; (void)r; (void)v; }
void gte_write_data(CPUState *cpu, uint8_t r, uint32_t v) { (void)cpu; (void)r; (void)v; }
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
    (void)c; (void)i; (void)v; (void)a;
}
void psx_pgxp_load(struct CPUState *c, uint32_t i, uint32_t a, uint32_t v) {
    (void)c; (void)i; (void)a; (void)v;
}
void psx_pgxp_muldiv(struct CPUState *c, uint32_t i, uint32_t h, uint32_t l,
                     uint32_t a, uint32_t b) {
    (void)c; (void)i; (void)h; (void)l; (void)a; (void)b;
}
void psx_pgxp_store(struct CPUState *c, uint32_t i, uint32_t a, uint32_t v) {
    (void)c; (void)i; (void)a; (void)v;
}
void psx_rfe_mark_escape(void) {}
int psx_syscall(CPUState *cpu, uint32_t code) { (void)cpu; (void)code; return 0; }

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

enum { V0 = 2, V1 = 3, A0 = 4 };
#define CODE 0x80020000u /* phys 0x20000: outside the BIOS load-delay window */

static uint32_t i_type(uint32_t op, uint32_t rs, uint32_t rt, uint16_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | imm;
}

/* Executes one instruction at `pc` (its delay slot, a nop, comes from RAM)
 * and returns the next PC. */
static uint32_t step(CPUState *cpu, uint32_t pc, uint32_t insn) {
    uint32_t next = 0;
    cpu->pc = pc;
    int transfer = exec_one_fetched(cpu, pc, insn, &next);
    return transfer ? cpu->pc : next;
}

/* Branch at `pc` with offset +3 words: taken -> pc+16, fall-through -> pc+8. */
static int branch_taken(CPUState *cpu, uint32_t pc, uint32_t insn) {
    uint32_t next = step(cpu, pc, insn);
    CHECK(next == pc + 16 || next == pc + 8,
          "branch 0x%08X at 0x%08X went to 0x%08X", insn, pc, next);
    return next == pc + 16;
}

static uint32_t load(CPUState *cpu, uint32_t pc, uint32_t insn) {
    cpu->gpr[V0] = 0xDEADBEEFu;
    (void)step(cpu, pc, insn);
    dirty_ram_ld_delay_flush(cpu); /* no pending write outside the BIOS window */
    return cpu->gpr[V0];
}

static void put_half(uint32_t addr, uint16_t v) { memcpy(test_ram + (addr & 0x1FFFFFu), &v, 2); }
static void put_word(uint32_t addr, uint32_t v) { memcpy(test_ram + (addr & 0x1FFFFFu), &v, 4); }

/* stderr is captured through a named file (argv[1]; ctest passes one in its
 * binary directory) rather than tmpfile(): on MSYS2 MinGW64 (msvcrt)
 * tmpfile() creates its file in the root of the current drive, which fails
 * for a normal account. */
static const char *s_capture_path = "ws_cull_edge_interp_stderr.txt";

/* Runs `fn` with stderr captured into `buf`. */
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
static uint32_t big_list[300];
static void set_bltz_300(void) {
    gpu_ws_set_branch_cull_sites(big_list, 300, NULL, 0, NULL, 0);
}
static void set_bltz_256(void) {
    gpu_ws_set_branch_cull_sites(big_list, 256, NULL, 0, NULL, 0);
}

static void test_store(void) {
    /* Unsorted, duplicated, mixed KUSEG/KSEG0/KSEG1 spellings. */
    const uint32_t bgez[] = { 0x80013F50u, 0x00013F40u, 0xA0013F48u,
                              0x80013F40u, 0x8001FFFCu, 0x80013F50u };
    gpu_ws_set_branch_cull_sites(NULL, 0, bgez, 6, NULL, 0);
    CHECK(ws_explicit_bgez_n == 4, "de-duplicated count %d, want 4",
          ws_explicit_bgez_n);
    for (int i = 1; i < ws_explicit_bgez_n; i++)
        CHECK(ws_explicit_bgez_sites[i - 1] < ws_explicit_bgez_sites[i],
              "store not strictly ascending at %d", i);
    CHECK(ws_explicit_bgez_sites[0] == 0x00013F40u, "stored as physical");
    for (int i = 0; i < 6; i++)
        CHECK(psx_ws_is_cull_bgez_site(bgez[i]), "listed 0x%08X missed", bgez[i]);
    CHECK(psx_ws_is_cull_bgez_site(0xA001FFFCu), "last entry via KSEG1");
    CHECK(psx_ws_is_cull_bgez_site(0x00013F48u), "middle entry via KUSEG");
    CHECK(!psx_ws_is_cull_bgez_site(0x80013F3Cu), "below the first entry hit");
    CHECK(!psx_ws_is_cull_bgez_site(0x80013F44u), "between entries hit");
    CHECK(!psx_ws_is_cull_bgez_site(0x80013F4Cu), "between entries hit");
    CHECK(!psx_ws_is_cull_bgez_site(0x80020000u), "above the last entry hit");
    CHECK(!psx_ws_is_cull_bgez_site(0x00000000u), "address 0 hit");
    CHECK(!psx_ws_is_cull_bltz_site(0x80013F40u), "kinds leak into each other");
    CHECK(!psx_ws_is_cull_branch_keep_site(0x80013F40u), "kinds leak into each other");

    /* One entry, empty, negative count, NULL pointer. */
    const uint32_t one = 0x80010040u;
    gpu_ws_set_branch_cull_sites(NULL, 0, NULL, 0, &one, 1);
    CHECK(psx_ws_is_cull_branch_keep_site(one), "single entry missed");
    CHECK(!psx_ws_is_cull_branch_keep_site(one + 4), "single entry neighbour hit");
    CHECK(ws_explicit_bgez_n == 0, "re-set did not clear bgez");
    gpu_ws_set_branch_cull_sites(&one, -3, &one, 0, NULL, 5);
    CHECK(ws_explicit_bltz_n == 0 && ws_explicit_branch_keep_n == 0,
          "negative count or NULL list stored entries");
    CHECK(!psx_ws_is_cull_bltz_site(one), "empty list hit");

    /* The pre-existing setters share the store. */
    const uint32_t slti[] = { 0x80030010u, 0x80030000u };
    gpu_ws_set_explicit_cull_sites(NULL, 0, slti, 2, NULL, 0);
    CHECK(psx_ws_is_cull_slti_site(0x80030000u) &&
          psx_ws_is_cull_slti_site(0x80030010u) &&
          !psx_ws_is_cull_slti_site(0x80030008u), "slti_sites store");
    gpu_ws_set_explicit_cull_sites(NULL, 0, NULL, 0, NULL, 0);

    /* Cap: exactly 256 is silent and complete. */
    for (int i = 0; i < 300; i++)
        big_list[i] = 0x80040000u + 4u * (uint32_t)(299 - i); /* descending */
    char log[512];
    capture_stderr(set_bltz_256, log, sizeof log);
    CHECK(ws_explicit_bltz_n == 256, "256 sites stored %d", ws_explicit_bltz_n);
    CHECK(strstr(log, "exceed") == NULL, "a list at the cap was logged: %s", log);
    int hits = 0;
    for (int i = 0; i < 256; i++) hits += psx_ws_is_cull_bltz_site(big_list[i]);
    CHECK(hits == 256, "only %d of 256 sites found at the cap", hits);

    /* 300: logged, the first 256 in list order kept, the rest vanilla. */
    capture_stderr(set_bltz_300, log, sizeof log);
    CHECK(ws_explicit_bltz_n == 256, "over-long list stored %d", ws_explicit_bltz_n);
    CHECK(strstr(log, "[widescreen] bltz_sites: 300 sites exceed the "
                      "interpreter cap of 256") != NULL,
          "overflow not logged (got \"%s\")", log);
    hits = 0;
    for (int i = 0; i < 256; i++) hits += psx_ws_is_cull_bltz_site(big_list[i]);
    CHECK(hits == 256, "only %d of the first 256 sites found", hits);
    hits = 0;
    for (int i = 256; i < 300; i++) hits += psx_ws_is_cull_bltz_site(big_list[i]);
    CHECK(hits == 0, "%d sites past the cap were kept", hits);
    gpu_ws_set_branch_cull_sites(NULL, 0, NULL, 0, NULL, 0);

    /* Clip-edge width: 0 means the 320 default. */
    gpu_ws_set_clip_edge_x_load_sites(&one, 1, 0);
    CHECK(psx_ws_clip_edge_width() == 320u, "default width %u",
          psx_ws_clip_edge_width());
    gpu_ws_set_clip_edge_x_load_sites(&one, 1, 368);
    CHECK(psx_ws_clip_edge_width() == 368u, "explicit width");
    CHECK(psx_ws_is_cull_clip_edge_x_load_site(one), "clip-edge entry missed");
    gpu_ws_set_clip_edge_x_load_sites(NULL, 0, 0);
}

/* ---- helpers through a forced margin -------------------------------------- */
static void test_helpers(void) {
    gpu_ws_set_margin_override(0);
    CHECK(psx_ws_cull_bgez((uint32_t)-1) == 0 && psx_ws_cull_bgez(0) == 1,
          "bgez not vanilla at margin 0");
    CHECK(psx_ws_cull_bltz((uint32_t)-1) == 1, "bltz not vanilla at margin 0");
    CHECK(psx_ws_clip_edge_x(0, 320) == 0 && psx_ws_clip_edge_x(320, 320) == 320,
          "clip edge not identity at margin 0");

    gpu_ws_set_margin_override(53);
    CHECK(psx_ws_x_margin() == 53, "margin override");
    CHECK(psx_ws_cull_bgez((uint32_t)-53) == 1 && psx_ws_cull_bgez((uint32_t)-54) == 0,
          "bgez edge at margin 53");
    CHECK(psx_ws_cull_bltz((uint32_t)-53) == 0 && psx_ws_cull_bltz((uint32_t)-54) == 1,
          "bltz edge at margin 53");
    CHECK(psx_ws_clip_edge_x(0, 320) == (uint32_t)-53, "left bound -> -m");
    CHECK(psx_ws_clip_edge_x(320, 320) == 373u, "right bound -> W+m");
    CHECK(psx_ws_clip_edge_x(98, 320) == 98u && psx_ws_clip_edge_x(222, 320) == 222u,
          "interior (mirror) bounds must not move");
    CHECK(psx_ws_clip_edge_x(368, 368) == 421u && psx_ws_clip_edge_x(320, 368) == 320u,
          "width parameter");
    gpu_ws_set_margin_override(0);
}

/* ---- interpreted instructions ---------------------------------------------- */
static void test_interpreter(void) {
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    memset(test_ram, 0, sizeof test_ram); /* every delay slot is a nop */

    const uint32_t bgez_site = CODE + 0x000, bgez_plain = CODE + 0x100;
    const uint32_t bltz_site = CODE + 0x010, bltz_plain = CODE + 0x110;
    const uint32_t bne_site  = CODE + 0x020, bne_plain  = CODE + 0x120;
    const uint32_t blez_site = CODE + 0x030, blez_plain = CODE + 0x130;
    const uint32_t lh_site   = CODE + 0x040, lh_plain   = CODE + 0x140;
    const uint32_t lhu_site  = CODE + 0x050, lw_site    = CODE + 0x060;

    const uint32_t bgez = i_type(0x01, V0, 0x01, 3);  /* bgez v0,+3 */
    const uint32_t bltz = i_type(0x01, V0, 0x00, 3);  /* bltz v0,+3 */
    const uint32_t bne  = i_type(0x05, A0, 0, 3);     /* bne  a0,zero,+3 */
    const uint32_t blez = i_type(0x06, A0, 0, 3);     /* blez a0,+3 */
    const uint32_t lh   = i_type(0x21, A0, V0, 0x70); /* lh   v0,0x70(a0) */
    const uint32_t lhu  = i_type(0x25, A0, V0, 0x70); /* lhu  v0,0x70(a0) */
    const uint32_t lw   = i_type(0x23, A0, V0, 0x70); /* lw   v0,0x70(a0) */

    const uint32_t bltz_l[] = { bltz_site }, bgez_l[] = { bgez_site };
    const uint32_t keep_l[] = { bne_site, blez_site };
    const uint32_t clip_l[] = { lh_site, lhu_site, lw_site };
    gpu_ws_set_branch_cull_sites(bltz_l, 1, bgez_l, 1, keep_l, 2);
    gpu_ws_set_clip_edge_x_load_sites(clip_l, 3, 320);

    for (int pass = 0; pass < 2; pass++) {
        const int m = pass ? 53 : 0;
        gpu_ws_set_margin_override(m);

        /* bgez: x0 = -10 sits in the revealed band when m = 53. */
        cpu.gpr[V0] = (uint32_t)-10;
        CHECK(branch_taken(&cpu, bgez_site, bgez) == (m > 0),
              "bgez site, x=-10, m=%d", m);
        CHECK(!branch_taken(&cpu, bgez_plain, bgez), "unlisted bgez, m=%d", m);
        cpu.gpr[V0] = (uint32_t)-60;
        CHECK(!branch_taken(&cpu, bgez_site, bgez), "bgez site, x=-60, m=%d", m);
        cpu.gpr[V0] = 5;
        CHECK(branch_taken(&cpu, bgez_site, bgez), "bgez site, x=5, m=%d", m);

        /* bltz: rejects only past -m. */
        cpu.gpr[V0] = (uint32_t)-10;
        CHECK(branch_taken(&cpu, bltz_site, bltz) == (m == 0),
              "bltz site, x=-10, m=%d", m);
        CHECK(branch_taken(&cpu, bltz_plain, bltz), "unlisted bltz, m=%d", m);
        cpu.gpr[V0] = (uint32_t)-60;
        CHECK(branch_taken(&cpu, bltz_site, bltz), "bltz site, x=-60, m=%d", m);

        /* branch_keep: the reject is forced not-taken only while wide. */
        cpu.gpr[A0] = 7;
        CHECK(branch_taken(&cpu, bne_site, bne) == (m == 0),
              "bne keep site, m=%d", m);
        CHECK(branch_taken(&cpu, bne_plain, bne), "unlisted bne, m=%d", m);
        cpu.gpr[A0] = 0;
        CHECK(!branch_taken(&cpu, bne_site, bne), "bne keep site not taken, m=%d", m);
        CHECK(branch_taken(&cpu, blez_site, blez) == (m == 0),
              "blez keep site, m=%d", m);
        CHECK(branch_taken(&cpu, blez_plain, blez), "unlisted blez, m=%d", m);

        /* clip_edge_x_load: edges move out, interior bounds stay. */
        const uint32_t bound = 0x80100000u;
        cpu.gpr[A0] = bound - 0x70u;
        put_half(bound, 0);
        CHECK(load(&cpu, lh_site, lh) == (m ? (uint32_t)-m : 0u), "lh 0, m=%d", m);
        CHECK(load(&cpu, lh_plain, lh) == 0u, "unlisted lh 0, m=%d", m);
        put_half(bound, 320);
        CHECK(load(&cpu, lh_site, lh) == 320u + (uint32_t)m, "lh 320, m=%d", m);
        CHECK(load(&cpu, lhu_site, lhu) == 320u + (uint32_t)m, "lhu 320, m=%d", m);
        CHECK(load(&cpu, lh_plain, lh) == 320u, "unlisted lh 320, m=%d", m);
        put_half(bound, 98);
        CHECK(load(&cpu, lh_site, lh) == 98u, "lh mirror bound 98, m=%d", m);
        put_half(bound, 222);
        CHECK(load(&cpu, lhu_site, lhu) == 222u, "lhu mirror bound 222, m=%d", m);
        put_half(bound, 0xFFFF); /* -1 sign-extends for lh, 65535 for lhu */
        CHECK(load(&cpu, lh_site, lh) == 0xFFFFFFFFu, "lh -1, m=%d", m);
        CHECK(load(&cpu, lhu_site, lhu) == 0xFFFFu, "lhu 65535, m=%d", m);
        put_word(bound, 0);
        CHECK(load(&cpu, lw_site, lw) == (m ? (uint32_t)-m : 0u), "lw 0, m=%d", m);
        put_word(bound, 320);
        CHECK(load(&cpu, lw_site, lw) == 320u + (uint32_t)m, "lw 320, m=%d", m);
    }

    /* A configured width replaces 320 as the right edge. */
    gpu_ws_set_clip_edge_x_load_sites(clip_l, 3, 368);
    gpu_ws_set_margin_override(53);
    const uint32_t bound = 0x80100000u;
    cpu.gpr[A0] = bound - 0x70u;
    put_half(bound, 368);
    CHECK(load(&cpu, lh_site, lh) == 421u, "lh 368 with width 368");
    put_half(bound, 320);
    CHECK(load(&cpu, lh_site, lh) == 320u, "lh 320 is interior with width 368");

    gpu_ws_set_margin_override(-1);
    gpu_ws_set_branch_cull_sites(NULL, 0, NULL, 0, NULL, 0);
    gpu_ws_set_clip_edge_x_load_sites(NULL, 0, 0);
}

static void test_masked_reject(void) {
    CPUState cpu = {0};
    memset(test_ram, 0, sizeof test_ram);
    const uint32_t site = CODE + 0x200;
    const uint32_t word = i_type(5, A0, 0, 3);
    const uint32_t mask = 0xFFFF0000u;
    gpu_ws_set_masked_reject_sites(&site, &word, &mask, 1);
    for (int margin = 0; margin <= 53; margin += 53) {
        gpu_ws_set_margin_override(margin);
        cpu.gpr[A0] = 0xFE00u;
        CHECK(branch_taken(&cpu, site, word) == (margin == 0), "packed X reject at margin %d", margin);
        CHECK(cpu.gpr[A0] == 0xFE00u, "packed cull preserves guest flags");
        CHECK(branch_taken(&cpu, site + 0x40, word), "unlisted packed branch stays vanilla");
        cpu.gpr[V0] = 0;
        CHECK(branch_taken(&cpu, site, i_type(5, A0, V0, 3)), "mismatched word stays vanilla");
        cpu.gpr[A0] = 0xFF00FE00u;
        CHECK(branch_taken(&cpu, site, word), "packed Y reject retained at margin %d", margin);
        cpu.gpr[A0] = 0;
        CHECK(!branch_taken(&cpu, site, word), "zero flags stay visible");
    }
    gpu_ws_set_masked_reject_sites(NULL, NULL, NULL, 0);
    gpu_ws_set_margin_override(-1);
}

static void test_gte_summary_reject(void) {
    CPUState cpu = {0};
    const uint32_t site = CODE + 0x240;
    const uint32_t word = i_type(1, A0, 0, 3);
    const uint32_t mask = 0x7F87A000u;
    gpu_ws_set_masked_reject_sites(&site, &word, &mask, 1);
    for (int margin = 0; margin <= 53; margin += 53) {
        gpu_ws_set_margin_override(margin);
        cpu.gpr[A0] = 0x80004000u;
        CHECK(branch_taken(&cpu, site, word) == (margin == 0), "GTE X-only summary at margin %d", margin);
        CHECK(cpu.gpr[A0] == 0x80004000u, "GTE mask preserves architectural FLAG copy");
        CHECK(branch_taken(&cpu, site + 0x40, word), "unlisted GTE branch stays vanilla");
        CHECK(!branch_taken(&cpu, site, i_type(1, A0, 1, 3)), "wrong GTE full word keeps BGEZ predicate");
        const uint32_t flags[] = {0x80002000u, 0x80020000u, 0x80040000u, 0xC0000000u};
        for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; ++i) {
            cpu.gpr[A0] = flags[i];
            CHECK(branch_taken(&cpu, site, word), "GTE Y/depth flags retain rejection");
        }
        cpu.gpr[A0] = 0x00401000u;
        CHECK(!branch_taken(&cpu, site, word), "positive non-summary flags do not reject");
    }
    gpu_ws_set_masked_reject_sites(NULL, NULL, NULL, 0);
    gpu_ws_set_margin_override(-1);
}

static void test_wide_nclip_branches(void) {
    CPUState cpu = {0};
    ws_xnum = 3; ws_xden = 4; ws_mode = 0;
    ws_precise_nclip_cfg = 1;
    precise_sign_valid = 1; precise_sign = 1;
    const uint32_t sites[] = {CODE+0x300,CODE+0x340,CODE+0x380};
    const uint32_t words[] = {i_type(6,A0,0,3),i_type(7,A0,0,3),i_type(1,A0,1,3)};
    psx_mod_set_native_wide_nclip_sites(sites,words,3);
    for (int margin=0;margin<=53;margin+=53) {
        gpu_ws_set_margin_override(margin);
        cpu.gpr[A0]=0;
        CHECK(branch_taken(&cpu,sites[0],words[0]) == (margin==0), "BLEZ native zero vs precise positive");
        CHECK(branch_taken(&cpu,sites[1],words[1]) == (margin!=0), "BGTZ native zero vs precise positive");
        CHECK(cpu.gpr[A0]==0, "winding helper leaves guest MAC0 register alone");
        cpu.gpr[A0]=(uint32_t)-1;
        CHECK(branch_taken(&cpu,sites[2],words[2]) == (margin!=0), "BGEZ exact keep predicate");
        CHECK(branch_taken(&cpu,sites[0]+4,words[0]), "unlisted twin stays native");
        cpu.gpr[V0]=(uint32_t)-1;
        CHECK(branch_taken(&cpu,sites[0],i_type(6,V0,0,3)), "wrong full word stays native");
    }
    precise_sign_valid=0; gpu_ws_set_margin_override(53); cpu.gpr[A0]=0;
    CHECK(branch_taken(&cpu,sites[0],words[0]), "missing precision stays native");
    precise_sign_valid=1; precise_sign=-1;
    previous_sign_valid=1; previous_sign=1;
    psx_mod_set_native_wide_nclip_previous_site(sites[1],words[1]);
    CHECK(branch_taken(&cpu,sites[1],words[1]), "first quad branch selects preceding positive winding");
    CHECK(!branch_taken(&cpu,sites[2],words[2]), "second quad branch selects latest negative winding");
    previous_sign_valid=0;
    CHECK(!branch_taken(&cpu,sites[1],words[1]), "missing preceding winding cannot use latest result");
    previous_sign_valid=1; gpu_ws_set_margin_override(0);
    CHECK(!branch_taken(&cpu,sites[1],words[1]), "preceding winding is disabled in native 4:3");
    gpu_ws_set_margin_override(53);
    psx_mod_set_native_wide_nclip_sites(sites,words,3);
    CHECK(!branch_taken(&cpu,sites[1],words[1]), "registration resets preceding-result bindings");
    psx_mod_set_native_wide_nclip_sites(NULL,NULL,0);
    ws_precise_nclip_cfg=0; ws_xnum=ws_xden=1;
    gpu_ws_set_margin_override(-1);
}

int main(int argc, char **argv) {
    if (argc > 1) s_capture_path = argv[1];
    test_store();
    test_helpers();
    test_interpreter();
    test_masked_reject();
    test_gte_summary_reject();
    test_wide_nclip_branches();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("PASS explicit cull-site store (sort, de-dup, cap, log) and interpreted "
         "bgez/bltz/branch-keep/clip-edge sites at margin 0 and 53");
    return 0;
}
