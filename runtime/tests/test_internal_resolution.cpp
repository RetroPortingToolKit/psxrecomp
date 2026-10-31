// internal_resolution.h: preset ids, the value encoding, and the integer
// scale each preset asks for. Presets x reference
// 240/480 x backend maxima (the GL clamp at 16384 is 16x; 18x needs the
// windowed high-resolution surface or a 32768-limit GPU).

#include "internal_resolution.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

static int g_failures = 0;

static void expect(const char* what, long long got, long long want) {
    if (got != want) {
        std::printf("FAIL %s: got %lld want %lld\n", what, got, want);
        ++g_failures;
    }
}

int main() {
    // Presets at the NTSC 240-line reference (R4 races are 320x240).
    struct { const char* id; int s240; int s480; } table[] = {
        { "native", 1, 1 }, { "720p", 3, 2 }, { "1080p", 5, 3 }, { "1440p", 6, 3 },
        { "4k", 9, 5 }, { "5k", 12, 6 }, { "8k", 18, 9 },
    };
    for (const auto& t : table) {
        int v = 0;
        expect(t.id, psx_ir_parse(t.id, &v), 1);
        expect(t.id, psx_resolve_internal_scale(v, 240, 0, 32), t.s240);
        expect(t.id, psx_resolve_internal_scale(v, 480, 0, 32), t.s480);
        expect("id round trip", std::strcmp(psx_ir_id_for(v), t.id), 0);
    }
    // Fixed scale choices are independent of title reference height and SSAA.
    // The backend's admitted maximum still bounds every request.
    for (int scale = 1; scale <= PSX_IR_MAX_SCALE; ++scale) {
        char id[8];
        std::snprintf(id, sizeof id, "%dx", scale);
        int value = 0;
        expect(id, psx_ir_parse(id, &value), 1);
        expect("fixed encoding", value, PSX_IR_SCALE(scale));
        expect("fixed valid", psx_ir_value_valid(value), 1);
        expect("fixed id", std::strcmp(psx_ir_id_for(value), id), 0);
        for (int reference : {0, 240, 256, 480})
            expect("fixed reference", psx_resolve_internal_scale(value, reference, 8640, 32), scale);
        expect("fixed ignores SSAA", psx_resolve_internal_scale_ss(value, 240, 1080, 32, 4000), scale);
        expect("fixed portable clamp", psx_resolve_internal_scale(value, 240, 0, 4), scale > 4 ? 4 : scale);
        expect("fixed texture clamp", psx_resolve_internal_scale(value, 240, 0, 16), scale > 16 ? 16 : scale);
    }
    expect("invalid fixed lower bound", psx_ir_value_valid(PSX_IR_SCALE_BASE), 0);
    expect("invalid fixed upper bound", psx_ir_value_valid(PSX_IR_SCALE(33)), 0);

    // Backend maxima: software/Vulkan 4, full-VRAM GL at a 16384 limit 16.
    int v8k = 0;
    psx_ir_parse("8K", &v8k);
    expect("8k sw", psx_resolve_internal_scale(v8k, 240, 0, 4), 4);
    expect("8k gl16", psx_resolve_internal_scale(v8k, 240, 0, 16), 16);
    expect("8k lines", v8k, 4320);

    // Match display: the monitor's pixel height, 1 until it is known.
    int vd = 0;
    expect("display parse", psx_ir_parse("display", &vd), 1);
    expect("display value", vd, PSX_IR_DISPLAY);
    expect("display unknown", psx_resolve_internal_scale(vd, 240, 0, 32), 1);
    expect("display 1080", psx_resolve_internal_scale(vd, 240, 1080, 32), 5);
    expect("display 1964 (14in MBP)", psx_resolve_internal_scale(vd, 240, 1964, 32), 9);
    expect("display 2160", psx_resolve_internal_scale(vd, 240, 2160, 32), 9);
    expect("display 2880 (5K)", psx_resolve_internal_scale(vd, 240, 2880, 32), 12);
    expect("display 4320 (8K)", psx_resolve_internal_scale(vd, 240, 4320, 32), 18);
    expect("display id", std::strcmp(psx_ir_id_for(vd), "display"), 0);

    // Auto: Match display with dynamic resolution on. Every scale helper
    // resolves it like Match display; a floor is never Auto.
    int va = 0;
    expect("auto parse", psx_ir_parse("Auto", &va), 1);
    expect("auto value", va, PSX_IR_AUTO);
    expect("auto valid", psx_ir_value_valid(va), 1);
    expect("auto is auto", psx_ir_is_auto(va), 1);
    expect("display is not auto", psx_ir_is_auto(PSX_IR_DISPLAY), 0);
    expect("auto ceiling", psx_ir_ceiling(va), PSX_IR_DISPLAY);
    expect("fixed ceiling", psx_ir_ceiling(1080), 1080);
    expect("auto id", std::strcmp(psx_ir_id_for(va), "auto"), 0);
    expect("auto label", std::strcmp(psx_ir_label_for(va), "Auto"), 0);
    expect("auto unknown display", psx_resolve_internal_scale(va, 240, 0, 32), 1);
    expect("auto 1080", psx_resolve_internal_scale(va, 240, 1080, 32), 5);
    expect("auto 2160", psx_resolve_internal_scale(va, 240, 2160, 32), 9);
    expect("auto ss 2.0", psx_resolve_internal_scale_ss(va, 240, 1080, 32, 2000), 9);
    expect("auto capped like display", psx_resolve_internal_scale_ss_cap(
        va, 240, 8640, 32, 4000, 4320), 18);
    expect("auto floor display", psx_dynres_floor_scale(PSX_IR_DISPLAY, va, 240, 1080, 9), 5);
    expect("auto floor 720p", psx_dynres_floor_scale(720, va, 240, 1080, 9), 3);

    // Integer line counts and rejects.
    int v = 0;
    expect("lines parse", psx_ir_parse("1600", &v), 1);
    expect("lines value", v, 1600);
    expect("lines scale", psx_resolve_internal_scale(v, 240, 0, 32), 7);
    expect("custom has no id", psx_ir_id_for(1600) == nullptr, 1);
    v = 77;
    expect("reject junk", psx_ir_parse("4kk", &v), 0);
    expect("reject empty", psx_ir_parse("", &v), 0);
    expect("reject 1", psx_ir_parse("1", &v), 0);
    expect("reject 0x", psx_ir_parse("0x", &v), 0);
    expect("reject 33x", psx_ir_parse("33x", &v), 0);
    expect("reject 999x", psx_ir_parse("999x", &v), 0);
    expect("reject huge", psx_ir_parse("99999", &v), 0);
    expect("untouched on reject", v, 77);
    expect("unset scale", psx_resolve_internal_scale(PSX_IR_UNSET, 240, 0, 32), 1);
    expect("bad reference defaults to 240", psx_resolve_internal_scale(2160, 0, 0, 32), 9);

    // Supersample: a factor on the target, not on native or the legacy factor.
    expect("ss parse 1.5", psx_ss_parse_milli("1.5"), 1500);
    expect("ss parse 2x", psx_ss_parse_milli("2x"), 2000);
    expect("ss parse 4", psx_ss_parse_milli("4"), 4000);
    expect("ss reject 0.5", psx_ss_parse_milli("0.5"), 0);
    expect("ss reject 5", psx_ss_parse_milli("5"), 0);
    // Dynamic resolution floor "display": the output's lines, no supersample.
    expect("floor display 1080 MD", psx_dynres_floor_scale(PSX_IR_DISPLAY, PSX_IR_DISPLAY, 240, 1080, 9), 5);
    expect("floor display 1440 MD", psx_dynres_floor_scale(PSX_IR_DISPLAY, PSX_IR_DISPLAY, 240, 1440, 12), 6);
    expect("floor display preset", psx_dynres_floor_scale(PSX_IR_DISPLAY, 1080, 240, 2160, 9), 5);
    expect("floor display unset", psx_dynres_floor_scale(PSX_IR_DISPLAY, PSX_IR_UNSET, 240, 1080, 9), 5);
    expect("floor display clamp", psx_dynres_floor_scale(PSX_IR_DISPLAY, PSX_IR_DISPLAY, 240, 2160, 4), 4);
    expect("floor 720p", psx_dynres_floor_scale(720, PSX_IR_DISPLAY, 240, 1080, 9), 3);
    expect("ss reject junk", psx_ss_parse_milli("2y"), 0);
    expect("ss 1.0 is the plain resolve",
           psx_resolve_internal_scale_ss(PSX_IR_DISPLAY, 240, 1080, 32, 1000), 5);
    expect("ss 2.0 match display 1080", psx_resolve_internal_scale_ss(PSX_IR_DISPLAY, 240, 1080, 32, 2000), 9);
    expect("ss 1.5 at 1440p", psx_resolve_internal_scale_ss(1440, 240, 0, 32, 1500), 9);
    expect("ss leaves native", psx_resolve_internal_scale_ss(PSX_IR_NATIVE, 240, 1080, 32, 4000), 1);
    expect("ss clamps to the ceiling", psx_resolve_internal_scale_ss(4320, 240, 0, 32, 4000), 32);
    expect("ss unknown display stays 1", psx_resolve_internal_scale_ss(PSX_IR_DISPLAY, 240, 0, 32, 2000), 1);

    expect("ss policy opt-out", psx_ir_display_supersample(1500, 4320, 0), 1500);
    expect("ss below threshold", psx_ir_display_supersample(1500, 2159, 2160), 1500);
    expect("ss at threshold", psx_ir_display_supersample(1500, 2160, 2160), 1000);
    expect("ss on 8K", psx_ir_display_supersample(1500, 4320, 2160), 1000);
    expect("ss unknown display", psx_ir_display_supersample(1500, 0, 2160), 1500);
    expect("cap includes SS below threshold", psx_resolve_internal_scale_ss_cap(
        PSX_IR_DISPLAY, 240, 1440, 32, 4000, 4320), 18);
    expect("cap below 8K", psx_resolve_internal_scale_ss_cap(
        PSX_IR_DISPLAY, 240, 1080, 32, 1500, 4320), 7);
    expect("8K capped after SS", psx_resolve_internal_scale_ss_cap(
        PSX_IR_DISPLAY, 240, 8640, 32, 4000, 4320), 18);
    expect("fixed preset keeps own ceiling", psx_resolve_internal_scale_ss_cap(
        4320, 240, 8640, 32, 4000, 4320), 32);
    expect("cap opt-out", psx_resolve_internal_scale_ss_cap(
        PSX_IR_DISPLAY, 240, 4320, 32, 1500, 0), 27);

    if (g_failures) return 1;
    std::printf("internal_resolution: all checks passed\n");
    return 0;
}
