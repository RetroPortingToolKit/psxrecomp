/* State-file replace: a save must never cost the player the previous file.
 *
 * boot_state_save() writes through boot_state_write_file_atomic(): the state
 * uses the shared streaming transaction and replaces <path> only when complete. This test drives
 * that helper with stand-in writers, so it needs no serializer:
 *   - replace onto an absent and an existing target (a bare Windows rename
 *     refuses the existing one);
 *   - a write that fails partway and a temporary that cannot be created both
 *     leave the previous file byte for byte and remove the temporary.
 * Usage: test_boot_state_replace [work-folder]; default is the current folder. */
#include "boot_state.h"
#include "boot_state_replace.inc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <direct.h>
#  define make_dir(p) _mkdir(p)
#  define remove_dir(p) _rmdir(p)
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define make_dir(p) mkdir((p), 0700)
#  define remove_dir(p) rmdir(p)
#endif

static int failures = 0;
static void check(int cond, const char *what) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

static int write_bytes(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (fwrite(data, 1, n, f) != n) { fclose(f); return 0; }
    return fclose(f) == 0;
}

static int read_is(const char *path, const void *data, size_t n) {
    unsigned char buf[256];
    FILE *f = fopen(path, "rb");
    size_t got;
    if (!f) return 0;
    got = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return got == n && memcmp(buf, data, n) == 0;
}

static int exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

typedef struct { const char *text; int fail_after; } Writer;

/* Writes `text`; with fail_after set, stops after that many bytes and fails,
 * as the serializer does when a section write fails midway. */
static int stand_in_writer(RecompAtomicFile *f, void *ctx) {
    const Writer *w = (const Writer *)ctx;
    size_t n = strlen(w->text);
    if (w->fail_after >= 0) {
        recomp_atomic_file_write(f, w->text, (size_t)w->fail_after);
        return 0;
    }
    return recomp_atomic_file_write(f, w->text, n);
}

int main(int argc, char **argv) {
    static const unsigned char old_state[] = {
        'P', 'S', 'T', 0x00, 0x01, 0xFF, 0x0D, 0x0A, 0x1A, 'O', 'L', 'D'
    };
    const char *dir = argc > 1 ? argv[1] : ".";
    char target[512], tmp[512];
    snprintf(target, sizeof target, "%s/boot_state_replace.pst", dir);
    snprintf(tmp, sizeof tmp, "%s/boot_state_replace.pst.tmp", dir);
    remove(target);
    remove(tmp);
    remove_dir(tmp);

    /* 1. replace onto an absent target through the actual serializer binding. */
    Writer first = { "NEW", -1 };
    check(boot_state_write_file_atomic(target, stand_in_writer, &first) == 1, "replace onto absent target");
    check(read_is(target, "NEW", 3), "target content after first replace");
    check(!exists(tmp), "tmp consumed");

    /* 2. replace onto an EXISTING target */
    Writer newer = { "NEWER", -1 };
    check(boot_state_write_file_atomic(target, stand_in_writer, &newer) == 1, "replace onto existing target");
    check(read_is(target, "NEWER", 5), "target content after overwrite");
    check(!exists(tmp), "tmp consumed on overwrite");

    /* 3. null arguments refuse without invoking a serializer. */
    check(boot_state_write_file_atomic(NULL, stand_in_writer, NULL) == 0,
          "refuse null path");

    /* 5. a save that fails partway keeps the previous file byte for byte */
    {
        Writer w = { "PARTIAL-NEW-STATE", 7 };
        check(write_bytes(target, old_state, sizeof old_state), "stage old state");
        check(boot_state_write_file_atomic(target, stand_in_writer, &w) == 0,
              "failed write refuses the save");
        check(read_is(target, old_state, sizeof old_state),
              "old state byte for byte after failed write");
        check(!exists(tmp), "tmp removed after failed write");
    }

    check(boot_state_write_file_atomic(target, NULL, NULL) == 0,
          "refuse null serializer");

    /* A legacy temporary belongs to somebody else; never truncate/remove it. */
    {
        Writer w = { "NEW-STATE", -1 };
        check(write_bytes(tmp, "FOREIGN", 7), "stage foreign temporary");
        check(boot_state_write_file_atomic(target, stand_in_writer, &w) == 1,
              "exclusive transaction ignores a foreign temporary");
        check(read_is(tmp, "FOREIGN", 7), "foreign temporary preserved");
        remove(tmp);
        char missing[700];
        snprintf(missing, sizeof missing, "%s/missing-parent/state.pst", dir);
        check(boot_state_write_file_atomic(missing, stand_in_writer, &w) == 0,
              "uncreatable temporary refuses the save");
        check(read_is(target, "NEW-STATE", 9), "last complete state preserved");
    }

    /* 7. a complete save replaces the previous file */
    {
        Writer w = { "NEW-STATE", -1 };
        check(boot_state_write_file_atomic(target, stand_in_writer, &w) == 1,
              "complete save succeeds");
        check(read_is(target, "NEW-STATE", 9), "target holds the new state");
        check(!exists(tmp), "tmp consumed by complete save");
    }

    remove(target);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: shared streamed replace, absent/existing target, abort, "
         "uncreatable path and foreign-temporary preservation");
    return 0;
}
