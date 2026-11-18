/* psx_file_create_new: a new file is created and written, an existing file is
 * refused and left as it is, and a text stream writes what a text-mode fopen
 * writes on this host. Built by test_file_create_new.py with -std=c11 and no
 * feature macro, so the header must declare what it uses itself. */
#include "psx_file_create_new.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(condition, ...) do { ++checks; if (!(condition)) { ++failures; \
    fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static size_t slurp(const char *path, unsigned char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return (size_t)-1;
    n = fread(out, 1, cap, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    static const char text[] = "frame\tpc\n1\t80010000\n";
    static const unsigned char bytes[] = {0x0A, 0x0D, 0x0A, 0x1A, 0x00, 0xFF, 0x0A};
    char made[4096], plain[4096], binary[4096], absent[4096];
    unsigned char a[64], b[64];
    size_t na, nb;
    FILE *f;
    if (argc != 2) return 2;
    snprintf(made, sizeof made, "%s/made.tsv", argv[1]);
    snprintf(plain, sizeof plain, "%s/plain.tsv", argv[1]);
    snprintf(binary, sizeof binary, "%s/frame.bin", argv[1]);
    snprintf(absent, sizeof absent, "%s/no-such-folder/made.tsv", argv[1]);

    /* A text stream: the same bytes on disk as a text-mode fopen gives. */
    f = psx_file_create_new(made, 0);
    CHECK(f != NULL, "text file not created (errno %d)", errno);
    if (f) { CHECK(fputs(text, f) >= 0 && fclose(f) == 0, "text file not written"); }
    f = fopen(plain, "w");
    CHECK(f != NULL, "reference file not created");
    if (f) { fputs(text, f); fclose(f); }
    na = slurp(made, a, sizeof a);
    nb = slurp(plain, b, sizeof b);
    CHECK(na == nb && na != (size_t)-1 && na >= sizeof text - 1 && memcmp(a, b, na) == 0,
          "text stream wrote %lu byte(s), a text-mode fopen %lu", (unsigned long)na, (unsigned long)nb);

    /* A binary stream: the bytes as given, on every host. */
    f = psx_file_create_new(binary, 1);
    CHECK(f != NULL, "binary file not created (errno %d)", errno);
    if (f) { CHECK(fwrite(bytes, 1, sizeof bytes, f) == sizeof bytes && fclose(f) == 0,
                   "binary file not written"); }
    na = slurp(binary, a, sizeof a);
    CHECK(na == sizeof bytes && memcmp(a, bytes, sizeof bytes) == 0,
          "binary stream wrote %lu byte(s), not the %lu given", (unsigned long)na,
          (unsigned long)sizeof bytes);

    /* A file that exists is refused in both modes and is not touched. */
    for (int mode = 0; mode < 2; ++mode) {
        errno = 0;
        f = psx_file_create_new(binary, mode);
        CHECK(f == NULL, "an existing file was opened (mode %d)", mode);
        CHECK(errno == EEXIST, "an existing file gave errno %d, not EEXIST (mode %d)", errno, mode);
        if (f) fclose(f);
        nb = slurp(binary, b, sizeof b);
        CHECK(nb == sizeof bytes && memcmp(b, bytes, sizeof bytes) == 0,
              "a refused create changed the existing file (mode %d)", mode);
    }
    errno = 0;
    f = psx_file_create_new(made, 0);
    CHECK(f == NULL && errno == EEXIST, "an existing text file was not refused (errno %d)", errno);
    if (f) fclose(f);

    /* A path that cannot be created gives NULL, not a stream. */
    f = psx_file_create_new(absent, 1);
    CHECK(f == NULL, "a file in a missing folder was created");
    if (f) fclose(f);

    if (failures) { fprintf(stderr, "%d of %d checks failed\n", failures, checks); return 1; }
    printf("PASS: %d checks\n", checks);
    return 0;
}
