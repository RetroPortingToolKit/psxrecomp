#include "ws_repeat_rect.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void assert_span(const WsRepeatSpan *span, int32_t x, int32_t u,
                        int32_t w) {
    assert(span->x == x);
    assert(span->u == u);
    assert(span->w == w);
}

static int emit_spans(int32_t x, int32_t u, int32_t w, int32_t period,
                      int32_t left, int32_t width, int32_t margin,
                      WsRepeatSpan *out, int capacity) {
    for (int i = 0; i < capacity; i++) {
        out[i].x = 0x12345678;
        out[i].u = 0x12345678;
        out[i].w = 0x12345678;
    }
    return ws_repeat_rect_spans(x, u, w, period, left, width, margin, out,
                                capacity);
}

static void test_margin_zero(void) {
    WsRepeatSpan spans[4];
    assert(emit_spans(0, 0, 128, 512, 0, 512, 0, spans, 4) == 0);
    assert(spans[0].x == 0x12345678);
}

static void test_both_sides_and_u_offsets(void) {
    WsRepeatSpan spans[4];
    int count = emit_spans(-10, 0, 20, 512, 0, 512, 20, spans, 4);
    assert(count == 2);
    assert_span(&spans[0], -10, 0, 10);
    assert_span(&spans[1], 512, 10, 10);
}

static void test_xoffset_framebuffer(void) {
    WsRepeatSpan spans[4];
    int count = emit_spans(502, 12, 20, 512, 512, 512, 20, spans, 4);
    assert(count == 2);
    assert_span(&spans[0], 502, 12, 10);
    assert_span(&spans[1], 1024, 22, 10);
}

static void test_negative_x_partial_strip(void) {
    WsRepeatSpan spans[4];
    int count = emit_spans(-30, 10, 60, 512, 0, 512, 64, spans, 4);
    assert(count == 2);
    assert_span(&spans[0], -30, 10, 30);
    assert_span(&spans[1], 512, 40, 30);

    count = emit_spans(500, 4, 20, 512, 0, 512, 16, spans, 4);
    assert(count == 2);
    assert_span(&spans[0], -12, 4, 12);
    assert_span(&spans[1], 512, 16, 8);
}

static void mark_span_coverage(const WsRepeatSpan *span, int *left_cov,
                               int *right_cov) {
    for (int32_t x = span->x; x < span->x + span->w; x++) {
        if (x >= -256 && x < 0)
            left_cov[x + 256]++;
        else if (x >= 512 && x < 768)
            right_cov[x - 512]++;
        else
            assert(0 && "span overlapped canonical area or outer margin");
    }
}

static void test_full_512_composite_all_phases(void) {
    for (int phase = 0; phase < 256; phase++) {
        int left_cov[256] = {0};
        int right_cov[256] = {0};
        WsRepeatSpan spans[4];
        int total = 0;

        if (phase == 0) {
            const int32_t pieces[2][3] = {
                {0, 0, 256},
                {256, 0, 256},
            };
            for (int i = 0; i < 2; i++) {
                int count = emit_spans(pieces[i][0], pieces[i][1],
                                       pieces[i][2], 512, 0, 512, 256,
                                       spans, 4);
                total += count;
                for (int j = 0; j < count; j++)
                    mark_span_coverage(&spans[j], left_cov, right_cov);
            }
        } else {
            const int32_t pieces[4][3] = {
                {512 - phase, 0, phase},
                {256, phase, 256 - phase},
                {256 - phase, 0, phase},
                {0, phase, 256 - phase},
            };
            for (int i = 0; i < 4; i++) {
                int count = emit_spans(pieces[i][0], pieces[i][1],
                                       pieces[i][2], 512, 0, 512, 256,
                                       spans, 4);
                total += count;
                for (int j = 0; j < count; j++)
                    mark_span_coverage(&spans[j], left_cov, right_cov);
            }
        }

        assert(total == (phase == 0 ? 2 : 4));
        for (int i = 0; i < 256; i++) {
            assert(left_cov[i] == 1);
            assert(right_cov[i] == 1);
        }
    }
}

static void test_bad_inputs_and_capacity(void) {
    WsRepeatSpan spans[8];

    assert(emit_spans(0, 0, 20, 0, 0, 512, 32, spans, 8) == 0);
    assert(emit_spans(0, 0, 0, 512, 0, 512, 32, spans, 8) == 0);
    assert(emit_spans(0, 0, 257, 512, 0, 512, 32, spans, 8) == 0);
    assert(emit_spans(0, -1, 20, 512, 0, 512, 32, spans, 8) == 0);
    assert(emit_spans(0, 240, 20, 512, 0, 512, 32, spans, 8) == 0);
    assert(emit_spans(9000, 0, 20, 512, 0, 512, 32, spans, 8) == 0);
    assert(emit_spans(0, 0, 20, 512, -1, 512, 32, spans, 8) == 0);
    assert(emit_spans(0, 0, 20, 512, 0, 0, 32, spans, 8) == 0);
    assert(emit_spans(0, 0, 20, 512, 0, 512, -1, spans, 8) == 0);

    assert(emit_spans(-10, 0, 20, 512, 0, 512, 20, spans, 1) == 0);
    assert(spans[0].x == 0x12345678);

    assert(emit_spans(0, 0, 1, 1, 0, 512, 2048, spans, 8) == 0);
}

static void test_tag_table(void) {
    WsRepeatRectTag tags[WS_REPEAT_RECT_TAG_TABLE_SIZE];
    uint32_t packet[] = {
        0x64010203u, 0x00100020u, 0x00040008u, 0x00100010u
    };
    WsPrepassPacketGuard guard = ws_prepass_packet_guard(packet, 4u);

    ws_repeat_rect_tag_clear(tags);
    ws_repeat_rect_tag_insert(tags, 0x80010004u, 512, &guard, 100u);

    assert(ws_repeat_rect_tag_lookup(tags, 0x80010004u, packet, 4u, 100u) ==
           512);
    assert(ws_repeat_rect_tag_lookup(tags, 0x80010004u, packet, 4u, 102u) ==
           512);
    assert(ws_repeat_rect_tag_lookup(tags, 0x80010004u, packet, 4u, 99u) ==
           0);
    assert(ws_repeat_rect_tag_lookup(tags, 0x80010004u, packet, 4u, 103u) ==
           0);
    assert(ws_repeat_rect_tag_lookup(tags, 0x80010008u, packet, 4u, 100u) ==
           0);
    assert(ws_repeat_rect_tag_lookup(tags, 0x80010004u, packet, 3u, 100u) ==
           0);

    packet[3] = 0x00200010u;
    assert(ws_repeat_rect_tag_lookup(tags, 0x80010004u, packet, 4u, 100u) ==
           0);
    packet[3] = 0x00100010u;

    ws_repeat_rect_tag_insert(tags, 0x80010004u, 256, &guard, 104u);
    assert(ws_repeat_rect_tag_lookup(tags, 0x80010004u, packet, 4u, 104u) ==
           256);

    ws_repeat_rect_tag_insert(tags, 0x80010404u, 128, &guard, 104u);
    assert(ws_repeat_rect_tag_lookup(tags, 0x80010404u, packet, 4u, 104u) ==
           128);

    ws_repeat_rect_tag_insert(tags, 0x80010804u, 0, &guard, 104u);
    assert(ws_repeat_rect_tag_lookup(tags, 0x80010804u, packet, 4u, 104u) ==
           0);

    ws_repeat_rect_tag_clear(tags);
    assert(ws_repeat_rect_tag_lookup(tags, 0x80010004u, packet, 4u, 104u) ==
           0);
}

int main(void) {
    test_margin_zero();
    test_both_sides_and_u_offsets();
    test_xoffset_framebuffer();
    test_negative_x_partial_strip();
    test_full_512_composite_all_phases();
    test_bad_inputs_and_capacity();
    test_tag_table();

    puts("ws_repeat_rect_test: PASS");
    return 0;
}
