#include "memcard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define PATH0 "card1.mcd"
#define PATH1 "card2.mcd"
#define SECTOR 400
static uint8_t base[MEMCARD_SIZE], saved[MEMCARD_SIZE];
static int loads, save_reject, load_reject, have_tail[2];

static long file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    CHECK(f && fseek(f, 0, SEEK_END) == 0);
    long result = ftell(f);
    CHECK(fclose(f) == 0);
    return result;
}

static void append_tail(uint32_t size) {
    FILE* f = fopen(PATH0, "ab");
    CHECK(f);
    for (uint32_t i = 0; i < size; ++i) CHECK(fputc('X', f) != EOF);
    CHECK(fclose(f) == 0);
}

static void read_base(uint8_t* out) {
    FILE* f = fopen(PATH0, "rb");
    CHECK(f && fread(out, 1, MEMCARD_SIZE, f) == MEMCARD_SIZE);
    CHECK(fclose(f) == 0);
}

static int codec_load(int card, const uint8_t* raw, const uint8_t* tail,
                      uint32_t size) {
    CHECK(card == 0 || card == 1);
    CHECK(raw[0] == 'M' && raw[1] == 'C');
    CHECK(size == 0 || (size == 5 && tail));
    ++loads;
    have_tail[card] = size != 0;
    return load_reject ? -1 : 0;
}

static int codec_save(int card, const uint8_t* raw, uint8_t* tail,
                      uint32_t capacity) {
    if (save_reject) return save_reject == 1 ? -1 : (int)capacity + 1;
    if (!have_tail[card]) return 0;
    CHECK(capacity >= 5);
    memcpy(tail, "TAIL", 4);
    tail[4] = raw[SECTOR * MEMCARD_SECTOR_SIZE];
    return 5;
}

static void write_sector(uint8_t value) {
    uint8_t sector[MEMCARD_SECTOR_SIZE];
    memset(sector, value, sizeof sector);
    CHECK(memcard_write_sector(0, SECTOR, sector) == 0);
}

static int dirty(void) {
    int value;
    CHECK(memcard_debug_info(0, NULL, NULL, NULL, &value) == 0);
    return value;
}

int main(void) {
    const MemcardSlotConfig slots[2] = {{PATH0, 1}, {PATH1, 1}};
    MemcardSummary summary;
    CHECK(memcard_format_file(PATH0) == 0);
    CHECK(memcard_format_file(PATH1) == 0);
    append_tail(5);

    /* Inactive mods cannot silently truncate metadata, and the device/raw
     * export must still expose exactly the ordinary card bytes. */
    memcard_set_extension_codec(NULL, NULL);
    memcard_init_slots(NULL, slots);
    CHECK(memcard_export_raw(0, base) == 0);
    CHECK(base[0] == 'M' && base[1] == 'C');
    write_sector(17);
    memcard_flush(0);
    CHECK(memcard_last_flush_result(0) == 0 && !dirty());
    CHECK(file_size(PATH0) == MEMCARD_SIZE + 5);
    FILE* f = fopen(PATH0, "rb");
    CHECK(f && fseek(f, MEMCARD_SIZE, SEEK_SET) == 0);
    for (int i = 0; i < 5; ++i) CHECK(fgetc(f) == 'X');
    CHECK(fclose(f) == 0);
    CHECK(memcard_summary_path(PATH0, &summary) == 0);
    CHECK(summary.valid && summary.size_bytes == MEMCARD_SIZE + 5);

    /* A selected codec sees both slots and binds the extension to the base
     * in the SAME publication, after the actual native writes. */
    memcard_set_extension_codec(codec_load, codec_save);
    memcard_init_slots(NULL, slots);
    CHECK(loads == 2 && have_tail[0] && !have_tail[1]);
    write_sector(39);
    memcard_flush_all();
    CHECK(memcard_last_flush_result(0) == 0 && !dirty());
    CHECK(file_size(PATH0) == MEMCARD_SIZE + 5);
    f = fopen(PATH0, "rb");
    CHECK(f && fseek(f, MEMCARD_SIZE, SEEK_SET) == 0);
    char tail[5];
    CHECK(fread(tail, 1, 5, f) == 5);
    CHECK(memcmp(tail, "TAIL", 4) == 0 && tail[4] == 39);
    CHECK(fclose(f) == 0);
    read_base(saved);

    /* Rejection or an impossible returned length must leave the original
     * file intact and the buffered native write dirty for a later retry. */
    write_sector(54);
    for (save_reject = 1; save_reject <= 2; ++save_reject) {
        memcard_flush(0);
        CHECK(memcard_last_flush_result(0) == -2 && dirty());
        read_base(base);
        CHECK(memcmp(base, saved, sizeof base) == 0);
        CHECK(file_size(PATH0) == MEMCARD_SIZE + 5);
    }
    save_reject = 0;
    memcard_flush(0);
    CHECK(memcard_last_flush_result(0) == 0 && !dirty());
    read_base(saved);

    load_reject = 1;
    CHECK(memcard_reload_bound() == 0);
    write_sector(71);
    memcard_flush(0);
    CHECK(memcard_last_flush_result(0) == -2 && dirty());
    read_base(base);
    CHECK(memcmp(base, saved, sizeof base) == 0);
    load_reject = 0;
    CHECK(memcard_reload_bound() == 0);

    /* Oversized tails cannot be accidentally replaced with a shorter image. */
    memcard_set_extension_codec(NULL, NULL);
    CHECK(memcard_format_file(PATH0) == 0);
    append_tail(MEMCARD_EXTENSION_MAX + 1u);
    CHECK(memcard_reload_bound() == 0);
    write_sector(91);
    memcard_flush(0);
    CHECK(memcard_last_flush_result(0) == -2 && dirty());
    CHECK(file_size(PATH0) == MEMCARD_SIZE + MEMCARD_EXTENSION_MAX + 1u);

    /* Raw import is explicit replacement, so previous extension ownership
     * does not leak into a different standard card (e.g. netplay sandbox). */
    CHECK(memcard_import_raw(0, saved) == 0);
    CHECK(file_size(PATH0) == MEMCARD_SIZE);

    /* Publication failure leaves dirty data and does not touch the old card. */
#ifdef _WIN32
    (void)_mkdir("blocked_target");
#else
    (void)mkdir("blocked_target", 0700);
#endif
    CHECK(memcard_rebind_path(0, "blocked_target") == 0);
    write_sector(105);
    memcard_flush(0);
    CHECK(memcard_last_flush_result(0) == -1 && dirty());
    read_base(base);
    CHECK(memcmp(base, saved, sizeof base) == 0);
    memcard_set_extension_codec(NULL, NULL);
    puts("memcard extensions: preservation, refresh, rejection, raw import and atomic failure passed");
    return 0;
}
