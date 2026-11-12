#include "../src/gpu_sw_edges.h"

#include <stdio.h>

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)

int main(void) {
    int long_x = 0, short_x = 0;

    /* A right triangle's final scanline is y2-1; y2 itself is excluded by the
     * caller. Pin the DDA values used to produce the half-open X span. */
    psx_triangle_edges_at_y(0, 0, 8, 0, 0, 8, 0, &long_x, &short_x);
    CHECK(long_x == 0 && short_x == 8, "top scanline spans [0,8)");
    psx_triangle_edges_at_y(0, 0, 8, 0, 0, 8, 7, &long_x, &short_x);
    CHECK(long_x == 0 && short_x == 1, "last scanline spans one pixel");

    CHECK(psx_edge_step(8, 8) == (1LL << 32),
          "positive unit slope has exact 32.32 step");
    CHECK(psx_edge_step(-8, 8) == -(1LL << 32),
          "negative unit slope has exact 32.32 step");

    /* Signed coordinates, nonintegral slopes, and the full int32 helper
     * endpoints. Expected steps are signed ceil(abs(dx)*2^32/dy). */
    static const struct { int dx, dy; int64_t expected; } steps[] = {
        { -1, 3, -INT64_C(1431655766) },
        { 1, 3, INT64_C(1431655766) },
        { INT32_MIN, 1, INT64_MIN },
        { INT32_MIN, 3, -INT64_C(3074457345618258603) },
        { INT32_MIN, INT32_MAX, -INT64_C(4294967299) },
        { INT32_MAX, 1, INT64_C(9223372032559808512) },
        { INT32_MAX, 3, INT64_C(3074457344186602838) },
        { INT32_MAX, INT32_MAX, INT64_C(4294967296) },
        { 0, 3, INT64_C(0) },
        { -262144, 262144, -INT64_C(4294967296) },
        { 262144, 262144, INT64_C(4294967296) },
    };
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; ++i)
        CHECK(psx_edge_step(steps[i].dx, steps[i].dy) == steps[i].expected,
              "rounded step matches exact integer golden");
    CHECK(psx_edge_fp(-1) == -INT64_C(2048), "negative coordinate bias");
    CHECK(psx_edge_fp(0) == INT64_C(4294965248), "zero coordinate bias");
    CHECK(psx_edge_fp(INT32_MIN) == -INT64_C(9223372032559810560),
          "smallest int32 coordinate fits biased fixed point");
    CHECK(psx_edge_fp(INT32_MAX) == INT64_MAX - INT64_C(2047),
          "largest int32 coordinate fits biased fixed point");

    psx_triangle_edges_at_y(-8, -8, 0, -8, -8, 0, -1, &long_x, &short_x);
    CHECK(long_x == -8 && short_x == -7, "negative translated last scanline");
    psx_triangle_edges_at_y(-4, -4, -8, 4, 0, 4, 3, &long_x, &short_x);
    CHECK(long_x == 0 && short_x == -7, "flat-bottom negative-coordinate edges");
    psx_triangle_edges_at_y(-131072, -131072, 131072, -131072,
                            -131072, 131072, 131071, &long_x, &short_x);
    CHECK(long_x == -131072 && short_x == -131071,
          "scaled precise-coordinate last scanline");
    psx_triangle_edges_at_y(INT32_MIN, 0, INT32_MIN + 8, 0,
                            INT32_MIN, 8, 7, &long_x, &short_x);
    CHECK(long_x == INT32_MIN && short_x == INT32_MIN + 1,
          "minimum translated triangle does not overflow");
    psx_triangle_edges_at_y(INT32_MAX - 8, 0, INT32_MAX, 0,
                            INT32_MAX - 8, 8, 7, &long_x, &short_x);
    CHECK(long_x == INT32_MAX - 8 && short_x == INT32_MAX - 7,
          "maximum translated triangle does not overflow");

    if (failures) return 1;
    puts("ALL PASS");
    return 0;
}
