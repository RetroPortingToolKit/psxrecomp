#include "input_replay.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { puts(#x); return 1; } } while (0)
int main(int argc, char **argv) {
    CHECK(argc == 2);
    FILE *f = fopen(argv[1], "w"); CHECK(f);
    fputs("# neutral then steer\n2 ffff\n3 bfdf\n1 bfff\n", f); fclose(f);
    CHECK(input_replay_load(argv[1]));
    CHECK(input_replay_buttons(0) == -1);
    input_replay_on_load(100);
    CHECK(input_replay_buttons(99) == -1);
    CHECK(input_replay_buttons(100) == 65535);
    CHECK(input_replay_buttons(101) == 65535);
    CHECK(input_replay_buttons(102) == 0xbfdf);
    CHECK(input_replay_buttons(102) == 0xbfdf);
    CHECK(input_replay_buttons(104) == 0xbfdf);
    CHECK(input_replay_buttons(105) == 0xbfff);
    CHECK(input_replay_buttons(106) == 65535);
    CHECK(input_replay_buttons(102) == 0xbfdf);
    input_replay_on_load(200);
    CHECK(input_replay_frame(200) == 0);
    CHECK(input_replay_buttons(202) == 0xbfdf);
    f = fopen(argv[1], "w"); CHECK(f);
    fputs("0 bfff\n", f); fclose(f);
    CHECK(!input_replay_load(argv[1]));
    const char *bad[] = {"-1 bfff\n", "4294967296 bfff\n", "1 10000\n", "1 bfff junk\n", "1 -1\n"};
    for (unsigned i=0; i<sizeof bad/sizeof *bad; i++) {
        f = fopen(argv[1], "w"); CHECK(f); fputs(bad[i], f); fclose(f);
        CHECK(!input_replay_load(argv[1]));
    }
    remove(argv[1]);
    puts("input replay: PASS");
    return 0;
}
