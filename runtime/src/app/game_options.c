/* game_options.c — see game_options.h.
 *
 * Persists a game's native OPTION-screen settings across launches (issue #5),
 * for titles (e.g. Tomba) that keep them only in a per-boot RAM global with no
 * memory-card config block. Two halves, both deterministic — no polling, no
 * timing heuristics:
 *
 *   RESTORE (once, at init): the recompiler rewrites each setting's boot-init
 *     store (sb/sh) to route its value through psx_game_option_store(addr,val).
 *     When the game runs that init, the store writes our PERSISTED value instead
 *     of the default (or `val` unchanged if nothing is persisted). The in-OPTION
 *     write is a different instruction, untouched, so the player can still change
 *     settings in-game.
 *   SAVE (on exit): game_options_save_now() reads the current globals and writes
 *     them to <memcard_dir>/<game_id>.options. Registered via atexit, so a clean
 *     exit captures whatever the player last set in the OPTION screen.
 *
 * Guest RAM via psx_read_byte; no debug server — works in release builds. The
 * field set is declared per game in game_options.toml (read by both the
 * recompiler, for the hook PCs, and here, for the addresses + saved values).
 */
#include "game_options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t psx_read_byte(uint32_t addr);   /* memory.c */

#define GO_MAX_FIELDS 32

/* State-file format version. Written as `format_version=N` on save; on load a
 * file declaring a HIGHER version than this is rejected wholesale (a format we
 * don't understand). A missing version is treated as legacy 0 and still accepted
 * (the field set is name-keyed + value-validated, so old files stay safe). Bump
 * this only on an incompatible format change. */
#define GO_FORMAT_VERSION 1

static int      g_count = 0;
static uint32_t g_addr[GO_MAX_FIELDS];
static uint8_t  g_size[GO_MAX_FIELDS];
static char     g_name[GO_MAX_FIELDS][32];
static int32_t  g_vmin[GO_MAX_FIELDS];       /* inclusive valid range (restore)   */
static int32_t  g_vmax[GO_MAX_FIELDS];
static char     g_path[1024];

static int      g_have_file = 0;             /* a saved state file was loaded     */
static uint32_t g_loaded[GO_MAX_FIELDS];     /* values from the state file        */
static uint8_t  g_loaded_valid[GO_MAX_FIELDS]; /* in-range AND matched a field    */
static int      g_dbg_fopen = 0;             /* load_state_file: fopen succeeded  */
static int      g_dbg_matched = 0;           /* load_state_file: in-range matches */
static int      g_dbg_rejected = 0;          /* values dropped (out of range)     */
static int      g_dbg_ver_bad = 0;           /* file rejected: version too new    */

static uint32_t read_field(int i) {
    uint32_t v = 0;
    for (int b = 0; b < g_size[i]; b++)
        v |= (uint32_t)psx_read_byte(g_addr[i] + (uint32_t)b) << (8 * b);
    return v;
}

/* Load "name=value" lines into g_loaded[] (matched by name), validating each
 * value against its declared range and honouring a format_version line. A value
 * outside [g_vmin,g_vmax] is DROPPED (the field keeps its default), so a corrupt
 * or stale file can never inject an out-of-range value; a file declaring a newer
 * format_version than we understand is rejected wholesale. */
static void load_state_file(void) {
    g_have_file = 0;
    g_dbg_fopen = 0;
    g_dbg_matched = 0;
    g_dbg_rejected = 0;
    g_dbg_ver_bad = 0;
    for (int i = 0; i < g_count; i++) g_loaded_valid[i] = 0;
    FILE *f = fopen(g_path, "r");
    if (!f) return;
    g_dbg_fopen = 1;
    char line[256];
    int matched = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        long val = strtol(eq + 1, NULL, 0);
        /* Reserved key: a file from a newer (incompatible) format is dropped. */
        if (strcmp(line, "format_version") == 0) {
            if (val > GO_FORMAT_VERSION) { g_dbg_ver_bad = 1; fclose(f); return; }
            continue;
        }
        for (int i = 0; i < g_count; i++) {
            if (strcmp(line, g_name[i]) != 0) continue;
            if ((long)val < (long)g_vmin[i] || (long)val > (long)g_vmax[i]) {
                g_dbg_rejected++;            /* out of range -> keep the default */
            } else {
                g_loaded[i] = (uint32_t)val;
                g_loaded_valid[i] = 1;
                matched++;
            }
            break;
        }
    }
    fclose(f);
    g_dbg_matched = matched;
    if (matched > 0) g_have_file = 1;
}

void game_options_configure(const char *state_path,
                            const uint32_t *addrs,
                            const uint8_t  *sizes,
                            const char *const *names,
                            const int32_t  *vmins,
                            const int32_t  *vmaxs,
                            int count) {
    g_count = 0;
    if (!state_path || count <= 0) return;
    if (count > GO_MAX_FIELDS) count = GO_MAX_FIELDS;
    snprintf(g_path, sizeof(g_path), "%s", state_path);
    for (int i = 0; i < count; i++) {
        g_addr[i] = addrs[i];
        g_size[i] = (sizes[i] == 2) ? 2 : 1;
        snprintf(g_name[i], sizeof(g_name[i]), "%s", names[i] ? names[i] : "f");
        g_vmin[i] = vmins ? vmins[i] : (int32_t)0x80000000; /* INT32_MIN default */
        g_vmax[i] = vmaxs ? vmaxs[i] : (int32_t)0x7FFFFFFF; /* INT32_MAX default */
    }
    g_count = count;
    load_state_file();
}

/* Recompiler hook: at each config global's boot-init store, the generated code
 * calls this with the store's target address and the default value being stored.
 * Return the persisted value for that address if we have one, else the default
 * (so a fresh install is byte-identical). Address is matched modulo the KUSEG/
 * KSEG region bits. Called from the emu thread during init only. */
int psx_game_option_store(uint32_t addr, int val) {
    if (!g_have_file) return val;
    uint32_t a = addr & 0x1FFFFFFFu;
    for (int i = 0; i < g_count; i++)
        if ((g_addr[i] & 0x1FFFFFFFu) == a)
            return g_loaded_valid[i] ? (int)g_loaded[i] : val; /* dropped -> default */
    return val;
}

/* Save on exit (atexit). Reads the current globals — which reflect the player's
 * last OPTION-screen choices — and writes the state file. Skips writing if the
 * game never booted far enough to initialise any field AND there is no prior
 * file, so a quit during early boot can't stamp an all-zero file. */
void game_options_save_now(void) {
    if (g_count <= 0) return;
    uint32_t cur[GO_MAX_FIELDS];
    int any = 0;
    for (int i = 0; i < g_count; i++) { cur[i] = read_field(i); if (cur[i]) any = 1; }
    if (!any && !g_have_file) return;
    FILE *f = fopen(g_path, "w");
    if (!f) return;
    fputs("# Persistent game options (issue #5). Written by the runtime on exit.\n", f);
    fprintf(f, "format_version=%d\n", GO_FORMAT_VERSION);
    for (int i = 0; i < g_count; i++)
        fprintf(f, "%s=%u\n", g_name[i], (unsigned)cur[i]);
    fclose(f);
}

/* JSON state dump for the debug server. Path uses forward slashes to stay valid
 * JSON. */
int game_options_debug_json(char *out, int cap) {
    char p[1024];
    snprintf(p, sizeof(p), "%s", g_path);
    for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
    int n = snprintf(out, cap,
        "{\"count\":%d,\"have_file\":%d,\"fopen\":%d,\"matched\":%d,\"rejected\":%d,"
        "\"version_rejected\":%d,\"format_version\":%d,\"path\":\"%s\",\"fields\":[",
        g_count, g_have_file, g_dbg_fopen, g_dbg_matched, g_dbg_rejected,
        g_dbg_ver_bad, GO_FORMAT_VERSION, p);
    for (int i = 0; i < g_count && n < cap; i++) {
        n += snprintf(out + n, cap - n,
            "%s{\"name\":\"%s\",\"addr\":\"0x%08X\",\"size\":%d,\"min\":%d,\"max\":%d,"
            "\"loaded\":%u,\"valid\":%d,\"cur\":%u}",
            i ? "," : "", g_name[i], g_addr[i], g_size[i], g_vmin[i], g_vmax[i],
            (unsigned)g_loaded[i], g_loaded_valid[i], (unsigned)read_field(i));
    }
    n += snprintf(out + n, (cap - n) > 0 ? cap - n : 0, "]}");
    return n;
}
