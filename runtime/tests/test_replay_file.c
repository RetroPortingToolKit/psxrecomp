/* Player replay file: write/read round trip, refusals, and the identity check. */
#include "replay_file.h"
#include "replay_identity.h"
#include "psx_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

#define FRAMES 300u
static ReplayStep s_steps[REPLAY_FILE_MAX_STEPS];
static ReplayFileInfo s_info;

static void put32(unsigned char *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

static void identity(ReplayIdentity *id)
{
    memset(id, 0, sizeof *id);
    strcpy(id->build, "0123456789abcdef0123456789abcdef01234567");
    strcpy(id->disc_serial, "SLUS-00000");
    strcpy(id->bios_stem, "scph1001");
    strcpy(id->boot_mode, "hle");
    id->disc_digest_kind = REPLAY_DISC_DIGEST_FILE;
    for (unsigned i = 0; i < 32; ++i) id->disc_digest[i] = (uint8_t)(i * 7 + 1);
}

/* Writes `o` to a buffer through a file. Returns the size, 0 on failure. */
static size_t write_buffer(const ReplayFileOut *o, unsigned char **out)
{
    FILE *f = fopen("replay_file_test.psxrpl", "wb+");
    long size;
    const char *error;
    if (!f) return 0;
    error = replay_file_write(f, o);
    if (error) { fprintf(stderr, "write: %s\n", error); fclose(f); return 0; }
    size = ftell(f);
    *out = (unsigned char *)malloc((size_t)size);
    rewind(f);
    if (!*out || fread(*out, 1, (size_t)size, f) != (size_t)size) size = 0;
    fclose(f);
    return (size_t)size;
}

static const char *read_buffer(const unsigned char *buf, size_t size)
{
    FILE *f = fopen("replay_file_test.psxrpl", "wb+");
    const char *error;
    if (!f) return "cannot open the test file";
    if (fwrite(buf, 1, size, f) != size) { fclose(f); return "cannot write the test file"; }
    rewind(f);
    memset(&s_info, 0, sizeof s_info);
    error = replay_file_read(f, &s_info, s_steps, REPLAY_FILE_MAX_STEPS);
    fclose(f);
    return error;
}

/* `buf` with one extension entry (tag, u32 payload) inserted after the header. */
static const char *read_with_entry(const unsigned char *buf, size_t size, uint32_t tag)
{
    unsigned char *b = (unsigned char *)malloc(size + 12);
    const char *error;
    memcpy(b, buf, REPLAY_FILE_HEADER_BYTES);
    put32(b + 24, replay_file_le32(buf + 24) + 12);
    put32(b + 28, tag);
    put32(b + 32, 4);
    put32(b + 36, 0x12345678u);
    memcpy(b + 40, buf + REPLAY_FILE_HEADER_BYTES, size - REPLAY_FILE_HEADER_BYTES);
    error = read_buffer(b, size + 12);
    free(b);
    return error;
}

static void test_round_trip(void)
{
    static ReplayWord words[FRAMES];
    static ReplayCheckpoint end;
    static uint8_t ram[2u << 20];
    static uint8_t cards[2 * REPLAY_FILE_CARD_BYTES];
    static const uint32_t digests[2][5] = {{0, 1, 2, 3, 4}, {60, 5, 6, 7, 8}};
    static const uint32_t thumb[4] = {0xff000000u, 0xffffffffu, 0xff102030u, 0xff405060u};
    unsigned char anchor[100], *buf = NULL;
    ReplayIdentity id;
    ReplayFileOut o;
    size_t size;
    const char *error;

    identity(&id);
    for (uint32_t i = 0; i < FRAMES; ++i) {
        words[i].buttons = i < 100 ? 0xffffu : (i < 150 ? 0xbfffu : 0xffffu);
        memset(words[i].axes_ly_lx_ry_rx, 0x80, 4);
    }
    words[200].axes_ly_lx_ry_rx[1] = 0xff;
    for (uint32_t i = 0; i < sizeof ram; ++i) ram[i] = (uint8_t)(i * 13);
    replay_file_checkpoint(FRAMES, ram, 123456789012ull, &end);
    for (unsigned i = 0; i < sizeof anchor; ++i) anchor[i] = (unsigned char)i;

    memset(&o, 0, sizeof o);
    o.id = &id;
    o.words = words;
    o.frames = FRAMES;
    o.end = &end;
    o.anchor = anchor;
    o.anchor_length = 99; /* odd: exercises padding */
    o.settings = "cdrom_speed=2\nidle_skip=0\n";
    o.digests = digests;
    o.digest_count = 2;
    o.thumb = thumb;
    o.thumb_w = 2;
    o.thumb_h = 2;
    o.name = "First stage";
    o.product = "exe_sha256=ab\nbios_crc32=1234abcd\n";
    o.code_set = "count=2\ndigest=00ff\nunit=gcc/a.dll\nunit=gcc/b.dll\n";
    size = write_buffer(&o, &buf);
    CHECK(size > 0);
    error = read_buffer(buf, size);
    CHECK(error == NULL);
    if (!error) {
        CHECK(s_info.frames == FRAMES);
        CHECK(s_info.steps == 5); /* 100 idle, 50 pressed, 50 idle, 1 stick, 99 idle */
        CHECK(s_steps[0].frames == 100 && s_steps[0].buttons == 0xffffu);
        CHECK(s_steps[1].frames == 50 && s_steps[1].buttons == 0xbfffu);
        CHECK(s_steps[3].frames == 1 && s_steps[3].axes_ly_lx_ry_rx[1] == 0xff);
        CHECK(!memcmp(&s_info.id, &id, sizeof id));
        CHECK(s_info.has_anchor && !s_info.power_on && s_info.anchor_length == 99);
        CHECK(!memcmp(buf + s_info.anchor_offset, anchor, 99));
        CHECK(!strcmp(s_info.settings, o.settings));
        CHECK(!strcmp(s_info.product, o.product));
        CHECK(!strcmp(s_info.code_set, o.code_set));
        CHECK(!strcmp(s_info.name, "First stage"));
        CHECK(s_info.digest_count == 2 && replay_file_le32(buf + s_info.digest_offset + 20) == 60);
        CHECK(s_info.thumb_w == 2 && s_info.thumb_h == 2 &&
              replay_file_le32(buf + s_info.thumb_offset + 12) == 0xff405060u);
        CHECK(s_info.end.frame == FRAMES && s_info.end.cycle == 123456789012ull);
        CHECK(!memcmp(s_info.end.ram_sha256, end.ram_sha256, 32));
        CHECK(!memcmp(s_info.end.pages, end.pages, sizeof end.pages));
    }

    /* An unknown skippable entry is skipped; anything else unknown is refused. */
    CHECK(read_with_entry(buf, size, 0x80000400u) == NULL);
    CHECK(read_with_entry(buf, size, 0x00000400u) != NULL);
    CHECK(read_with_entry(buf, size, 0x80000199u) != NULL);
    /* A second anchor or identity tag is refused. */
    CHECK(read_with_entry(buf, size, REPLAY_TAG_POWER_ON) != NULL);
    {
        unsigned char *b = (unsigned char *)malloc(size);
        memcpy(b, buf, size);
        CHECK(read_buffer(b, size - 1) != NULL);                  /* truncated */
        put32(b + 12, 16);
        CHECK(read_buffer(b, size) != NULL);                      /* record size */
        memcpy(b, buf, size);
        put32(b + size - REPLAY_FILE_RECORD_BYTES, 7);
        CHECK(read_buffer(b, size) != NULL);                      /* record sequence */
        memcpy(b, buf, size);
        b[0] = 'X';
        CHECK(read_buffer(b, size) != NULL);                      /* magic */
        memcpy(b, buf, size);
        put32(b + 20, 1);
        CHECK(read_buffer(b, size) != NULL);                      /* flags */
        free(b);
    }
    {
        /* Too few step slots for the runs. */
        FILE *f = fopen("replay_file_test.psxrpl", "wb+");
        CHECK(f && fwrite(buf, 1, size, f) == size);
        if (f) {
            rewind(f);
            CHECK(replay_file_read(f, &s_info, s_steps, 4) != NULL);
            fclose(f);
        }
    }
    free(buf);
    buf = NULL;

    /* The code set is optional to old readers: skippable, and outside the
     * identity and marker tags an unknown-tag reader refuses. */
    CHECK((REPLAY_TAG_CODE_SET & REPLAY_TAG_SKIPPABLE) != 0);
    CHECK((REPLAY_TAG_CODE_SET & 0x7fffff00u) != 0x100u);
    /* The writer refuses what the reader would refuse. */
    o.code_set = "count=1\n\001";
    CHECK(replay_file_write(stdout, &o) != NULL);
    {
        static char big[REPLAY_FILE_CODE_SET_MAX + 2];
        memset(big, 'a', sizeof big - 1);
        o.code_set = big;
        CHECK(replay_file_write(stdout, &o) != NULL);
    }
    /* A file written without it, as every older recorder wrote, reads with
     * an empty code set. */
    o.code_set = NULL;
    o.power_on = 1;
    CHECK(replay_file_write(stdout, &o) != NULL);              /* anchor and power-on */
    o.anchor = NULL;
    o.anchor_length = 0;
    o.digests = NULL;
    o.digest_count = 0;
    o.name = "bad\001name";
    CHECK(replay_file_write(stdout, &o) != NULL);
    o.name = NULL;

    /* Power-on with card 2 inserted. */
    for (uint32_t i = 0; i < REPLAY_FILE_CARD_BYTES; ++i) cards[i] = (uint8_t)(i ^ 0x5a);
    o.cards = cards;
    o.cards_mask = 2u;
    size = write_buffer(&o, &buf);
    CHECK(size > 0);
    error = read_buffer(buf, size);
    CHECK(error == NULL);
    if (!error) {
        CHECK(s_info.power_on && !s_info.has_anchor && s_info.cards_mask == 2u);
        CHECK(!memcmp(buf + s_info.cards_offset, cards, REPLAY_FILE_CARD_BYTES));
        CHECK(s_info.digest_count == 0 && !s_info.name[0]);
        CHECK(!s_info.code_set[0]);
    }
    free(buf);
    remove("replay_file_test.psxrpl");
}

static void test_identity_check(void)
{
    ReplayIdentity rec, player;
    char why[160];
    identity(&rec);
    identity(&player);
    CHECK(replay_identity_check(&rec, "", &player, "", why, sizeof why) == REPLAY_IDENTITY_SAME_BUILD);
    strcpy(player.bios_stem, "SCPH1001");
    CHECK(replay_identity_check(&rec, "", &player, "", why, sizeof why) == REPLAY_IDENTITY_SAME_BUILD);
    player.build[0] = 'f';
    CHECK(replay_identity_check(&rec, "", &player, "", why, sizeof why) == REPLAY_IDENTITY_OTHER_BUILD);
    identity(&player);
    CHECK(replay_identity_check(&rec, "exe_sha256=aa\n", &player, "exe_sha256=bb\n", why, sizeof why) ==
          REPLAY_IDENTITY_OTHER_BUILD);
    CHECK(replay_identity_check(&rec, "exe_sha256=aa\n", &player, "", why, sizeof why) ==
          REPLAY_IDENTITY_SAME_BUILD);
    /* The BIOS: the CRC decides when both sides have one, else the stem. */
    strcpy(player.bios_stem, "other-name");
    CHECK(replay_identity_check(&rec, "bios_crc32=37157331\n", &player, "bios_crc32=37157331", why,
                                sizeof why) == REPLAY_IDENTITY_SAME_BUILD);
    CHECK(replay_identity_check(&rec, "", &player, "", why, sizeof why) == REPLAY_IDENTITY_REFUSE);
    CHECK(strstr(why, "BIOS") != NULL);
    identity(&player);
    CHECK(replay_identity_check(&rec, "bios_crc32=37157331\n", &player, "bios_crc32=00000000\n", why,
                                sizeof why) == REPLAY_IDENTITY_REFUSE);
    identity(&player);
    player.disc_digest[5] ^= 1;
    CHECK(replay_identity_check(&rec, "", &player, "", why, sizeof why) == REPLAY_IDENTITY_REFUSE);
    CHECK(strstr(why, "disc") != NULL);
    identity(&player);
    strcpy(player.disc_serial, "SLUS-00001");
    CHECK(replay_identity_check(&rec, "", &player, "", why, sizeof why) == REPLAY_IDENTITY_REFUSE);
    identity(&player);
    strcpy(player.boot_mode, "lle");
    CHECK(replay_identity_check(&rec, "", &player, "", why, sizeof why) == REPLAY_IDENTITY_REFUSE);
    CHECK(strstr(why, "boot mode") != NULL);
    CHECK(!strcmp(replay_identity_boot_mode(1, 1), "hle"));
    CHECK(!strcmp(replay_identity_boot_mode(1, 0), "hle-calls"));
    CHECK(!strcmp(replay_identity_boot_mode(0, 1), "hle-boot"));
    CHECK(!strcmp(replay_identity_boot_mode(0, 0), "lle"));
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void hex_of(const char *text, char out[65])
{
    static const char digits[] = "0123456789abcdef";
    uint8_t d[32];
    psx_sha256_compute((const uint8_t *)text, strlen(text), d);
    for (unsigned i = 0; i < 32; ++i) {
        out[2 * i] = digits[d[i] >> 4];
        out[2 * i + 1] = digits[d[i] & 15];
    }
    out[64] = 0;
}

static void test_disc_digest(void)
{
    const char *cue = "FILE \"replay_track1.bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n"
                      "FILE \"replay_track2.bin\" BINARY\r\n  TRACK 02 AUDIO\r\n";
    char joined[64 * 3 + 3], h[65];
    uint8_t want[32], got[32];
    uint32_t kind = 0;
    ReplayIdentity id;
    char why[160];

    write_file("replay_track1.bin", "data track");
    write_file("replay_track2.bin", "audio track");
    write_file("replay_disc.cue", cue);
    hex_of(cue, h);
    strcpy(joined, h);
    strcat(joined, "\n");
    hex_of("data track", h);
    strcat(joined, h);
    strcat(joined, "\n");
    hex_of("audio track", h);
    strcat(joined, h);
    psx_sha256_compute((const uint8_t *)joined, strlen(joined), want);
    CHECK(replay_identity_disc_digest("replay_disc.cue", &kind, got) == NULL);
    CHECK(kind == REPLAY_DISC_DIGEST_CUE && !memcmp(got, want, 32));

    psx_sha256_compute((const uint8_t *)"data track", 10, want);
    CHECK(replay_identity_disc_digest("replay_track1.bin", &kind, got) == NULL);
    CHECK(kind == REPLAY_DISC_DIGEST_FILE && !memcmp(got, want, 32));

    write_file("replay_bad.cue", "FILE replay_track1.bin BINARY\n");
    CHECK(replay_identity_disc_digest("replay_bad.cue", &kind, got) != NULL);
    write_file("replay_missing.cue", "FILE \"replay_none.bin\" BINARY\n");
    CHECK(replay_identity_disc_digest("replay_missing.cue", &kind, got) != NULL);

    replay_identity_set_product("SLUS-00000", "replay_disc.cue", "bios/SCPH1001.BIN");
    CHECK(replay_identity_current(&id, 1, 0, why, sizeof why) == 1);
    CHECK(!strcmp(id.bios_stem, "SCPH1001") && !strcmp(id.boot_mode, "hle-calls"));
    CHECK(strlen(id.build) == 40 && id.disc_digest_kind == REPLAY_DISC_DIGEST_CUE);
    replay_identity_set_product("", "replay_disc.cue", "bios/SCPH1001.BIN");
    CHECK(replay_identity_current(&id, 1, 0, why, sizeof why) == 0);

    remove("replay_track1.bin");
    remove("replay_track2.bin");
    remove("replay_disc.cue");
    remove("replay_bad.cue");
    remove("replay_missing.cue");
}

int main(void)
{
    test_round_trip();
    test_identity_check();
    test_disc_digest();
    if (failures) {
        fprintf(stderr, "replay_file_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("replay_file_test: ok\n");
    return 0;
}
