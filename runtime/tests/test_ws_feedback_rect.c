#include "ws_feedback_rect.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* Kula's lethargy sprites: draw buffer at VRAM x 320, 320 wide, 16:9 margin. */
enum { LEFT = 320, WIDTH = 320, MARGIN = 54 };

static int span_equals(const WsFeedbackSpan *span, int32_t dst_x, int32_t src_x,
                       int32_t w) {
    return span->dst_x == dst_x && span->src_x == src_x && span->w == w;
}

static void test_left_edge_sprite_gets_left_band(void) {
    WsFeedbackSpan spans[2];
    int count = ws_feedback_rect_spans(320, 256, 0, LEFT, WIDTH, MARGIN, spans);
    assert(count == 1 && span_equals(&spans[0], 266, -54, 54));
}

static void test_right_edge_sprite_gets_right_band(void) {
    WsFeedbackSpan spans[2];
    int count = ws_feedback_rect_spans(576, 64, 256, LEFT, WIDTH, MARGIN, spans);
    assert(count == 1 && span_equals(&spans[0], 640, 320, 54));
}

static void test_full_width_sprite_gets_both_bands(void) {
    WsFeedbackSpan spans[2];
    int count = ws_feedback_rect_spans(320, 320, 0, LEFT, WIDTH, MARGIN, spans);
    assert(count == 2 && span_equals(&spans[1], 640, 320, 54));
}

static void test_inner_sprite_gets_no_band(void) {
    WsFeedbackSpan spans[2];
    assert(ws_feedback_rect_spans(400, 64, 80, LEFT, WIDTH, MARGIN, spans) == 0);
}

static void test_offscreen_sprite_gets_no_band(void) {
    WsFeedbackSpan spans[2];
    assert(ws_feedback_rect_spans(LEFT - 64, 32, 0, LEFT, WIDTH, MARGIN, spans) == 0);
    assert(ws_feedback_rect_spans(LEFT - 32, 32, 0, LEFT, WIDTH, MARGIN, spans) == 0);
    assert(ws_feedback_rect_spans(LEFT + WIDTH, 32, 0, LEFT, WIDTH, MARGIN, spans) == 0);
    assert(ws_feedback_rect_spans(LEFT + WIDTH + 16, 32, 0, LEFT, WIDTH, MARGIN, spans) == 0);
}

static void test_zero_margin_gets_no_band(void) {
    WsFeedbackSpan spans[2];
    assert(ws_feedback_rect_spans(320, 320, 0, LEFT, WIDTH, 0, spans) == 0);
}

static void test_empty_sprite_rejected(void) {
    WsFeedbackSpan spans[2];
    assert(ws_feedback_rect_spans(320, 0, 0, LEFT, WIDTH, MARGIN, spans) == 0);
}

int main(void) {
    test_left_edge_sprite_gets_left_band();
    test_right_edge_sprite_gets_right_band();
    test_full_width_sprite_gets_both_bands();
    test_inner_sprite_gets_no_band();
    test_offscreen_sprite_gets_no_band();
    test_zero_margin_gets_no_band();
    test_empty_sprite_rejected();
    puts("ws_feedback_rect: all tests passed");
    return 0;
}
