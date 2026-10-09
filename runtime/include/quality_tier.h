#pragma once
/* quality_tier.h -- graphics preset autodetection (docs/QUALITY_PRESETS.md).
 *
 * Pure policy: describe the host (CPU, cores, RAM, GPU name, Steam Deck)
 * and get the preset a title's [quality.*] tables should start from. The
 * probes that fill PsxHostInfo live in quality_probe.c; this file does no
 * I/O so the rules are unit-testable (tests/test_quality_tier.c). */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PSX_QUALITY_LOW    = 0,
    PSX_QUALITY_MEDIUM = 1,
    PSX_QUALITY_HIGH   = 2,
    PSX_QUALITY_ULTRA  = 3,
    PSX_QUALITY_COUNT  = 4,
    PSX_QUALITY_CUSTOM = 4,      /* the player changed a preset-governed setting */
    PSX_QUALITY_NONE   = -1
};

typedef struct PsxHostInfo {
    char     cpu[128];         /* CPU brand string ("Apple M1", "AMD Custom APU 0405") */
    char     gpu[128];         /* GPU name ("NVIDIA GeForce RTX 4080 SUPER"); "" unknown */
    char     gpu_id[32];       /* stable id used for the fingerprint when gpu is unknown */
    int      logical_cores;    /* 0 unknown */
    uint64_t ram_mb;           /* 0 unknown */
    int      steam_deck;       /* 1: Steam Deck hardware (DMI / SteamDeck env) */
} PsxHostInfo;

/* "low" .. "ultra", "custom"; NULL for anything else. */
const char *psx_quality_name(int tier);
/* Case-insensitive; PSX_QUALITY_NONE when unknown. */
int psx_quality_from_name(const char *name);

/* The preset this host should start from, and one line saying why
 * ("AMD integrated GPU: Low"). Ultra unless the class cannot hold 60 Hz even
 * at the dynamic floor; unknown hardware gets Ultra. */
int psx_quality_classify(const PsxHostInfo *h, char *reason, size_t reason_cap);

/* Nearest preset the title offers (bit i of offered_mask = tier i): the
 * highest offered tier <= want, else the lowest offered above it. NONE when
 * the mask is empty. */
int psx_quality_pick_offered(int want, unsigned offered_mask);

/* 16 hex digits identifying CPU, core count, RAM (to the GB) and GPU, so a
 * new machine or GPU triggers a fresh detection. Driver updates do not. */
void psx_quality_fingerprint(const PsxHostInfo *h, char out[17]);

/* quality_probe.c: OS facts only (CPU brand, threads, RAM, Windows/macOS
 * GPU name, Linux GPU PCI id, Steam Deck DMI). Cheap; runs every launch. */
void psx_quality_probe_host(PsxHostInfo *h);
/* GL_RENDERER from a hidden 1x1 window; needs SDL video initialised.
 * 1 on success. Only for a due detection with no OS GPU name. */
int psx_quality_probe_gl_renderer(char *out, size_t cap);

/* Human summary for the launcher: "Apple M1 · 8 cores · 8 GB". */
void psx_quality_summary(const PsxHostInfo *h, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
