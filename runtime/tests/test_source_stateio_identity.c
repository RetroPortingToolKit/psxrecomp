/* Checkpoint identity dimensions: file hash, runtime-binary hash, route hash,
 * configuration digest. Driven by test_source_stateio_identity.py. */
#include "source_stateio_identity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static void check(int cond, const char *what) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

int main(int argc, char **argv) {
    char a[65], b[65], cfg[65], cfg2[65];
    FILE *f;

    if (argc != 2 && argc != 3) return 2;
    if (argc == 3 && strcmp(argv[2], "--expect-overflow") == 0) {
        memset(cfg, 'x', sizeof cfg);
        check(source_stateio_config_digest_hex(cfg) == 0 && cfg[0] == '\0',
              "an overflowing configuration refuses with no digest");
        check(source_stateio_config_digest_accepts("not-a-digest") == 0,
              "an overflowing configuration never accepts a stored digest");
        if (failures) return 1;
        puts("config=refused");
        return 0;
    }

    /* self-hash of this test executable resolves and is stable */
    check(source_stateio_exe_sha256(a) == 1 && strlen(a) == 64, "exe hash resolves");
    check(source_stateio_exe_sha256(b) == 1 && strcmp(a, b) == 0, "exe hash stable");
    check(source_stateio_file_sha256(argv[1], b) == 1 && strcmp(a, b) != 0,
          "distinct file hashes differently");

    /* content sensitivity */
    f = fopen(argv[1], "wb");
    if (!f) return 2;
    fputs("one", f); fclose(f);
    check(source_stateio_file_sha256(argv[1], a) == 1, "hash written file");
    f = fopen(argv[1], "wb");
    if (!f) return 2;
    fputs("two", f); fclose(f);
    check(source_stateio_file_sha256(argv[1], b) == 1 && strcmp(a, b) != 0,
          "file hash content sensitive");
    /* SHA-256("two") */
    check(strcmp(b, "3fc4ccfe745870e2c0d99f71f30ff0656c8dedd41cc1d7d3d376b0dbe685e2f3") == 0,
          "file hash is SHA-256");

    /* refusal cases */
    check(source_stateio_file_sha256("does-not-exist-xyz", b) == 0 && b[0] == '\0',
          "missing file refuses");
    check(source_stateio_file_sha256(NULL, b) == 0, "null path refuses");

    /* The route digest refuses without a route, then is computed once per process:
     * a capture reuses it instead of rehashing (the route file is immutable). */
    {
        char route[65], again[65], direct[65];
#if defined(_WIN32)
        _putenv_s("PSX_INPUT_ROUTE_FILE", "");
#else
        unsetenv("PSX_INPUT_ROUTE_FILE");
#endif
        check(source_stateio_route_sha256(route) == 0 && route[0] == '\0', "no route refuses");
#if defined(_WIN32)
        _putenv_s("PSX_INPUT_ROUTE_FILE", argv[1]);
#else
        setenv("PSX_INPUT_ROUTE_FILE", argv[1], 1);
#endif
        check(source_stateio_route_sha256(route) == 1 && source_stateio_file_sha256(argv[1], direct) == 1 &&
              strcmp(route, direct) == 0, "route digest is the route file's SHA-256");
        f = fopen(argv[1], "wb");
        if (!f) return 2;
        fputs("three", f); fclose(f);
        check(source_stateio_route_sha256(again) == 1 && strcmp(route, again) == 0,
              "route digest computed once per process");
    }

    /* The digest's value depends on the environment, so only shape and
     * stability are asserted here; the driver compares environments. */
    check(source_stateio_config_digest_hex(cfg) == 1 && strlen(cfg) == 64, "config digest shape");
    check(source_stateio_config_digest_hex(cfg2) == 1 && strcmp(cfg, cfg2) == 0,
          "config digest stable");
    check(source_stateio_config_digest_accepts(cfg) == 1, "the current digest accepts itself");
    check(source_stateio_config_digest_accepts("not-a-digest") == 0, "a foreign digest is refused");
    check(source_stateio_config_digest_accepts(NULL) == 0, "no stored digest is refused");
    /* argv[2]: a stored manifest digest; the driver asks how it is judged. */
    if (argc == 3)
        printf("accept=%s\n", source_stateio_config_digest_accepts(argv[2]) ? "yes" : "refused");

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("config=%s\n", cfg);
    puts("PASS: checkpoint identity dimensions");
    return 0;
}
