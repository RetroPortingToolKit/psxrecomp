#ifndef PSX_REPLAY_FILE_H
#define PSX_REPLAY_FILE_H
/* Player replay files (.psxrpl): the PSXRTI3 input-route container with the
 * replay entries. This reads and writes the subset a player replay uses: one
 * DualShock record per guest vblank for port 1, the identity of the disc,
 * BIOS and boot mode it was recorded on, where it starts (an anchor save
 * state, or power-on with the memory cards the guest booted with), the host
 * settings that change guest timing, and checks for playback (state digests
 * every so many frames and an END checkpoint on the last boundary).
 *
 * Wire layout, little-endian:
 *   header   u8[8] "PSXRTI3\0", u32 version=3, u32 record_size=12,
 *            u32 frame_count, u32 flags=0, u32 ext_bytes (multiple of 4)
 *   entries  ext_bytes of: u32 tag, u32 length, payload[length],
 *            zero padding to 4 bytes (not counted in length)
 *   records  frame_count records: u32 one-based frame, u16 active-low
 *            buttons, u8 LY,LX,RY,RX, u8 analog button (0/1), u8 0
 * The file size must equal 28 + ext_bytes + frame_count * 12.
 *
 * Tag bit 31 marks an entry that does not change playback; a reader skips an
 * unknown one. Any other unknown tag is refused, as is an unknown identity or
 * marker tag (0x800001xx), so a replay is never played with a field ignored.
 *
 * Frame N in a marker, checkpoint or digest is the boundary after record N
 * was fed and before record N+1; frame 0 is before the first record.
 * Reading touches no guest state and no live input. */
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REPLAY_FILE_HEADER_BYTES 28u
#define REPLAY_FILE_RECORD_BYTES 12u
#define REPLAY_FILE_MAX_EXT (16u << 20)
#define REPLAY_FILE_MAX_FRAMES 1000000u
#define REPLAY_FILE_MAX_STEPS 65536u
#define REPLAY_FILE_MAX_MARKERS 256u
#define REPLAY_FILE_TEXT 128u
#define REPLAY_FILE_RAM_PAGES 512u          /* 2 MiB main RAM, 4 KiB pages */

#define REPLAY_TAG_SKIPPABLE   0x80000000u
/* Identity: all five or none. */
#define REPLAY_TAG_BUILD       0x80000101u  /* 40 lowercase hex framework commit */
#define REPLAY_TAG_DISC_SERIAL 0x80000102u  /* boot serial, e.g. SCUS-12345 */
#define REPLAY_TAG_DISC_DIGEST 0x80000103u  /* u8 kind, u8[3] 0, u8[32] SHA-256 */
#define REPLAY_TAG_BIOS_STEM   0x80000104u  /* BIOS file name without extension */
/* "lle" (no HLE), "hle" (kernel calls and boot skip), "hle-calls" (calls
 * only), "hle-boot" (boot skip only). */
#define REPLAY_TAG_BOOT_MODE   0x80000105u
#define REPLAY_TAG_MARKER      0x80000110u  /* u32 frame, u8 kind, u8[3] 0 */
#define REPLAY_TAG_CHECKPOINT  0x80000111u  /* see REPLAY_FILE_CHECKPOINT_BYTES */
/* Mandatory replay entries. A replay holds exactly one of ANCHOR and POWER_ON.
 *   ANCHOR    the save state (.pst image) playback loads first
 *   SETTINGS  ASCII "key=value" lines of the host settings that change guest
 *             timing; playback switches to them and restores the player's
 *   DIGESTS   u32 count, then count x (u32 frame, u32 core, av, aux, ext
 *             state digests), frames strictly increasing
 *   POWER_ON  u32 vblank (always 0): record 0 is fed at the first vblank of
 *             a cold boot
 *   CARDS     u32 mask (bit 0 card 1, bit 1 card 2), then one 128 KiB image
 *             per inserted card in slot order: the cards at power-on */
#define REPLAY_TAG_ANCHOR      0x00000301u
#define REPLAY_TAG_SETTINGS    0x00000302u
#define REPLAY_TAG_DIGESTS     0x00000303u
#define REPLAY_TAG_POWER_ON    0x00000306u
#define REPLAY_TAG_CARDS       0x00000307u
/* Optional entries (skippable):
 *   THUMB    u16 width, u16 height, width*height u32 ARGB: the picture at the
 *            start
 *   NAME     UTF-8 text, no control bytes
 *   PRODUCT  ASCII "key=value" lines naming the product that recorded it:
 *            exe_sha256, codegen, bios_crc32, renderer, platform */
#define REPLAY_TAG_THUMB       0x80000304u
#define REPLAY_TAG_NAME        0x80000305u
#define REPLAY_TAG_PRODUCT     0x80000308u

#define REPLAY_FILE_DIGEST_BYTES 20u
#define REPLAY_FILE_SETTINGS_MAX 4096u
#define REPLAY_FILE_NAME_MAX 96u
#define REPLAY_FILE_PRODUCT_MAX 1024u
#define REPLAY_FILE_THUMB_MAX (4u + 4u * 256u * 256u)
#define REPLAY_FILE_CARD_BYTES (128u * 1024u)

/* Disc digest kinds. CUE: SHA-256 over the lowercase hex SHA-256 of the cue
 * sheet and then of each FILE track in cue order, joined by '\n'. FILE: the
 * SHA-256 of the whole image. */
#define REPLAY_DISC_DIGEST_CUE  1u
#define REPLAY_DISC_DIGEST_FILE 2u

#define REPLAY_MARKER_MENU     1u
#define REPLAY_MARKER_GAMEPLAY 2u
#define REPLAY_MARKER_END      3u    /* the last boundary */
/* u32 frame, u32 0, u64 guest cycle, u8[32] SHA-256 of main RAM, u64[512]
 * FNV-1a 64 hash of each 4 KiB page. */
#define REPLAY_FILE_CHECKPOINT_BYTES (4u + 4u + 8u + 32u + 8u * REPLAY_FILE_RAM_PAGES)

typedef struct {
    char build[REPLAY_FILE_TEXT];
    char disc_serial[REPLAY_FILE_TEXT];
    char bios_stem[REPLAY_FILE_TEXT];
    char boot_mode[REPLAY_FILE_TEXT];
    uint32_t disc_digest_kind;
    uint8_t disc_digest[32];
} ReplayIdentity;

typedef struct {
    uint32_t frame;
    uint64_t cycle;
    uint8_t ram_sha256[32];
    uint64_t pages[REPLAY_FILE_RAM_PAGES];
} ReplayCheckpoint;

/* A run of identical records. */
typedef struct {
    uint32_t frames;
    uint16_t buttons;
    uint8_t axes_ly_lx_ry_rx[4];
    uint8_t analog_button;
} ReplayStep;

/* One record as written: active-low buttons and axes LY,LX,RY,RX. */
typedef struct {
    uint16_t buttons;
    uint8_t axes_ly_lx_ry_rx[4];
} ReplayWord;

/* What replay_file_read found. Large entries are located, not loaded:
 * the offsets count from the start of the file. */
typedef struct {
    uint32_t frames;
    uint32_t steps;
    ReplayIdentity id;
    ReplayCheckpoint end;           /* the checkpoint on the last boundary */
    int has_anchor;
    long anchor_offset;
    uint32_t anchor_length;
    char settings[REPLAY_FILE_SETTINGS_MAX + 1];
    uint32_t digest_count;
    long digest_offset;             /* the first entry, after the count */
    uint32_t thumb_w, thumb_h;      /* 0 when absent */
    long thumb_offset;
    char name[REPLAY_FILE_NAME_MAX + 1];
    int power_on;
    uint32_t cards_mask;
    long cards_offset;              /* the first inserted card's image */
    char product[REPLAY_FILE_PRODUCT_MAX + 1];
} ReplayFileInfo;

/* What replay_file_write writes. NULL or zero members are left out; exactly
 * one of anchor and power_on. */
typedef struct {
    const ReplayIdentity *id;
    const ReplayWord *words;
    uint32_t frames;
    const ReplayCheckpoint *end;    /* end->frame must equal frames */
    const void *anchor;
    uint32_t anchor_length;
    int power_on;
    const uint8_t *cards;           /* the inserted cards' images, slot order */
    uint32_t cards_mask;
    const char *settings;
    const uint32_t (*digests)[5];   /* frame, core, av, aux, ext */
    uint32_t digest_count;
    const uint32_t *thumb;          /* thumb_w * thumb_h ARGB */
    uint16_t thumb_w, thumb_h;
    const char *name;
    const char *product;
} ReplayFileOut;

/* Reads a whole replay from the current position (the magic). Up to
 * `step_cap` runs of identical records go to `steps`; `info` is published
 * only when the whole file passes. Returns NULL, or the reason it was
 * refused. */
const char *replay_file_read(FILE *f, ReplayFileInfo *info, ReplayStep *steps, uint32_t step_cap);
/* Writes a replay; the reader's rules are checked first. NULL or the reason. */
const char *replay_file_write(FILE *f, const ReplayFileOut *out);

/* The checkpoint of a boundary: RAM SHA-256 and per-page FNV-1a 64. */
void replay_file_checkpoint(uint32_t frame, const uint8_t *ram, uint64_t cycle, ReplayCheckpoint *out);
/* A replay name: 1..REPLAY_FILE_NAME_MAX bytes, no control bytes. */
int replay_file_name_ok(const char *s, size_t n);
static inline uint32_t replay_file_card_count(uint32_t mask)
{
    return (mask & 1u) + ((mask >> 1) & 1u);
}
uint32_t replay_file_le32(const unsigned char *p);

#ifdef __cplusplus
}
#endif
#endif
