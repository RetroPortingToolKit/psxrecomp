/* quality_tier.c -- graphics preset autodetection rules (quality_tier.h).
 *
 * Ultra is the default; a GPU class, a thread count or memory size steps a
 * machine down only where Ultra cannot hold 60 Hz even at its dynamic floor
 * (docs/QUALITY_PRESETS.md). */
#include "quality_tier.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *const kNames[] = {"low", "medium", "high", "ultra", "custom"};
static const char *const kLabels[] = {"Low", "Medium", "High", "Ultra", "Custom"};

const char *psx_quality_name(int tier) {
    return (tier >= 0 && tier <= PSX_QUALITY_CUSTOM) ? kNames[tier] : NULL;
}

int psx_quality_from_name(const char *name) {
    if (!name) return PSX_QUALITY_NONE;
    for (int i = 0; i <= PSX_QUALITY_CUSTOM; i++) {
        const char *a = name, *b = kNames[i];
        while (*a && *b && tolower((unsigned char)*a) == *b) { a++; b++; }
        if (!*a && !*b) return i;
    }
    return PSX_QUALITY_NONE;
}

static void lower_copy(const char *in, char *out, size_t cap) {
    size_t i = 0;
    for (; in && in[i] && i + 1 < cap; i++) out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = 0;
}

static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

/* "apple m<N>[ pro| max| ultra]" -> generation N and variant 0 base, 1 pro,
 * 2 max/ultra. 0 when not Apple silicon. */
static int apple_gen(const char *s, int *variant) {
    const char *p = strstr(s, "apple m");
    *variant = 0;
    if (!p) return 0;
    p += 7;
    int gen = 0;
    while (isdigit((unsigned char)*p)) gen = gen * 10 + (*p++ - '0');
    if (has(p, "max") || has(p, "ultra")) *variant = 2;
    else if (has(p, "pro")) *variant = 1;
    return gen;
}

/* Ultra is the default (owner decision 2026-10-08): dynamic resolution
 * (down to native x1), Smooth motion from surplus only and the adaptive
 * aspect absorb most hardware live. A GPU class steps down only when Ultra
 * cannot hold 60 Hz even at that floor: software GL, the Steam Deck's APU,
 * mobile GPUs and the weakest integrated GPUs (Intel HD/UHD, small AMD
 * "Radeon Graphics"/Vega iGPUs) go to Low; mid integrated GPUs to Medium.
 * Returns -1 when the name says nothing we know (=> Ultra). */
static int gpu_tier(const char *g, const char **why) {
    int variant = 0;
    if (apple_gen(g, &variant)) {
        *why = "Apple silicon";
        return PSX_QUALITY_ULTRA;
    }
    if (has(g, "llvmpipe") || has(g, "softpipe") || has(g, "swiftshader") ||
        has(g, "basic render") || has(g, "gdi generic") || has(g, "software")) {
        *why = "software rendering";
        return PSX_QUALITY_LOW;
    }
    if (has(g, "vangogh") || has(g, "van gogh") || has(g, "custom gpu 0405") ||
        has(g, "custom gpu 0932")) {
        *why = "Steam Deck GPU";
        return PSX_QUALITY_LOW;
    }
    if (has(g, "mali") || has(g, "adreno") || has(g, "powervr") || has(g, "videocore")) {
        *why = "mobile GPU";
        return PSX_QUALITY_LOW;
    }
    if (has(g, "nvidia") || has(g, "geforce") || has(g, "quadro") || has(g, "rtx")) {
        *why = "NVIDIA GPU";
        if (has(g, " mx") || has(g, "gt ") || has(g, "gt1") || has(g, "gt7")) return PSX_QUALITY_MEDIUM;
        return PSX_QUALITY_ULTRA;
    }
    if (has(g, "intel")) {
        if (has(g, "arc") && !has(g, "arc(tm) graphics") && !has(g, "arc graphics")) {
            *why = "Intel Arc GPU";
            return PSX_QUALITY_ULTRA;
        }
        *why = "Intel integrated GPU";
        if (has(g, "iris") || has(g, "xe") || has(g, "arc")) return PSX_QUALITY_MEDIUM;
        return PSX_QUALITY_LOW;          /* HD / UHD Graphics */
    }
    if (has(g, "radeon") || has(g, "amd") || has(g, "ati ")) {
        if (has(g, " rx ") || has(g, "rx 4") || has(g, "rx 5") || has(g, "rx 6") ||
            has(g, "rx 7") || has(g, "rx 9") || has(g, "pro w") || has(g, "vega 56") ||
            has(g, "vega 64")) {
            *why = "AMD Radeon GPU";
            return PSX_QUALITY_ULTRA;
        }
        *why = "AMD integrated GPU";
        if (has(g, "680m") || has(g, "760m") || has(g, "780m") || has(g, "880m") ||
            has(g, "890m") || has(g, "8060s") || has(g, "8050s"))
            return PSX_QUALITY_MEDIUM;
        return PSX_QUALITY_LOW;          /* Vega 3..11, "Radeon(TM) Graphics" */
    }
    return -1;
}

int psx_quality_classify(const PsxHostInfo *h, char *reason, size_t reason_cap) {
    char g[160], c[160];
    lower_copy(h ? h->gpu : "", g, sizeof g);
    lower_copy(h ? h->cpu : "", c, sizeof c);
    const char *why = "unknown GPU";
    int tier = -1;
    const int deck = h && (h->steam_deck || has(c, "custom apu 0405") || has(c, "custom apu 0932"));
    if (deck) {
        tier = PSX_QUALITY_LOW;
        why = "Steam Deck";
    }
    if (tier < 0 && g[0]) tier = gpu_tier(g, &why);
    if (tier < 0) tier = gpu_tier(c, &why);      /* Apple: the CPU names the GPU */
    if (tier < 0) { tier = PSX_QUALITY_ULTRA; why = "unknown GPU"; }

    /* Only what the dynamic systems cannot absorb: too few CPU threads for
     * the emulation + render threads, or too little memory. */
    const char *cap_why = NULL;
    if (h && h->logical_cores > 0 && h->logical_cores < 4) {
        if (tier > PSX_QUALITY_LOW) { tier = PSX_QUALITY_LOW; cap_why = "fewer than 4 CPU threads"; }
    } else if (h && h->logical_cores > 0 && h->logical_cores == 4) {
        if (tier > PSX_QUALITY_MEDIUM) { tier = PSX_QUALITY_MEDIUM; cap_why = "4 CPU threads"; }
    }
    if (h && h->ram_mb > 0 && h->ram_mb < 6u * 1024u) {
        if (tier > PSX_QUALITY_LOW) { tier = PSX_QUALITY_LOW; cap_why = "under 6 GB of memory"; }
    }
    if (reason && reason_cap) {
        if (cap_why)
            snprintf(reason, reason_cap, "%s, capped by %s: %s", why, cap_why, kLabels[tier]);
        else
            snprintf(reason, reason_cap, "%s: %s", why, kLabels[tier]);
    }
    return tier;
}

int psx_quality_pick_offered(int want, unsigned offered_mask) {
    if (!(offered_mask & 0xFu)) return PSX_QUALITY_NONE;
    if (want < 0) want = 0;
    if (want >= PSX_QUALITY_COUNT) want = PSX_QUALITY_COUNT - 1;
    for (int t = want; t >= 0; t--)
        if (offered_mask & (1u << t)) return t;
    for (int t = want + 1; t < PSX_QUALITY_COUNT; t++)
        if (offered_mask & (1u << t)) return t;
    return PSX_QUALITY_NONE;
}

static uint64_t fnv1a(uint64_t h, const char *s) {
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h ^ 0xFFu;   /* field separator */
}

void psx_quality_fingerprint(const PsxHostInfo *h, char out[17]) {
    char num[64];
    uint64_t x = 14695981039346656037ull;
    x = fnv1a(x, h->cpu);
    x = fnv1a(x, h->gpu[0] ? h->gpu : h->gpu_id);
    snprintf(num, sizeof num, "%d/%llu/%d", h->logical_cores,
             (unsigned long long)((h->ram_mb + 512u) / 1024u), h->steam_deck);
    x = fnv1a(x, num);
    snprintf(out, 17, "%016llx", (unsigned long long)x);
}

void psx_quality_summary(const PsxHostInfo *h, char *out, size_t cap) {
    const char *gpu = h->gpu[0] ? h->gpu : "";
    const char *cpu = h->cpu[0] ? h->cpu : "Unknown CPU";
    char mem[32] = "";
    if (h->ram_mb) snprintf(mem, sizeof mem, " \xC2\xB7 %llu GB",
                            (unsigned long long)((h->ram_mb + 512u) / 1024u));
    /* Apple silicon names the GPU after the CPU: say it once. */
    if (gpu[0] && strcmp(gpu, cpu) != 0)
        snprintf(out, cap, "%s%s \xC2\xB7 %s \xC2\xB7 %d threads%s", h->steam_deck ? "Steam Deck \xC2\xB7 " : "",
                 gpu, cpu, h->logical_cores, mem);
    else
        snprintf(out, cap, "%s%s \xC2\xB7 %d threads%s", h->steam_deck ? "Steam Deck \xC2\xB7 " : "",
                 cpu, h->logical_cores, mem);
}
