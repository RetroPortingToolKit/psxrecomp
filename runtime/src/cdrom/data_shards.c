/* data_shards.c — memoized pure-function replay ("post-decompression shards").
 * See data_shards.h and docs/DATA_SHARDS.md for the design and soundness
 * model. Summary of the lifecycle:
 *
 *   enter(key):
 *     - shard whose read-set byte-matches live RAM?  -> REPLAY:
 *         apply write-set (through psx_write_* so dirty-code tracking, text
 *         guard, card traps etc. all fire exactly as a real run would),
 *         mark executable ranges like the CD-DMA path does, restore the
 *         caller-visible registers, credit the recorded guest cycles in
 *         slices through psx_advance_cycles + psx_check_interrupts_at
 *         (events and IRQs land at their authentic guest cycle), publish
 *         pc = $ra, return 1.
 *     - no shard and recorder idle -> ARM capture, return 0 (body runs
 *         natively; the memory chokepoints feed the recorder).
 *   ret(): at the hooked function's own jr $ra with the armed sp -> finalize:
 *     build read/write range lists from the byte maps, snapshot the
 *     caller-visible registers and cycle cost, store + persist.
 *
 * Purity poisons (capture aborted, never replayed): MMIO access outside
 * exception context, DMA write to RAM inside the window, a green-thread
 * switch inside the window (other threads' work would leak into the trace),
 * host-frame escape (longjmp/RestoreState), window byte budget exceeded.
 *
 * Known v1 fidelity notes (documented in DATA_SHARDS.md):
 *  - cycle cost includes any ISR cycles that ran inside the capture window
 *    (replay re-runs ISRs live, so those cycles are credited ~twice; small).
 *  - an IRQ handler that fires mid-replay observes the write-set complete.
 *  - GTE/COP0 state changes by the function are not captured (target
 *    functions are data transforms; a GTE-using function must not be listed).
 */
#include "data_shards.h"
#include "psx_cycles.h"
#include "interrupts.h"
#include "crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#endif

extern uint8_t *memory_get_ram_ptr(void);
extern uint8_t *memory_get_scratchpad_ptr(void);
extern void psx_write_word(uint32_t addr, uint32_t val);
extern void psx_write_byte(uint32_t addr, uint8_t val);
extern void dirty_ram_mark_executable_range(uint32_t addr, uint32_t len);

#define DS_RAM_BYTES   (2u * 1024u * 1024u)
#define DS_SP_BYTES    1024u
#define DS_SPACE       (DS_RAM_BYTES + DS_SP_BYTES)   /* unified index space */
#define DS_PAGE_SHIFT  12
#define DS_NPAGES      ((DS_SPACE + (1u << DS_PAGE_SHIFT) - 1) >> DS_PAGE_SHIFT)
#define DS_MAX_BYTES   (4u * 1024u * 1024u)  /* window write+read budget */
#define DS_REG_COUNT   19                    /* at,v0,v1,a0-a3,t0-t7,t8,t9,hi,lo */

/* caller-visible register set restored on replay */
static const uint8_t k_ds_gprs[17] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,24,25};

typedef struct { uint32_t idx, len; } DsRange;

typedef struct DsShard {
    struct DsShard* next;
    uint32_t key;
    uint32_t args[4];          /* a0-a3 at entry (prefilter) */
    uint32_t rd_crc;           /* crc of read-set bytes (prefilter) */
    uint32_t nrd, nwr;
    DsRange* rd;               /* read-set ranges (unified idx) */
    uint8_t* rd_bytes;         /* concatenated expected bytes */
    uint32_t rd_total;
    DsRange* wr;
    uint8_t* wr_bytes;
    uint32_t wr_total;
    uint32_t regs[DS_REG_COUNT];
    uint64_t cyc_cost;
    uint32_t hits;
} DsShard;

#define DS_KEY_BUCKETS 64
static DsShard* s_buckets[DS_KEY_BUCKETS];
static int s_loaded_keys[256]; static int s_loaded_n = 0;  /* keys with disk load done */

volatile int g_ds_recording = 0;
/* Fail closed. A configured hook is only a candidate boundary; replay must be
 * enabled explicitly after capture/verification for the current experiment.
 * This prevents an unsafe persisted candidate from mutating boot assets before
 * the debug/control plane is available to disable it. */
static int   s_enabled = 0;

/* ---- recorder state ---- */
static CPUState* s_arm_cpu = 0;
static uint32_t  s_arm_key = 0, s_arm_sp = 0, s_arm_ra = 0;
static uint32_t  s_arm_args[4];
static uint64_t  s_arm_cyc0 = 0;
static int       s_poisoned = 0;
static uint32_t  s_bytes_touched = 0;

static uint8_t* s_rd_flag = 0;   /* 1 = read before written */
static uint8_t* s_wr_flag = 0;   /* 1 = written */
static uint8_t* s_rd_orig = 0;   /* byte value at first (pre-write) read */
static uint8_t  s_page_touched[DS_NPAGES];
static uint32_t s_pages[DS_NPAGES]; static uint32_t s_npages = 0;

/* ---- stats (TCP `data_shards`) ---- */
static struct {
    uint64_t enters, replays, captures_armed, captures_ok, captures_poisoned;
    uint64_t verify_fail, bytes_replayed;
    uint64_t cyc_credited;
    uint32_t last_key, last_poison_key, shards;
    uint32_t poison_mmio, poison_dma, poison_thread, poison_budget, poison_stale;
} s_st;

static const char* s_cache_dir = 0;
static char s_dir_buf[1024];

void ds_set_enabled(int on) { s_enabled = on; }

void ds_init(const char* cache_dir, const char* game_id) {
    if (!cache_dir || !game_id) return;
    snprintf(s_dir_buf, sizeof(s_dir_buf), "%s/%s/datashards", cache_dir, game_id);
    s_cache_dir = s_dir_buf;
#ifdef _WIN32
    /* best-effort mkdir -p */
    char acc[1100]; int n = 0;
    for (const char* p = s_dir_buf; ; p++) {
        if (*p == '/' || *p == '\\' || *p == 0) {
            acc[n] = 0;
            if (n > 2) _mkdir(acc);
            if (*p == 0) break;
        }
        if (n < (int)sizeof(acc) - 1) acc[n++] = (*p == '/') ? '\\' : *p;
    }
#endif
}

/* ---------- unified index helpers ---------- */

/* virtual addr -> unified index, or -1 (untracked: ROM/open bus), or -2 (MMIO) */
static int64_t ds_index_of(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < 0x00800000u) return (int64_t)(phys & (DS_RAM_BYTES - 1u));
    if (phys >= 0x1F800000u && phys < 0x1F800000u + DS_SP_BYTES)
        return (int64_t)(DS_RAM_BYTES + (phys - 0x1F800000u));
    if (phys >= 0x1F801000u && phys < 0x1F803000u) return -2;   /* MMIO */
    if (phys >= 0x1FC00000u) return -1;                          /* BIOS ROM: constant */
    return -2;  /* anything else device-ish: treat as impure */
}

static uint8_t ds_live_byte(uint32_t idx) {
    return idx < DS_RAM_BYTES ? memory_get_ram_ptr()[idx]
                              : memory_get_scratchpad_ptr()[idx - DS_RAM_BYTES];
}

static void ds_touch_page(uint32_t idx) {
    uint32_t pg = idx >> DS_PAGE_SHIFT;
    if (!s_page_touched[pg]) { s_page_touched[pg] = 1; s_pages[s_npages++] = pg; }
}

static void ds_poison(uint32_t which) {
    if (!g_ds_recording || s_poisoned) return;
    s_poisoned = 1;
    s_st.captures_poisoned++;
    s_st.last_poison_key = s_arm_key;
    switch (which) {
    case 1: s_st.poison_mmio++;   break;
    case 2: s_st.poison_dma++;    break;
    case 3: s_st.poison_thread++; break;
    case 4: s_st.poison_budget++; break;
    default: s_st.poison_stale++; break;
    }
}

/* ---------- chokepoint feeds ---------- */

void ds_note_read(uint32_t addr, uint32_t size) {
    if (!g_ds_recording || s_poisoned) return;
    int64_t ix = ds_index_of(addr);
    if (ix == -1) return;
    if (ix == -2) { ds_poison(1); return; }
    uint32_t idx = (uint32_t)ix;
    for (uint32_t i = 0; i < size; i++, idx++) {
        if (s_wr_flag[idx] || s_rd_flag[idx]) continue;
        s_rd_flag[idx] = 1;
        s_rd_orig[idx] = ds_live_byte(idx);
        ds_touch_page(idx);
        if (++s_bytes_touched > DS_MAX_BYTES) { ds_poison(4); return; }
    }
}

void ds_note_write(uint32_t addr, uint32_t size) {
    if (!g_ds_recording || s_poisoned) return;
    int64_t ix = ds_index_of(addr);
    if (ix == -1) return;
    if (ix == -2) { ds_poison(1); return; }
    uint32_t idx = (uint32_t)ix;
    for (uint32_t i = 0; i < size; i++, idx++) {
        if (!s_wr_flag[idx]) {
            s_wr_flag[idx] = 1;
            ds_touch_page(idx);
            if (++s_bytes_touched > DS_MAX_BYTES) { ds_poison(4); return; }
        }
    }
}

void ds_note_mmio(uint32_t addr, int is_read) {
    (void)addr; (void)is_read;
    ds_poison(1);
}

void ds_note_dma_write(void) { ds_poison(2); }

/* traps.c calls this on every guest thread switch */
void ds_note_thread_switch(void) { if (g_ds_recording) ds_poison(3); }

/* ---------- capture arm / finalize ---------- */

static void ds_recorder_reset(void) {
    for (uint32_t i = 0; i < s_npages; i++) {
        uint32_t base = s_pages[i] << DS_PAGE_SHIFT;
        uint32_t len = (1u << DS_PAGE_SHIFT);
        if (base + len > DS_SPACE) len = DS_SPACE - base;
        memset(s_rd_flag + base, 0, len);
        memset(s_wr_flag + base, 0, len);
        s_page_touched[s_pages[i]] = 0;
    }
    s_npages = 0;
    s_bytes_touched = 0;
    s_poisoned = 0;
}

static int ds_alloc_maps(void) {
    if (s_rd_flag) return 1;
    s_rd_flag = (uint8_t*)calloc(1, DS_SPACE);
    s_wr_flag = (uint8_t*)calloc(1, DS_SPACE);
    s_rd_orig = (uint8_t*)malloc(DS_SPACE);
    return s_rd_flag && s_wr_flag && s_rd_orig;
}

static DsShard** ds_bucket(uint32_t key) {
    return &s_buckets[(key >> 2) & (DS_KEY_BUCKETS - 1)];
}

/* Build ranges from a flag map over the touched pages. Returns count. */
static uint32_t ds_build_ranges(const uint8_t* flag, DsRange** out_ranges,
                                uint8_t** out_bytes, const uint8_t* byte_src,
                                int bytes_from_live, uint32_t* out_total) {
    /* sort touched pages so ranges come out ordered and mergeable */
    for (uint32_t i = 1; i < s_npages; i++) {         /* insertion sort: n <= 1025 */
        uint32_t v = s_pages[i]; uint32_t j = i;
        while (j > 0 && s_pages[j-1] > v) { s_pages[j] = s_pages[j-1]; j--; }
        s_pages[j] = v;
    }
    uint32_t nr = 0, total = 0;
    DsRange* ranges = 0; uint32_t cap = 0;
    int64_t open = -1; uint32_t open_start = 0;
    uint32_t prev_end = 0; int have_prev = 0;
    for (uint32_t p = 0; p < s_npages; p++) {
        uint32_t base = s_pages[p] << DS_PAGE_SHIFT;
        uint32_t end = base + (1u << DS_PAGE_SHIFT);
        if (end > DS_SPACE) end = DS_SPACE;
        if (have_prev && base != prev_end && open >= 0) {
            /* page gap closes any open run */
            if (nr == cap) { cap = cap ? cap * 2 : 64; ranges = (DsRange*)realloc(ranges, cap * sizeof(DsRange)); }
            ranges[nr].idx = open_start; ranges[nr].len = prev_end - open_start;
            total += ranges[nr].len; nr++;
            open = -1;
        }
        for (uint32_t i = base; i < end; i++) {
            if (flag[i]) {
                if (open < 0) { open = 1; open_start = i; }
            } else if (open >= 0) {
                if (nr == cap) { cap = cap ? cap * 2 : 64; ranges = (DsRange*)realloc(ranges, cap * sizeof(DsRange)); }
                ranges[nr].idx = open_start; ranges[nr].len = i - open_start;
                total += ranges[nr].len; nr++;
                open = -1;
            }
        }
        prev_end = end; have_prev = 1;
    }
    if (open >= 0) {
        if (nr == cap) { cap = cap + 1; ranges = (DsRange*)realloc(ranges, cap * sizeof(DsRange)); }
        ranges[nr].idx = open_start; ranges[nr].len = prev_end - open_start;
        total += ranges[nr].len; nr++;
    }
    uint8_t* blob = (uint8_t*)malloc(total ? total : 1);
    uint32_t off = 0;
    for (uint32_t r = 0; r < nr; r++)
        for (uint32_t i = 0; i < ranges[r].len; i++, off++)
            blob[off] = bytes_from_live ? ds_live_byte(ranges[r].idx + i)
                                        : byte_src[ranges[r].idx + i];
    *out_ranges = ranges; *out_bytes = blob; *out_total = total;
    return nr;
}

static void ds_persist(const DsShard* s);

static void ds_finalize(CPUState* cpu) {
    DsShard* s = (DsShard*)calloc(1, sizeof(DsShard));
    s->key = s_arm_key;
    memcpy(s->args, s_arm_args, sizeof(s->args));
    s->nrd = ds_build_ranges(s_rd_flag, &s->rd, &s->rd_bytes, s_rd_orig, 0, &s->rd_total);
    s->nwr = ds_build_ranges(s_wr_flag, &s->wr, &s->wr_bytes, 0, 1, &s->wr_total);
    s->rd_crc = crc32_compute(s->rd_bytes, s->rd_total);
    for (int i = 0; i < 17; i++) s->regs[i] = cpu->gpr[k_ds_gprs[i]];
    s->regs[17] = cpu->hi; s->regs[18] = cpu->lo;
    s->cyc_cost = psx_get_cycle_count() - s_arm_cyc0;
    DsShard** b = ds_bucket(s->key);
    s->next = *b; *b = s;
    s_st.captures_ok++; s_st.shards++;
    ds_persist(s);
}

/* ---------- persistence ---------- */

static void ds_persist(const DsShard* s) {
    if (!s_cache_dir) return;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%08X_%08X.dss", s_cache_dir, s->key, s->rd_crc);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[8] = { 0x31534450u /* 'PDS1' */, s->key,
                        (uint32_t)s->cyc_cost, (uint32_t)(s->cyc_cost >> 32),
                        s->nrd, s->nwr, s->rd_total, s->wr_total };
    fwrite(hdr, 4, 8, f);
    fwrite(s->args, 4, 4, f);
    fwrite(s->regs, 4, DS_REG_COUNT, f);
    fwrite(s->rd, sizeof(DsRange), s->nrd, f);
    fwrite(s->rd_bytes, 1, s->rd_total, f);
    fwrite(s->wr, sizeof(DsRange), s->nwr, f);
    fwrite(s->wr_bytes, 1, s->wr_total, f);
    fclose(f);
}

static void ds_load_key(uint32_t key) {
    for (int i = 0; i < s_loaded_n; i++) if (s_loaded_keys[i] == (int)key) return;
    if (s_loaded_n < 256) s_loaded_keys[s_loaded_n++] = (int)key;
    if (!s_cache_dir) return;
#ifdef _WIN32
    char pat[1200];
    snprintf(pat, sizeof(pat), "%s/%08X_*.dss", s_cache_dir, key);
    struct _finddata_t fd; intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return;
    do {
        char path[1400];
        snprintf(path, sizeof(path), "%s/%s", s_cache_dir, fd.name);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        uint32_t hdr[8];
        if (fread(hdr, 4, 8, f) == 8 && hdr[0] == 0x31534450u && hdr[1] == key) {
            DsShard* s = (DsShard*)calloc(1, sizeof(DsShard));
            s->key = key;
            s->cyc_cost = (uint64_t)hdr[2] | ((uint64_t)hdr[3] << 32);
            s->nrd = hdr[4]; s->nwr = hdr[5]; s->rd_total = hdr[6]; s->wr_total = hdr[7];
            s->rd = (DsRange*)malloc(s->nrd * sizeof(DsRange));
            s->wr = (DsRange*)malloc(s->nwr * sizeof(DsRange));
            s->rd_bytes = (uint8_t*)malloc(s->rd_total ? s->rd_total : 1);
            s->wr_bytes = (uint8_t*)malloc(s->wr_total ? s->wr_total : 1);
            int ok = fread(s->args, 4, 4, f) == 4
                  && fread(s->regs, 4, DS_REG_COUNT, f) == DS_REG_COUNT
                  && fread(s->rd, sizeof(DsRange), s->nrd, f) == s->nrd
                  && fread(s->rd_bytes, 1, s->rd_total, f) == s->rd_total
                  && fread(s->wr, sizeof(DsRange), s->nwr, f) == s->nwr
                  && fread(s->wr_bytes, 1, s->wr_total, f) == s->wr_total;
            if (ok) {
                s->rd_crc = crc32_compute(s->rd_bytes, s->rd_total);
                DsShard** b = ds_bucket(key);
                s->next = *b; *b = s;
                s_st.shards++;
            } else {
                free(s->rd); free(s->wr); free(s->rd_bytes); free(s->wr_bytes); free(s);
            }
        }
        fclose(f);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#endif
}

/* ---------- replay ---------- */

static int ds_verify(const DsShard* s) {
    uint32_t off = 0;
    for (uint32_t r = 0; r < s->nrd; r++) {
        uint32_t idx = s->rd[r].idx, len = s->rd[r].len;
        const uint8_t* live = idx < DS_RAM_BYTES
            ? memory_get_ram_ptr() + idx
            : memory_get_scratchpad_ptr() + (idx - DS_RAM_BYTES);
        /* a range never straddles the RAM/scratch boundary by construction */
        if (memcmp(live, s->rd_bytes + off, len) != 0) return 0;
        off += len;
    }
    return 1;
}

static void ds_apply(CPUState* cpu, DsShard* s) {
    uint32_t off = 0;
    for (uint32_t r = 0; r < s->nwr; r++) {
        uint32_t idx = s->wr[r].idx, len = s->wr[r].len;
        if (idx < DS_RAM_BYTES) {
            /* write through the chokepointed byte path so dirty-code tracking,
             * text guard, card traps and cosim all see the stores; then mark
             * the executable range exactly like the CD-DMA delivery path. */
            uint32_t va = 0x80000000u + idx;
            for (uint32_t i = 0; i < len; i++)
                psx_write_byte(va + i, s->wr_bytes[off + i]);
            dirty_ram_mark_executable_range(va, len);
        } else {
            uint32_t va = 0x1F800000u + (idx - DS_RAM_BYTES);
            for (uint32_t i = 0; i < len; i++)
                psx_write_byte(va + i, s->wr_bytes[off + i]);
        }
        off += len;
    }
    for (int i = 0; i < 17; i++) cpu->gpr[k_ds_gprs[i]] = s->regs[i];
    cpu->hi = s->regs[17]; cpu->lo = s->regs[18];
    s_st.bytes_replayed += s->wr_total;
    /* cycle credit: slices through the normal machinery so timers/VBlank/CD
     * events and IRQ delivery land at their authentic guest cycles. */
    uint32_t ra = cpu->gpr[31];
    uint64_t remaining = s->cyc_cost;
    while (remaining) {
        uint32_t chunk = remaining > 2048u ? 2048u : (uint32_t)remaining;
        psx_advance_cycles(chunk);
        remaining -= chunk;
        psx_check_interrupts_at(cpu, ra);
    }
    s_st.cyc_credited += s->cyc_cost;
}

/* ---------- gen-time hooks ---------- */

int psx_datashard_enter(CPUState* cpu, uint32_t key) {
    if (!s_enabled) return 0;
    s_st.enters++; s_st.last_key = key;

    if (g_ds_recording) {
        /* nested/stale window: a hooked function re-entered (recursion or an
         * abnormal exit skipped ret). Abort the outer capture, run natively. */
        if (key == s_arm_key || cpu != s_arm_cpu) ds_poison(5);
        return 0;
    }

    ds_load_key(key);
    for (DsShard* s = *ds_bucket(key); s; s = s->next) {
        if (s->key != key) continue;
        if (s->args[0] != cpu->gpr[4] || s->args[1] != cpu->gpr[5] ||
            s->args[2] != cpu->gpr[6] || s->args[3] != cpu->gpr[7]) continue;
        if (ds_verify(s)) {
            s->hits++; s_st.replays++;
            ds_apply(cpu, s);
            cpu->pc = cpu->gpr[31];   /* CPS: publish $ra, caller resumes */
            return 1;
        }
        s_st.verify_fail++;
    }

    if (!ds_alloc_maps()) return 0;
    ds_recorder_reset();
    s_arm_cpu = cpu; s_arm_key = key;
    s_arm_sp = cpu->gpr[29]; s_arm_ra = cpu->gpr[31];
    s_arm_args[0] = cpu->gpr[4]; s_arm_args[1] = cpu->gpr[5];
    s_arm_args[2] = cpu->gpr[6]; s_arm_args[3] = cpu->gpr[7];
    s_arm_cyc0 = psx_get_cycle_count();
    g_ds_recording = 1;
    s_st.captures_armed++;
    return 0;
}

void psx_datashard_ret(CPUState* cpu) {
    if (!g_ds_recording || cpu != s_arm_cpu) return;
    if (cpu->gpr[29] != s_arm_sp || cpu->gpr[31] != s_arm_ra) return; /* callee's ret */
    g_ds_recording = 0;
    if (!s_poisoned) ds_finalize(cpu);
    ds_recorder_reset();
}

/* ---------- debug surface ---------- */

void ds_stats_json(char* buf, int cap) {
    snprintf(buf, (size_t)cap,
        "\"enabled\":%d,\"enters\":%llu,\"replays\":%llu,"
        "\"captures_armed\":%llu,\"captures_ok\":%llu,\"captures_poisoned\":%llu,"
        "\"verify_fail\":%llu,\"shards\":%u,\"bytes_replayed\":%llu,"
        "\"cyc_credited\":%llu,\"recording\":%d,\"last_key\":\"0x%08X\","
        "\"last_poison_key\":\"0x%08X\",\"poison_mmio\":%u,\"poison_dma\":%u,"
        "\"poison_thread\":%u,\"poison_budget\":%u,\"poison_stale\":%u",
        s_enabled,
        (unsigned long long)s_st.enters, (unsigned long long)s_st.replays,
        (unsigned long long)s_st.captures_armed, (unsigned long long)s_st.captures_ok,
        (unsigned long long)s_st.captures_poisoned,
        (unsigned long long)s_st.verify_fail, s_st.shards,
        (unsigned long long)s_st.bytes_replayed,
        (unsigned long long)s_st.cyc_credited,
        g_ds_recording, s_st.last_key, s_st.last_poison_key,
        s_st.poison_mmio, s_st.poison_dma, s_st.poison_thread,
        s_st.poison_budget, s_st.poison_stale);
}
