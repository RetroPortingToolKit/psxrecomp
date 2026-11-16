/* Checkpoint capture and resume for replays; see tas_checkpoint.h. */
#include "tas_checkpoint.h"

#ifndef PSX_NO_DEBUG_TOOLS

#include "boot_state.h"
#include "interrupts.h"
#include "memcard.h"
#include "psx_cycles.h"
#include "psx_memory.h"
#include "psx_scheduler.h"
#include "psx_sha256.h"
#include "savestate.h"
#include "source_stateio_identity.h"
#include "source_tas_stateio.h"
#include "timers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint64_t s_frame_count;

#define TAS_PATH_MAX 4096
#define TAS_RAM_BYTES 2097152u   /* the digest covers main RAM, 2 MiB */

static int env_set(const char *name) {
    const char *v = getenv(name);
    return v && *v;
}

/* The input route a capture was made with. Master feeds input over the debug
 * server, so without a route file there is nothing to hash: "none". */
static int route_id(char out[65]) {
    if (!env_set("PSX_INPUT_ROUTE_FILE")) {
        snprintf(out, 65, "%s", PSX_TAS_STATEIO_NO_CARD);
        return 1;
    }
    return source_stateio_route_sha256(out);
}

static void hex32(const uint8_t digest[32], char out[65]) {
    for (unsigned i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

static int file_size(const char *path, unsigned long long *bytes) {
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) { fclose(f); return 0; }
    fclose(f);
    *bytes = (unsigned long long)n;
    return 1;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* PSX_TAS_SAVE_STATE_PATH with "{frame}" replaced by the six-digit frame. */
static int state_path_for(unsigned frame, char *out, size_t cap) {
    const char *p = getenv("PSX_TAS_SAVE_STATE_PATH");
    const char *tok;
    int n;
    if (!p || !*p) return 0;
    tok = strstr(p, "{frame}");
    if (!tok) n = snprintf(out, cap, "%s", p);
    else n = snprintf(out, cap, "%.*s%06u%s", (int)(tok - p), p, frame, tok + 7);
    return n > 0 && (size_t)n < cap;
}

int tas_checkpoint_capture(const CPUState *snap, uint32_t bios_checksum, uint32_t entry_pc,
                           unsigned frame, unsigned requested_frame, const char *path) {
    static uint8_t image[PSX_TAS_STATEIO_CARD_BYTES];
    TasStateManifest m;
    char manifest_path[TAS_PATH_MAX + 8], state_sha[65];
    if (!snap || !path || !*path || requested_frame == 0 || requested_frame > frame) return 0;
    if (snprintf(manifest_path, sizeof manifest_path, "%s.json", path) >= (int)sizeof manifest_path)
        return 0;
    if (file_exists(manifest_path)) {
        fprintf(stderr, "[tas-checkpoint] capture refused at frame %u: %s exists\n", frame,
                manifest_path);
        return 0;
    }
    memset(&m, 0, sizeof m);
    m.frame = frame;
    m.requested_frame = requested_frame;
    m.cycle = psx_cycle_count;
    m.bios_checksum = bios_checksum;
    m.entry_pc = entry_pc;
    m.ram_digest = source_tas_stateio_ram_digest(memory_get_ram_ptr(), TAS_RAM_BYTES);
    snprintf(m.input_observer_sha256, sizeof m.input_observer_sha256, "%s",
             PSX_TAS_STATEIO_NO_CARD);
    if (!source_stateio_config_digest_hex(m.config_digest) ||
        !source_stateio_exe_sha256(m.exe_sha256) || !route_id(m.route_sha256)) {
        fprintf(stderr, "[tas-checkpoint] capture refused at frame %u: cannot hash the run's identity\n",
                frame);
        return 0;
    }
    if (!boot_state_save(snap, bios_checksum, entry_pc, path) ||
        !source_stateio_file_sha256(path, state_sha) || !file_size(path, &m.state_bytes)) {
        fprintf(stderr, "[tas-checkpoint] capture failed at frame %u: %s\n", frame, path);
        return 0;
    }
    /* Card images are host files, not state sections. */
    for (int slot = 0; slot < 2; slot++) {
        char card_path[TAS_PATH_MAX + 16];
        uint8_t digest[32];
        FILE *card;
        int ok;
        if (!memcard_is_present(slot)) {
            snprintf(m.card_sha256[slot], sizeof m.card_sha256[slot], "%s", PSX_TAS_STATEIO_NO_CARD);
            continue;
        }
        if (!source_tas_stateio_card_path(card_path, sizeof card_path, path, slot) ||
            memcard_export_raw(slot, image) != 0) {
            fprintf(stderr, "[tas-checkpoint] capture refused at frame %u: cannot read card %d\n",
                    frame, slot + 1);
            return 0;
        }
        /* No manifest names an existing image here, so it is a leftover. */
        if (file_exists(card_path)) (void)remove(card_path);
        card = psx_file_create_new(card_path, 1);
        ok = card && fwrite(image, 1, sizeof image, card) == sizeof image;
        if (card && fclose(card) != 0) ok = 0;
        if (!ok) {
            fprintf(stderr, "[tas-checkpoint] capture refused at frame %u: cannot write %s\n", frame,
                    card_path);
            return 0;
        }
        psx_sha256_compute(image, sizeof image, digest);
        hex32(digest, m.card_sha256[slot]);
    }
    if (!source_tas_stateio_manifest_write(manifest_path, &m, path, state_sha)) {
        fprintf(stderr, "[tas-checkpoint] capture failed at frame %u: cannot write %s\n", frame,
                manifest_path);
        return 0;
    }
    fprintf(stderr, "[tas-checkpoint] captured frame %u (requested %u) cycle %llu RAM %016llX -> %s\n",
            frame, requested_frame, (unsigned long long)m.cycle, (unsigned long long)m.ram_digest,
            path);
    return 1;
}

static unsigned s_requested_frame;   /* earliest frame awaiting a capture, 0 = none */

static void capture_at_boundary(const CPUState *snap, uint32_t bios_checksum, uint32_t entry_pc) {
    char path[TAS_PATH_MAX];
    const unsigned frame = (unsigned)s_frame_count;
    const unsigned requested = s_requested_frame;
    s_requested_frame = 0;
    if (!state_path_for(frame, path, sizeof path)) {
        fprintf(stderr, "[tas-checkpoint] capture refused at frame %u: PSX_TAS_SAVE_STATE_PATH unset or too long\n",
                frame);
        return;
    }
    (void)tas_checkpoint_capture(snap, bios_checksum, entry_pc, frame, requested, path);
}

void tas_checkpoint_on_frame(uint64_t frame) {
    if (s_requested_frame || frame == 0 || frame > 0xFFFFFFFFu ||
        !source_tas_stateio_save_at_match((unsigned)frame))
        return;
    s_requested_frame = (unsigned)frame;
    if (!savestate_request_checkpoint(capture_at_boundary)) {
        s_requested_frame = 0;
        fprintf(stderr, "[tas-checkpoint] capture at frame %u could not be staged\n",
                (unsigned)frame);
    }
}

/* One restored value changed on purpose, after the gate. */
static int perturb(const char *field) {
    if (!strcmp(field, "ram")) {
        memory_get_ram_ptr()[0x10000] ^= 0x01u;
    } else if (!strcmp(field, "irq_timing")) {
        PsxIrqResumeTiming t;
        interrupts_resume_timing_get(&t);
        t.cooldown_until += 1;
        interrupts_resume_timing_set(&t);
    } else if (!strcmp(field, "sched")) {
        psx_scheduler_set_return_tcb(psx_scheduler_return_tcb() ^ 0x4u);
    } else if (!strcmp(field, "timer2")) {
        uint16_t counter[3], target[3];
        uint32_t mode[3], frac[3];
        int32_t irq_line[3];
        timers_get_snapshot(counter, mode, target, irq_line, frac);
        counter[2] = (uint16_t)(counter[2] + 1u);
        timers_set_snapshot(counter, mode, target, irq_line, frac);
    } else if (!strcmp(field, "memctrl")) {
        uint32_t w[PSX_MEMORY_CONTROL_WORDS];
        memory_control_get(w);
        w[0] ^= 0x1u;
        memory_control_set(w);
    } else {
        return 0;
    }
    return 1;
}

static int refuse(char *reason, size_t cap, const char *text) {
    source_tas_stateio_reject(reason, cap, text);
    return 0;
}

int tas_checkpoint_resume(CPUState *cpu, const char *state_path, const char *manifest_path,
                          char *reason, size_t reason_cap) {
    static char text[32768];
    static uint8_t image[PSX_TAS_STATEIO_CARD_BYTES];
    TasStateManifest m;
    char json_path[TAS_PATH_MAX + 8], recorded_path[TAS_PATH_MAX + 1];
    char cfg[65], exe[65], route[65], want_sha[65], have_sha[65];
    uint32_t bios_checksum = 0, entry_pc = 0;
    const char *perturb_field = getenv("PSX_TAS_PERTURB_RESTORE");
    const int compatible = env_set("PSX_TAS_RESUME_COMPATIBLE_BUILD") &&
                           !strcmp(getenv("PSX_TAS_RESUME_COMPATIBLE_BUILD"), "1");
    if (!cpu || !state_path || !*state_path) return refuse(reason, reason_cap, "no state path");
    if (!manifest_path || !*manifest_path) {
        if (snprintf(json_path, sizeof json_path, "%s.json", state_path) >= (int)sizeof json_path)
            return refuse(reason, reason_cap, "state path too long");
        manifest_path = json_path;
    }
    if (!source_tas_stateio_read_text(manifest_path, text, sizeof text))
        return refuse(reason, reason_cap, "cannot read the manifest");
    if (!source_tas_stateio_manifest_parse(text, &m, recorded_path, sizeof recorded_path) ||
        !source_tas_stateio_find_field(text, "state_sha256", want_sha, sizeof want_sha))
        return refuse(reason, reason_cap, "malformed manifest");
    if (perturb_field && *perturb_field && strcmp(perturb_field, "ram") &&
        strcmp(perturb_field, "irq_timing") && strcmp(perturb_field, "sched") && strcmp(perturb_field, "timer2") &&
        strcmp(perturb_field, "memctrl"))
        return refuse(reason, reason_cap, "unknown PSX_TAS_PERTURB_RESTORE field");

    /* Identity before anything is loaded. */
    if (!source_stateio_config_digest_hex(cfg) || strcmp(cfg, m.config_digest))
        return refuse(reason, reason_cap, "configuration digest mismatch");
    if (!source_stateio_exe_sha256(exe) || !source_tas_stateio_binary_accept(&m, exe, compatible))
        return refuse(reason, reason_cap, "runtime binary mismatch");
    if (!route_id(route) || strcmp(route, m.route_sha256))
        return refuse(reason, reason_cap, "input route mismatch");
    if (!source_stateio_file_sha256(state_path, have_sha) || strcmp(have_sha, want_sha))
        return refuse(reason, reason_cap, "state file does not match its manifest");
    for (int slot = 0; slot < 2; slot++) {
        const int want_card = strcmp(m.card_sha256[slot], PSX_TAS_STATEIO_NO_CARD) != 0;
        uint8_t digest[32];
        char hex[65];
        if (want_card != memcard_is_present(slot))
            return refuse(reason, reason_cap, slot ? "card 2 presence differs" : "card 1 presence differs");
        if (!want_card) continue;
        if (memcard_export_raw(slot, image) != 0)
            return refuse(reason, reason_cap, "cannot read an inserted card");
        psx_sha256_compute(image, sizeof image, digest);
        hex32(digest, hex);
        if (strcmp(hex, m.card_sha256[slot]))
            return refuse(reason, reason_cap,
                          slot ? "card 2 differs from the checkpoint's image"
                               : "card 1 differs from the checkpoint's image");
    }

    savestate_get_integrity(&bios_checksum, &entry_pc);
    if (!boot_state_load(state_path, bios_checksum, entry_pc, cpu))
        return refuse(reason, reason_cap, "state did not load");
    psx_cycles_resync_after_restore(cpu);
    interrupts_resync_after_restore();
    if (!source_tas_stateio_manifest_accept_mode(&m, m.frame, psx_cycle_count,
            source_tas_stateio_ram_digest(memory_get_ram_ptr(), TAS_RAM_BYTES),
            bios_checksum, entry_pc, cfg, exe, route, compatible, reason, reason_cap))
        return 0;
    s_frame_count = m.frame;
    if (perturb_field && *perturb_field) {
        (void)perturb(perturb_field);
        fprintf(stderr, "[tas-checkpoint] perturbed restored %s\n", perturb_field);
    }
    return 1;
}

int tas_checkpoint_start(CPUState *cpu) {
    const char *state = getenv("PSX_TAS_RESUME_STATE");
    char exe[65], reason[160];
    if (!env_set("PSX_TAS_SAVE_STATE_AT") && !(state && *state)) return 0;
    /* Hash the executable now, not at the first capture, which would stall
     * the run for as long as hashing a large title takes. */
    if (!source_stateio_exe_sha256(exe)) {
        fprintf(stderr, "psxrecomp: [tas-checkpoint] cannot hash the running executable\n");
        return 2;
    }
    if (!(state && *state)) return 0;
    if (!tas_checkpoint_resume(cpu, state, getenv("PSX_TAS_RESUME_MANIFEST"), reason, sizeof reason)) {
        fprintf(stderr, "psxrecomp: [tas-checkpoint] resume rejected: %s\n", reason);
        return 2;
    }
    {
        extern int psx_scheduler_arm_resume(uint32_t resume_pc);
        if (!psx_scheduler_arm_resume(cpu->pc)) {
            fprintf(stderr, "psxrecomp: [tas-checkpoint] resume PC 0x%08X is not dispatchable\n",
                    (unsigned)cpu->pc);
            return 2;
        }
    }
    fprintf(stdout, "psxrecomp: [tas-checkpoint] resumed frame %llu cycle %llu from %s\n",
            (unsigned long long)s_frame_count, (unsigned long long)psx_cycle_count, state);
    return 0;
}

#else /* PSX_NO_DEBUG_TOOLS */

int tas_checkpoint_start(CPUState *cpu) { (void)cpu; return 0; }
void tas_checkpoint_on_frame(uint64_t frame) { (void)frame; }

#endif
