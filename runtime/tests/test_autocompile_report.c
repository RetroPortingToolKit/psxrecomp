/*
 * The overlay compile results of a start must reach the run report.
 *
 * The runtime counts the compile driver's built and failed units and keeps the
 * tail of its output, but served both only through the debug server, which a
 * product does not have. A unit the compiler rejects runs interpreted and is
 * not "degraded", so every build looked clean while clang refused
 * overlay units on Windows. autocompile_report_json() is what the run report
 * now carries under "overlay_compile".
 *
 * The driver here is a text file that the configured command prints ("cat" or
 * "type"): exactly the lines tools/compile_overlays.py prints. So this runs
 * the real path on every platform: spawn, pipe, line parser, accounting.
 *
 *   no run yet        every count is zero, no class, no tail;
 *   a clean run       the unit counts of its result line, no failure;
 *   a failing run     the count, the class, the first SHARD FAIL line, the
 *                     compiler's first error line, the tail; and one stderr
 *                     line per failure class;
 *   the same again    counts add up; the stderr line is not printed again;
 *   too many classes  the bound holds and says how many failures it left out.
 *   a quit mid-run    the report says that the quit stopped a run, and keeps the
 *                     result line that run had printed.
 *
 * Build/run: ctest -R autocompile_report_test
 */
#include "autocompile.h"
#include "overlay_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#  define DRIVER "cmd /C type"
static void nap_ms(int ms) { Sleep((DWORD)ms); }
#else
#  include <time.h>
#  include <unistd.h>
#  define DRIVER "cat"
static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* autocompile.c calls into the overlay loader when a run ends or publishes a
 * unit. Nothing is published here; the symbols only have to resolve.
 * Signatures must match overlay_loader.h exactly. */
struct OverlayPreparedImage { unsigned id; };
OverlayPreparedImage *overlay_loader_prepare_published(const char *dll_path) {
    (void)dll_path; return NULL;
}
int  overlay_loader_commit_published(OverlayPreparedImage *image) { (void)image; return 0; }
void overlay_loader_discard_prepared(OverlayPreparedImage *image) { (void)image; }
void overlay_loader_rescan(void) { }

static int failures;

#define CHECK(cond, msg) do {                                                 \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; }        \
} while (0)

static char s_json[32 * 1024];

static const char *report(void) {
    int n = autocompile_report_json(s_json, (int)sizeof s_json);
    if (n <= 0 || s_json[0] != '{' || s_json[n - 1] != '}') {
        fprintf(stderr, "FAIL: the report is not one complete object (n=%d)\n", n);
        failures++;
        s_json[0] = '\0';
    }
    /* A JSON string may not hold a raw control character. */
    for (const char *p = s_json; *p; p++)
        if ((unsigned char)*p < 0x20) {
            fprintf(stderr, "FAIL: raw control character 0x%02x in the report\n",
                    (unsigned char)*p);
            failures++;
            break;
        }
    return s_json;
}

static void expect(const char *needle, const char *what) {
    if (!strstr(report(), needle)) {
        fprintf(stderr, "FAIL: %s\n  expected: %s\n  report: %.1500s\n",
                what, needle, s_json);
        failures++;
    }
}

/* The number of one field of the report, or -1. */
static long field(const char *name) {
    char key[96];
    snprintf(key, sizeof key, "\"%s\":", name);
    const char *at = strstr(report(), key);
    return at ? strtol(at + strlen(key), NULL, 10) : -1;
}

static void write_driver(const char *name, const char *text) {
    FILE *f = fopen(name, "wb");
    if (!f) { fprintf(stderr, "FAIL: cannot write %s\n", name); failures++; return; }
    fputs(text, f);
    fclose(f);
}

/* One driver run, to the point where the runtime has accounted it. */
static void run_driver(const char *cwd, const char *file) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, DRIVER " %s", file);
    autocompile_configure(cmd, cwd);
    /* After a run with failed units the runtime waits before it starts the
     * next one (1 s, then 2 s, ...): ask until it agrees. */
    int started = 0;
    for (int i = 0; i < 800 && !started; i++) {
        started = autocompile_request();
        if (!started) nap_ms(25);
    }
    if (!started) {
        fprintf(stderr, "FAIL: the driver run for %s did not start\n", file);
        failures++;
        return;
    }
    for (int i = 0; i < 600 && autocompile_busy(); i++) {
        autocompile_poll_main();
        nap_ms(25);
    }
    CHECK(!autocompile_busy(), "the driver run ends and is accounted");
}

static int count_lines_with(const char *path, const char *needle) {
    char line[2048];
    int n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    while (fgets(line, sizeof line, f))
        if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

static const char CLEAN[] =
    "  unit 0007E000 built\n"
    "\n=== SHARD BUILD SUMMARY ===\n"
    "  built OK : 3\n"
    "  FAILED   : 0\n"
    "PSX_SHARD_RESULT ok=3 failed=0 skipped=2 capacity_fastpath=0\n";

/* The lines compile_overlays.py prints when clang rejects a unit, then a unit
 * of another failure class, then its summary. */
static const char FAILING[] =
    "  COMPILE ERROR (exit 1):\n"
    "In file included from unit_0007E000.c:3:\n"
    "unit_0007E000.c:412:6: error: redeclaration of 'func_8007E120' cannot add 'dllexport' attribute\n"
    "unit_0007E000.c:88:13: note: previous declaration is here\n"
    "  FAILED\n"
    "\n"
    "  SHARD FAIL [compile] 0007E000_1C02BEFB: gcc/tcc compile failed (see COMPILE ERROR above)\n"
    "  COMPILE ERROR (exit 1):\n"
    "unit_00081000.c:9:1: error: a second, different error \"quoted\" with a back\\slash\n"
    "  FAILED\n"
    "  SHARD FAIL [compile] 00081000_AAAA0001: gcc/tcc compile failed (see COMPILE ERROR above)\n"
    "  SHARD FAIL [no_ranges] 00085000_BBBB0002: DLL built but no _full.ranges (undispatchable)\n"
    "\n=== SHARD BUILD SUMMARY ===\n"
    "  built OK : 1\n"
    "  FAILED   : 3\n"
    "PSX_SHARD_RESULT ok=1 failed=3 skipped=5 capacity_fastpath=0\n";

int main(void) {
    char cwd[512], dir[640], errlog[700];
#ifdef _WIN32
    GetCurrentDirectoryA(sizeof cwd, cwd);
    snprintf(dir, sizeof dir, "%s\\autocompile_report_test.%lu", cwd,
             (unsigned long)GetCurrentProcessId());
    CHECK(_mkdir(dir) == 0, "work folder");
    CHECK(_chdir(dir) == 0, "enter the work folder");
#else
    if (!getcwd(cwd, sizeof cwd)) return 1;
    snprintf(dir, sizeof dir, "%s/autocompile_report_test.%ld", cwd, (long)getpid());
    char mk[700];
    snprintf(mk, sizeof mk, "mkdir -p '%s'", dir);
    CHECK(system(mk) == 0, "work folder");
    CHECK(chdir(dir) == 0, "enter the work folder");
#endif
    write_driver("clean.txt", CLEAN);
    write_driver("failing.txt", FAILING);
    {
        /* Ten classes: two more than the report names. */
        char many[2048];
        int o = 0;
        for (int i = 0; i < 10; i++)
            o += snprintf(many + o, sizeof many - (size_t)o,
                          "  SHARD FAIL [class_%d] unit_%d: made-up failure\n", i, i);
        snprintf(many + o, sizeof many - (size_t)o,
                 "PSX_SHARD_RESULT ok=0 failed=10 skipped=0\n");
        write_driver("many.txt", many);
    }
    /* The runtime's stderr lines go to a file this test can read. */
    snprintf(errlog, sizeof errlog, "stderr.txt");
    CHECK(freopen(errlog, "wb", stderr) != NULL, "stderr to a file");

    /* --- before any run ---------------------------------------------------- */
    expect("\"configured\":0", "an unconfigured start says so");
    expect("\"runs\":0,\"runs_failed\":0,\"runs_with_result\":0,"
           "\"units_attempted\":0,\"units_compiled\":0,\"units_failed\":0,"
           "\"units_skipped\":0,\"fail_lines\":0",
           "a start without a compile run reports zeros");
    expect("\"failure_classes\":[]", "no failure class before any run");
    expect("\"output_tail\":\"\"", "no output before any run");

    /* --- a clean run ------------------------------------------------------- */
    run_driver(dir, "clean.txt");
    expect("\"configured\":1", "a configured start says so");
    expect("\"runs\":1,", "one run");
    expect("\"runs_with_result\":1,\"units_attempted\":3,\"units_compiled\":3,"
           "\"units_failed\":0,\"units_skipped\":2,\"fail_lines\":0",
           "a clean run reports its units and no failure");
    expect("\"failure_classes\":[]", "a clean run names no failure class");
    expect("PSX_SHARD_RESULT ok=3 failed=0 skipped=2", "the tail holds the driver's last lines");

    /* --- a run with three failed units in two classes ---------------------- */
    run_driver(dir, "failing.txt");
    expect("\"runs\":2,", "two runs");
    expect("\"runs_with_result\":2,\"units_attempted\":7,\"units_compiled\":4,"
           "\"units_failed\":3,\"units_skipped\":7,\"fail_lines\":3",
           "the failed units are counted, and the sums cover both runs");
    expect("{\"class\":\"compile\",\"count\":2,"
           "\"first\":\"SHARD FAIL [compile] 0007E000_1C02BEFB: gcc/tcc compile failed "
           "(see COMPILE ERROR above)\","
           "\"first_error\":\"unit_0007E000.c:412:6: error: redeclaration of "
           "'func_8007E120' cannot add 'dllexport' attribute\"}",
           "the compile class carries its first line and the compiler's first error line");
    expect("{\"class\":\"no_ranges\",\"count\":1,"
           "\"first\":\"SHARD FAIL [no_ranges] 00085000_BBBB0002: DLL built but no "
           "_full.ranges (undispatchable)\",\"first_error\":\"\"}",
           "a class without a compiler error carries its first line and an empty error");
    expect("PSX_SHARD_RESULT ok=1 failed=3 skipped=5", "the tail is the latest run's");
    CHECK(strstr(report(), "\\\"quoted\\\" with a back\\\\slash") != NULL,
          "quotes and backslashes of the driver's output are escaped in the tail");

    /* --- the same failures in a later run ---------------------------------- */
    run_driver(dir, "failing.txt");
    expect("\"units_failed\":6,", "a unit that fails again is counted again");
    expect("\"fail_lines\":6", "the live count follows");
    expect("{\"class\":\"compile\",\"count\":4,", "the class count adds up");
    expect("\"first_error\":\"unit_0007E000.c:412:6: error: redeclaration",
           "the first error line of a class stays the first");

    /* --- more classes than the report names -------------------------------- */
    run_driver(dir, "many.txt");
    expect("\"failure_classes_max\":8,", "the report says its bound");
    expect("\"fail_lines_in_unnamed_classes\":4,",
           "failures in classes past the bound are counted, not dropped in silence");
    expect("{\"class\":\"class_5\",\"count\":1,", "the eighth class is still named");
    CHECK(strstr(report(), "\"class\":\"class_6\"") == NULL,
          "the ninth class is not named");
    expect("\"fail_lines\":16", "every failure line is counted");

    /* --- a buffer that is too small gets an empty object, not half of one -- */
    {
        char tiny[64];
        int n = autocompile_report_json(tiny, (int)sizeof tiny);
        CHECK(n == 2 && strcmp(tiny, "{}") == 0,
              "a buffer too small for the object gets {}");
    }

    /* --- a quit while a run is going --------------------------------------
     * The driver prints its result line and then stays alive. A normal quit
     * stops it and stores the state idle; the report is written after that.
     * It must say that a run was stopped, and it must keep the result line. */
    {
        const long results_before = field("runs_with_result");
        const long compiled_before = field("units_compiled");
        const long skipped_before = field("units_skipped");
#ifdef _WIN32
        write_driver("slow.cmd", "@type clean.txt\r\n@ping -n 60 127.0.0.1 >nul\r\n");
        autocompile_configure(".\\slow.cmd", dir);   /* the runtime runs it through cmd.exe /C */
#else
        write_driver("slow.sh", "cat clean.txt\nsleep 60\n");
        autocompile_configure("sh slow.sh", dir);
#endif
        int started = 0, seen = 0;
        for (int i = 0; i < 800 && !started; i++) {
            started = autocompile_request();
            if (!started) nap_ms(25);
        }
        CHECK(started, "the slow driver starts");
        /* Its result line is read while it still runs; the report counts it. */
        for (int i = 0; i < 400 && !seen; i++) {
            seen = field("runs_with_result") == results_before + 1;
            if (!seen) nap_ms(25);
        }
        CHECK(seen, "the result line of the running driver is read");
        expect("\"state\":\"running\"", "the slow run is still going");
        expect("\"runs_stopped_at_quit\":0,", "no run was stopped before the quit");
        autocompile_shutdown();
        expect("\"state\":\"idle\"", "after the quit nothing runs");
        expect("\"runs_stopped_at_quit\":1,", "the report says that the quit stopped a run");
        CHECK(field("runs_with_result") == results_before + 1 &&
              field("units_compiled") == compiled_before + 3 &&
              field("units_skipped") == skipped_before + 2,
              "the stopped run's result line is kept, and counted once");
        autocompile_shutdown();
        expect("\"runs_stopped_at_quit\":1,", "a second shutdown counts nothing");
    }
    fflush(stderr);
    {
        /* One line per failure class per start: compile and no_ranges from the
         * two failing runs, eight of the made-up classes, one notice. */
        int unit_lines = count_lines_with(errlog, "overlay compile: a unit failed");
        int compile_lines = count_lines_with(errlog, "SHARD FAIL [compile]");
        int notice = count_lines_with(errlog, "later classes are counted, not named");
        int with_error = count_lines_with(errlog,
            "(see COMPILE ERROR above) | unit_0007E000.c:412:6: error: redeclaration");
        int test_fails = count_lines_with(errlog, "FAIL: ");
        printf("stderr: %d unit lines, %d for the compile class, %d bound notice\n",
               unit_lines, compile_lines, notice);
        if (unit_lines != 8 || compile_lines != 1 || notice != 1 || with_error != 1) {
            printf("FAIL: expected 8 unit lines (2 real classes + 6 made-up ones "
                   "that fit), 1 for the compile class with its error line, 1 notice\n");
            failures++;
        }
        if (test_fails > 0) {
            char line[2048];
            FILE *f = fopen(errlog, "rb");
            while (f && fgets(line, sizeof line, f))
                if (strstr(line, "FAIL: ") || line[0] == ' ') fputs(line, stdout);
            if (f) fclose(f);
        }
    }

    if (failures) {
        printf("FAILED (%d)\n", failures);
        return 1;
    }
    printf("test_autocompile_report: all checks passed\n");
    return 0;
}
