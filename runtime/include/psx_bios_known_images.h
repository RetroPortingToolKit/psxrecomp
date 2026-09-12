/* psx_bios_known_images.h — identities of the retail BIOS images this
 * framework ships a build profile for.
 *
 * A *built* host asks its linked backends (psx_bios_registry) which image it
 * accepts. A *setup* host has no linked backend — that is what makes it a
 * setup host — so first-run discovery, bios.cfg seeding and picker copy had
 * nowhere to look and hardcoded SCPH-1001. Any kit pinning another image via
 * PSXRECOMP_BIOS_STEM / recompiler.bios_config (every wave-3 kit pins
 * SCPH5552) then refused to auto-discover a perfectly good dump and told the
 * player their BIOS was the wrong one.
 *
 * Keep in sync with bios/<stem>.toml. A stem absent from this table still
 * builds and runs; it only loses first-run auto-discovery and falls back to
 * asking the player, which is safe.
 */
#ifndef PSX_BIOS_KNOWN_IMAGES_H
#define PSX_BIOS_KNOWN_IMAGES_H

#include <stdint.h>
#include <string.h>

/* Set by runtime.cmake from PSXRECOMP_BIOS_STEM. */
#ifndef PSX_EXPECTED_BIOS_STEM
#define PSX_EXPECTED_BIOS_STEM "SCPH1001"
#endif

typedef struct PsxKnownBiosImage {
    const char* stem;  /* generated/<stem>_dispatch.c, bios/<stem>.toml */
    const char* id;    /* profile id, e.g. "SCPH-5552" */
    uint32_t    crc32; /* IEEE CRC-32 (zlib/Ethernet) over the whole image */
    uint32_t    size;  /* bytes */
} PsxKnownBiosImage;

/* Each entry verified two ways: against the dump itself, and against the
 * identity the recompiler emits into generated/<stem>_dispatch.c. */
static const PsxKnownBiosImage psx_known_bios_images[] = {
    { "SCPH1001", "SCPH-1001", 0x37157331u, 524288u },
    { "SCPH5552", "SCPH-5552", 0xD786F0B9u, 524288u },
};

static inline const PsxKnownBiosImage* psx_known_bios_by_stem(const char* stem) {
    size_t i;
    if (!stem || !stem[0])
        return 0;
    for (i = 0; i < sizeof(psx_known_bios_images) / sizeof(psx_known_bios_images[0]); ++i) {
        if (strcmp(psx_known_bios_images[i].stem, stem) == 0)
            return &psx_known_bios_images[i];
    }
    return 0;
}

/* The retail image THIS build was configured for, or NULL when the pinned
 * stem is not in the table. Callers must handle NULL by asking the player
 * rather than assuming SCPH-1001. */
static inline const PsxKnownBiosImage* psx_expected_bios(void) {
    return psx_known_bios_by_stem(PSX_EXPECTED_BIOS_STEM);
}

/* A short label for the pinned image, for player-facing copy: "SCPH5552.BIN".
 * Falls back to the raw stem when the identity is not in the table. */
static inline const char* psx_expected_bios_label(void) {
    static char label[40];
    if (!label[0]) {
        const PsxKnownBiosImage* e = psx_expected_bios();
        const char* stem = e ? e->stem : PSX_EXPECTED_BIOS_STEM;
        size_t n = strlen(stem);
        if (n > sizeof(label) - 5)
            n = sizeof(label) - 5;
        memcpy(label, stem, n);
        memcpy(label + n, ".BIN", 5);
    }
    return label;
}

/* Filename spellings worth probing for <img> during first-run discovery,
 * written into out[0..n). Covers the stem and the dashed profile id, each in
 * upper and lower case: SCPH5552.BIN, scph5552.bin, SCPH-5552.BIN, ... */
static inline int psx_known_bios_filenames(const PsxKnownBiosImage* img,
                                           char out[][32], int cap) {
    const char* bases[2];
    int n = 0, b, lower;
    if (!img)
        return 0;
    bases[0] = img->stem;
    bases[1] = img->id;
    for (b = 0; b < 2; ++b) {
        for (lower = 0; lower < 2; ++lower) {
            const char* ext = lower ? "bin" : "BIN";
            size_t i, len = strlen(bases[b]);
            char* dst;
            if (n >= cap || len + 5 > 32)
                continue;
            dst = out[n++];
            for (i = 0; i < len; ++i) {
                char c = bases[b][i];
                dst[i] = (char)(lower && c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            }
            dst[len] = '.';
            memcpy(dst + len + 1, ext, 3);
            dst[len + 4] = 0;
        }
    }
    return n;
}

#endif /* PSX_BIOS_KNOWN_IMAGES_H */
