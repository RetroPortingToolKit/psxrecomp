/* Exercise default-off and explicitly armed capture through real GP0 dispatch. */
#define main gpu_dot_fixture_main
#include "test_gpu_textured_dot_nw_shift_exec.c"
#undef main

static void draw_dot(void) {
    set_dot_packet(0x10004u, 12, 34);
    gp0_execute_command();
}

int main(int argc, char **argv) {
    assert(argc == 2);
    reset_gpu_state_for_test();

    draw_dot();
    assert(last_textured_rect.calls == 1);
    assert(last_textured_rect.x == 12 && last_textured_rect.y == 34);
    assert(gpu_ws_census_seq() == 0 && ws_census == NULL);

    gpu_ws_census_set(1);
    draw_dot();
    assert(last_textured_rect.calls == 2 && gpu_ws_census_seq() == 1);
    assert(gpu_ws_census_dump(100, 100, argv[1]) == 1);
    FILE *csv = fopen(argv[1], "r");
    assert(csv);
    char line[256];
    assert(fgets(line, sizeof(line), csv));
    assert(strncmp(line, "frame,src_addr,", 15) == 0);
    assert(fgets(line, sizeof(line), csv));
    assert(strncmp(line, "100,0x00010004,", 15) == 0);
    assert(strstr(line, ",12,34,12,12,0,0x6C,0"));
    assert(fgets(line, sizeof(line), csv) == NULL);
    assert(fclose(csv) == 0);
    assert(remove(argv[1]) == 0);

    gpu_ws_census_set(0);
    draw_dot();
    assert(last_textured_rect.calls == 3 && gpu_ws_census_seq() == 1);
    free(ws_census);
    ws_census = NULL;
    puts("PASS: drawing works without census allocation; armed capture exports and stops");
    return 0;
}
