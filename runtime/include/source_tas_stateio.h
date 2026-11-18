#ifndef PSX_SOURCE_TAS_STATEIO_H
#define PSX_SOURCE_TAS_STATEIO_H

/*
 * Checkpoint identity binding: a debug feature for replays, off by default.
 *
 * A checkpoint is only usable if it can be shown to belong to the exact build,
 * BIOS, entry point, frame and guest state it claims. This header owns that
 * proof: a small JSON manifest written beside the .pst state, and an
 * accept/reject gate. The runtime calls (boot_state save and load) live at
 * the call sites, so this file stays dependency-light and unit-testable.
 *
 * Environment contract (read by the call sites, not here):
 *   PSX_TAS_SAVE_STATE_AT=<frame>[,<frame>...]  save at these frames
 *   PSX_TAS_SAVE_STATE_PATH=<file.pst>          state path
 *   PSX_TAS_RESUME_STATE=<file.pst>             restore this state at startup
 *   PSX_TAS_RESUME_MANIFEST=<file.json>         its manifest (default: state path + ".json")
 *
 * A resumed run is admitted only when the restored state reproduces the
 * manifest's frame, cycle and RAM digest, and the manifest was produced under
 * the same BIOS, entry point, configuration, executable and input route.
 * Each refusal carries a reason.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx_file_create_new.h"

/* v3 adds the memory card images, which live in host files rather than in any
 * boot_state section: a checkpoint taken after the guest writes its card must
 * resume with that card, not with the route's initial image. */
#define PSX_TAS_STATEIO_SCHEMA "psx-tas-stateio-v3"
#define PSX_TAS_STATEIO_CARD_BYTES 131072u
#define PSX_TAS_STATEIO_NO_CARD "none"
/* Bump when a checkpoint changes representation or what its saved bytes
 * mean. --resume-compatible-build trusts this id alone to let a rebuilt
 * runtime continue a state, and boot_state's own section checks see layout,
 * not meaning. It never admits a different codegen or profile on its own:
 * the configuration digest still has to match. */
#define PSX_TAS_STATEIO_COMPATIBILITY "psx-tas-checkpoint-1"

/* Guest RAM accessor (memory.c). Declared here so the manifest/digest call
 * sites — including C++ — share one C-linkage declaration. */
#ifdef __cplusplus
extern "C" {
#endif
uint8_t *memory_get_ram_ptr(void);
#ifdef __cplusplus
}
#endif

typedef struct TasStateManifest {
    unsigned           frame;
    /* The earliest requested frame this state satisfies. Equal to frame
     * unless the request landed where the state could not represent the
     * instruction continuation and the capture moved to a later frame. */
    unsigned           requested_frame;
    unsigned           input_consumed;
    uint64_t           cycle;
    uint64_t           ram_digest;
    uint32_t           bios_checksum;
    uint32_t           entry_pc;
    unsigned long long state_bytes;
    /* The whole resolved configuration, not a hand-written flag list, plus
     * the two things a flag list can never cover: the exact runtime binary and
     * the exact input route content. */
    char               config_digest[65];
    char               exe_sha256[65];
    char               route_sha256[65];
    char               compatibility[65];
    /* SHA-256 of each slot's card image, saved beside the state as
     * <state>.card1.mcd / <state>.card2.mcd, or PSX_TAS_STATEIO_NO_CARD. */
    char               card_sha256[2][65];
    /* SHA-256 of an input-observer history saved beside the state, or
     * PSX_TAS_STATEIO_NO_CARD when the run kept none. */
    char               input_observer_sha256[65];
    unsigned           disc_number;
} TasStateManifest;

/* <state>.cardN.mcd for slot 0 or 1. Returns 1 when the path fits. */
static inline int source_tas_stateio_card_path(char *out, size_t cap, const char *state_path, int slot) {
    return out && state_path && (slot == 0 || slot == 1) &&
           snprintf(out, cap, "%s.card%d.mcd", state_path, slot + 1) < (int)cap;
}

/* PSX_* variables that may legitimately DIFFER between a save and a resume.
 * Everything else the runtime reads is part of the identity, so a newly added
 * model flag is covered automatically instead of being forgotten. */
static inline int source_tas_stateio_env_allowed_to_differ(const char *key, size_t klen) {
    static const char *const allow[] = {
        "PSX_INPUT_ROUTE_FILE",         /* path only; its content is hashed
                                           separately as route_sha256 */
        "PSX_TAS_SAVE_STATE_AT",        /* checkpoint request         */
        "PSX_TAS_SAVE_STATE_PATH",      /* checkpoint request         */
        "PSX_TAS_RESUME_STATE",         /* resume request             */
        "PSX_TAS_RESUME_MANIFEST",      /* resume request             */
        "PSX_TAS_RESUME_COMPATIBLE_BUILD", /* explicit diagnostic mode */
        "PSX_TAS_PERTURB_RESTORE",      /* negative-control injection */
        "PSX_LOAD_SLOT",                /* user slot load request     */
        NULL
    };
    for (int i = 0; allow[i]; i++)
        if (strlen(allow[i]) == klen && strncmp(key, allow[i], klen) == 0) return 1;
    return 0;
}

/* FNV-1a over guest RAM — same family as the RAM page probe, cheap enough to
 * run once per checkpoint. */
static inline uint64_t source_tas_stateio_ram_digest(const uint8_t *ram, size_t bytes) {
    uint64_t h = UINT64_C(14695981039346656037);
    if (!ram) return 0;
    for (size_t i = 0; i < bytes; ++i) h = (h ^ (uint64_t)ram[i]) * UINT64_C(1099511628211);
    return h;
}

/* Order-independent digest over the PSX_* entries of a NULL-terminated
 * "KEY=VALUE" environment array. Entries are SORTED first and then hashed as one
 * stream, so enumeration order cannot change the result and (unlike a
 * XOR-of-hashes) distinct configurations cannot cancel each other out. */
static inline uint64_t source_tas_stateio_env_digest(const char *const *env) {
    uint64_t acc = 0;
    if (!env) return 0;
    for (size_t i = 0; env[i]; i++) {
        const char *e = env[i];
        const char *eq = strchr(e, '=');
        size_t klen;
        if (!eq) continue;
        klen = (size_t)(eq - e);
        if (klen < 4 || strncmp(e, "PSX_", 4) != 0) continue;
        if (source_tas_stateio_env_allowed_to_differ(e, klen)) continue;
        acc ^= source_tas_stateio_ram_digest((const uint8_t *)e, strlen(e));
    }
    return acc;
}

/* Collect the identity-relevant PSX_* entries into `out` (up to `cap`), sorted by
 * byte order. Returns the count. Used for the sha256 configuration digest. */
static inline size_t source_tas_stateio_env_collect(const char *const *env,
                                                   const char **out, size_t cap) {
    size_t n = 0;
    if (!env) return 0;
    for (size_t i = 0; env[i] && n < cap; i++) {
        const char *e = env[i];
        const char *eq = strchr(e, '=');
        size_t klen;
        if (!eq) continue;
        klen = (size_t)(eq - e);
        if (klen < 4 || strncmp(e, "PSX_", 4) != 0) continue;
        if (source_tas_stateio_env_allowed_to_differ(e, klen)) continue;
        out[n++] = e;
    }
    for (size_t a = 1; a < n; a++) {          /* insertion sort; n is tiny */
        const char *v = out[a];
        size_t b = a;
        while (b > 0 && strcmp(out[b - 1], v) > 0) { out[b] = out[b - 1]; --b; }
        out[b] = v;
    }
    return n;
}
static inline void source_tas_stateio_hex64(uint64_t v, char out[17]) {
    snprintf(out, 17, "%016llX", (unsigned long long)v);
}

static inline int source_tas_stateio_manifest_write(const char *path, const TasStateManifest *m,
                                             const char *state_path, const char *state_sha256) {
    FILE *f;
    int ok;
    char escaped[24577]; size_t used=0;
    if (!path || !m || !state_path) return 0;
    if (strlen(state_path)>4096u) return 0;
    for (const unsigned char *p=(const unsigned char *)state_path;*p;p++) {
        if (*p=='"' || *p=='\\') { escaped[used++]='\\'; escaped[used++]=(char)*p; }
        else if (*p<32u) { snprintf(escaped+used,7,"\\u%04x",*p); used+=6; }
        else escaped[used++]=(char)*p;
    }
    escaped[used]=0;
    f = psx_file_create_new(path, 1);
    if (!f) return 0;
    ok = fprintf(f,
                 "{\n"
                 "  \"schema\": \"%s\",\n"
                 "  \"frame\": %u,\n"
                 "  \"requested_frame\": %u,\n"
                 "  \"input_consumed\": %u,\n"
                 "  \"cycle\": %llu,\n"
                 "  \"ram_digest\": \"%016llX\",\n"
                 "  \"bios_checksum\": %u,\n"
                 "  \"entry_pc\": %u,\n"
                 "  \"state_path\": \"%s\",\n"
                 "  \"state_sha256\": \"%s\",\n"
                 "  \"state_bytes\": %llu,\n"
                 "  \"config_digest\": \"%s\",\n"
                 "  \"exe_sha256\": \"%s\",\n"
                 "  \"route_sha256\": \"%s\",\n"
                 "  \"compatibility\": \"%s\",\n"
                 "  \"card1_sha256\": \"%s\",\n"
                 "  \"card2_sha256\": \"%s\",\n"
                 "  \"input_observer_sha256\": \"%s\",\n"
                 "  \"disc_number\": %u\n"
                 "}\n",
                 PSX_TAS_STATEIO_SCHEMA, m->frame, m->requested_frame ? m->requested_frame : m->frame,
                 m->input_consumed, (unsigned long long)m->cycle,
                 (unsigned long long)m->ram_digest, m->bios_checksum, m->entry_pc,
                 escaped, state_sha256 ? state_sha256 : "", m->state_bytes,
                 m->config_digest, m->exe_sha256, m->route_sha256,
                 PSX_TAS_STATEIO_COMPATIBILITY,
                 m->card_sha256[0][0] ? m->card_sha256[0] : PSX_TAS_STATEIO_NO_CARD,
                 m->card_sha256[1][0] ? m->card_sha256[1] : PSX_TAS_STATEIO_NO_CARD,
                 m->input_observer_sha256[0] ? m->input_observer_sha256 : PSX_TAS_STATEIO_NO_CARD,
                 m->disc_number) > 0;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* Extract a JSON scalar by key. Values may be quoted strings or bare numbers.
 * Deliberately minimal (strstr-based) so it has no parser dependency; the
 * manifest is written by this same module, not user-authored. */
static inline int source_tas_stateio_find_field(const char *text, const char *key,
                                         char *out, size_t cap) {
    char needle[64];
    const char *p;
    size_t n = 0;
    if (!text || !key || !out || cap == 0) return 0;
    if (snprintf(needle, sizeof needle, "\"%s\"", key) >= (int)sizeof needle) return 0;
    p = strstr(text, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != ':') return 0;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"' && *p != '\n' && n + 1 < cap) {
            if (*p=='\\') {
                ++p;
                if (*p=='u') {
                    unsigned value=0;
                    for(unsigned i=1;i<=4;i++) {
                        unsigned char c=(unsigned char)p[i];
                        if (!c) return 0;
                        unsigned digit=c>='0'&&c<='9'?(unsigned)(c-'0'):c>='a'&&c<='f'?c-'a'+10u:c>='A'&&c<='F'?c-'A'+10u:16u;
                        if(digit>15u)return 0;
                        value=value*16u+digit;
                    }
                    if (!value || value>127u) return 0;
                    out[n++]=(char)value;p+=5;continue;
                }
                if (*p!='\\' && *p!='"' && *p!='/')return 0;
            }
            out[n++]=*p++;
        }
        if (*p!='"')return 0;
    } else {
        while (*p && *p != ',' && *p != '\n' && *p != '}' && *p != ' ' && n + 1 < cap)
            out[n++] = *p++;
    }
    out[n] = '\0';
    return n > 0;
}

static inline int source_tas_stateio_parse_u64(const char *text, const char *key, uint64_t *out) {
    char buf[64];
    char *end;
    unsigned long long v;
    if (!source_tas_stateio_find_field(text, key, buf, sizeof buf)) return 0;
    v = strtoull(buf, &end, 10);
    if (end == buf || *end) return 0;
    *out = (uint64_t)v;
    return 1;
}

/* ram_digest is written as 16 hex digits; parse it base 16. */
static inline int source_tas_stateio_parse_digest(const char *text, uint64_t *out) {
    char buf[64];
    char *end;
    unsigned long long v;
    if (!source_tas_stateio_find_field(text, "ram_digest", buf, sizeof buf)) return 0;
    v = strtoull(buf, &end, 16);
    if (end == buf || *end) return 0;
    *out = (uint64_t)v;
    return 1;
}

/* Parse a manifest. Returns 1 only for a complete, schema-matching record. */
static inline int source_tas_stateio_manifest_parse(const char *text, TasStateManifest *out,
                                             char *state_path, size_t state_path_cap) {
    char schema[64];
    uint64_t v;
    if (!text || !out) return 0;
    memset(out, 0, sizeof *out);
    if (state_path && state_path_cap) state_path[0] = '\0';
    if (!source_tas_stateio_find_field(text, "schema", schema, sizeof schema)) return 0;
    if (strcmp(schema, PSX_TAS_STATEIO_SCHEMA) != 0) return 0;
    if (!source_tas_stateio_parse_u64(text, "frame", &v) || v > 0xFFFFFFFFull) return 0;
    out->frame = (unsigned)v;
    if (!source_tas_stateio_parse_u64(text, "requested_frame", &v) || v == 0 || v > out->frame) return 0;
    out->requested_frame = (unsigned)v;
    if (!source_tas_stateio_parse_u64(text, "input_consumed", &v) || v > 0xFFFFFFFFull) return 0;
    out->input_consumed = (unsigned)v;
    if (!source_tas_stateio_parse_u64(text, "cycle", &out->cycle)) return 0;
    if (!source_tas_stateio_parse_digest(text, &out->ram_digest)) return 0;
    if (!source_tas_stateio_parse_u64(text, "bios_checksum", &v) || v > 0xFFFFFFFFull) return 0;
    out->bios_checksum = (uint32_t)v;
    if (!source_tas_stateio_parse_u64(text, "entry_pc", &v) || v > 0xFFFFFFFFull) return 0;
    out->entry_pc = (uint32_t)v;
    /* state_bytes is unsigned long long (printed with %llu); uint64_t is
     * unsigned long on LP64 Linux, so parse through the uint64_t temporary. */
    if (!source_tas_stateio_parse_u64(text, "state_bytes", &v)) return 0;
    out->state_bytes = (unsigned long long)v;
    if (!source_tas_stateio_find_field(text, "config_digest", out->config_digest,
                                       sizeof out->config_digest)) return 0;
    if (!source_tas_stateio_find_field(text, "exe_sha256", out->exe_sha256,
                                       sizeof out->exe_sha256)) return 0;
    if (!source_tas_stateio_find_field(text, "route_sha256", out->route_sha256,
                                       sizeof out->route_sha256)) return 0;
    if (state_path && state_path_cap) {
        if (!source_tas_stateio_find_field(text, "state_path", state_path, state_path_cap))
            return 0;
    }
    /* Older manifests remain same-binary only. */
    (void)source_tas_stateio_find_field(text, "compatibility", out->compatibility,
                                       sizeof out->compatibility);
    for (int slot = 0; slot < 2; ++slot) {
        const char *key = slot ? "card2_sha256" : "card1_sha256";
        size_t n;
        if (!source_tas_stateio_find_field(text, key, out->card_sha256[slot], sizeof out->card_sha256[slot]))
            return 0;
        n = strlen(out->card_sha256[slot]);
        if (strcmp(out->card_sha256[slot], PSX_TAS_STATEIO_NO_CARD) != 0 &&
            (n != 64 || strspn(out->card_sha256[slot], "0123456789abcdef") != 64))
            return 0;
    }
    if(!source_tas_stateio_find_field(text,"input_observer_sha256",out->input_observer_sha256,
                                     sizeof out->input_observer_sha256))
        strcpy(out->input_observer_sha256,PSX_TAS_STATEIO_NO_CARD);
    if(strcmp(out->input_observer_sha256,PSX_TAS_STATEIO_NO_CARD) &&
       (strlen(out->input_observer_sha256)!=64u || strspn(out->input_observer_sha256,"0123456789abcdef")!=64u))return 0;
    if(source_tas_stateio_find_field(text,"disc_number",schema,sizeof schema)) {
        if(!source_tas_stateio_parse_u64(text,"disc_number",&v)||v>8u)return 0;
        out->disc_number=(unsigned)v;
    }
    return 1;
}

static inline int source_tas_stateio_binary_accept(const TasStateManifest *m,
                                                   const char *exe, int compatible) {
    return m && exe && *exe &&
        (!strcmp(m->exe_sha256, exe) ||
         (compatible && !strcmp(m->compatibility, PSX_TAS_STATEIO_COMPATIBILITY)));
}

static inline void source_tas_stateio_reject(char *reason, size_t cap, const char *text) {
    if (reason && cap) snprintf(reason, cap, "%s", text ? text : "rejected");
}

/* The gate. Returns 1 on accept, else 0 with a reason. The observed_* values
 * are what the resumed run actually reproduced. */
static inline int source_tas_stateio_manifest_accept_mode(const TasStateManifest *m,
                                              unsigned frame, uint64_t cycle,
                                              uint64_t ram_digest, uint32_t bios_checksum,
                                              uint32_t entry_pc,
                                              const char *config_digest,
                                              const char *exe_sha256,
                                              const char *route_sha256,
                                              int compatible_build,
                                              char *reason, size_t reason_cap) {
    if (!m) { source_tas_stateio_reject(reason, reason_cap, "missing manifest"); return 0; }
    if (m->frame != frame) {
        source_tas_stateio_reject(reason, reason_cap, "frame mismatch");
        return 0;
    }
    if (m->cycle != cycle) {
        source_tas_stateio_reject(reason, reason_cap, "cycle mismatch");
        return 0;
    }
    if (m->ram_digest != ram_digest) {
        source_tas_stateio_reject(reason, reason_cap, "restored RAM digest mismatch");
        return 0;
    }
    if (m->bios_checksum != bios_checksum) {
        source_tas_stateio_reject(reason, reason_cap, "foreign BIOS state");
        return 0;
    }
    if (m->entry_pc != entry_pc) {
        source_tas_stateio_reject(reason, reason_cap, "foreign entry point");
        return 0;
    }
    if (strcmp(m->config_digest, config_digest ? config_digest : "") != 0) {
        source_tas_stateio_reject(reason, reason_cap, "configuration digest mismatch");
        return 0;
    }
    if (!source_tas_stateio_binary_accept(m, exe_sha256, compatible_build)) {
        source_tas_stateio_reject(reason, reason_cap, "runtime binary mismatch");
        return 0;
    }
    if (strcmp(m->route_sha256, route_sha256 ? route_sha256 : "") != 0) {
        source_tas_stateio_reject(reason, reason_cap, "input route mismatch");
        return 0;
    }
    if (reason && reason_cap) reason[0] = '\0';
    return 1;
}

static inline int source_tas_stateio_manifest_accept(const TasStateManifest *m,
        unsigned frame, uint64_t cycle, uint64_t ram_digest, uint32_t bios_checksum,
        uint32_t entry_pc, const char *config_digest, const char *exe_sha256,
        const char *route_sha256, char *reason, size_t reason_cap) {
    return source_tas_stateio_manifest_accept_mode(m, frame, cycle, ram_digest,
        bios_checksum, entry_pc, config_digest, exe_sha256, route_sha256, 0,
        reason, reason_cap);
}

/* Read a whole small file (manifest) into caller storage. Returns bytes read
 * or 0 on failure. */
static inline size_t source_tas_stateio_read_text(const char *path, char *out, size_t cap) {
    FILE *f;
    size_t n;
    if (!path || !out || cap < 2) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    n = fread(out, 1, cap - 1, f);
    if (ferror(f)) n = 0;
    fclose(f);
    out[n] = '\0';
    return n;
}

/* Parse the save-at frame list from the environment. Comma-separated, so one
 * run can checkpoint at K and K+1. An empty, malformed or zero entry list is
 * disabled. */
static inline int source_tas_stateio_save_at_match(unsigned frame) {
    const char *e = getenv("PSX_TAS_SAVE_STATE_AT");
    if (!e || !*e || frame == 0) return 0;
    while (*e) {
        char *end;
        unsigned long v = strtoul(e, &end, 10);
        if (end == e) return 0;
        if (v == (unsigned long)frame) return 1;
        if (*end != ',') return 0;
        e = end + 1;
    }
    return 0;
}

#endif /* PSX_SOURCE_TAS_STATEIO_H */
