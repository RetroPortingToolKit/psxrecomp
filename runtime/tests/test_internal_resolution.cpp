// internal_resolution.h: preset ids, the value encoding shared with the
// launcher, and the integer scale each preset asks for. Presets x reference
// 240/480 x backend maxima (the GL clamp at 16384 is 16x; 18x needs the
// windowed high-resolution surface or a 32768-limit GPU).

#include "internal_resolution.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

static void expect(const char* what, long long got, long long want) {
    if (got != want) {
        std::printf("FAIL %s: got %lld want %lld\n", what, got, want);
        ++g_failures;
    }
}

// ---- Launcher round trip: seed -> launcher -> adopt -------------------------
// A model of recomp-ui's legacy Supersampling row (before the Internal
// resolution row): launcher_model clamps the seeded value to 1..4 on load and
// each click cycles 1 -> 2 -> 3 -> 4 -> 1.
static int legacy_row(int seeded, int clicks) {
    int v = seeded < 1 ? 1 : seeded > 4 ? 4 : seeded;
    for (int i = 0; i < clicks; i++) v = (v % 4) + 1;
    return v;
}

struct Host {             // the host's video state between launcher trips
    int preset;           // g_video_internal_res
    int scale;            // g_video_scale
};

// One trip, the way main.cpp runs it at first boot and on the netplay
// soft-return. has_row = RECOMP_LAUNCHER_HAS_INTERNAL_RESOLUTION.
// Legacy: ss_clicks cycles the Supersampling row. Row: row_pick > 0 picks a
// preset in the Internal resolution row (0 leaves it).
static PsxIrAdopted trip(Host& h, int has_row, int ss_clicks, int row_pick,
                         int s_max = 32, int display_px_h = 2160) {
    const int seeded_preset = h.preset;
    const int ss_seed = psx_ir_launcher_seed_supersampling(has_row, h.preset, h.scale,
                                                           240, display_px_h);
    const int ss_result = has_row ? ss_seed : legacy_row(ss_seed, ss_clicks);
    int ir_result = PSX_IR_UNSET;
    if (has_row) {
        const int shown = h.preset != PSX_IR_UNSET
                              ? h.preset : psx_ir_from_supersampling(h.scale, 240);
        ir_result = row_pick ? row_pick : shown;
    }
    PsxIrAdopted a = psx_ir_adopt_launcher(has_row, seeded_preset, ss_seed, ss_result,
                                           ir_result, 240, display_px_h, s_max);
    h.preset = a.preset;
    h.scale = a.scale;
    return a;
}

static void launcher_round_trips() {
    // Older recomp-ui (no Internal resolution row), nothing configured: the
    // historical round trip. The player's Supersampling pick is what runs and
    // what is saved, and no internal_resolution key appears.
    {
        Host h{PSX_IR_UNSET, 1};
        PsxIrAdopted a = trip(h, 0, 2, 0);                 // 1x -> 3x
        expect("legacy pick scale", a.scale, 3);
        expect("legacy pick preset", a.preset, PSX_IR_UNSET);
        expect("legacy pick save ss", a.save_ss, 3);
        expect("legacy pick no ir key", a.save_ir, PSX_IR_UNSET);
        // Netplay soft-return: seeded with the running scale, picked again.
        a = trip(h, 0, 1, 0);                              // 3x -> 4x
        expect("legacy soft-return pick", a.scale, 4);
        expect("legacy soft-return no ir key", a.save_ir, PSX_IR_UNSET);
        a = trip(h, 0, 0, 0);                              // untouched
        expect("legacy untouched", a.scale, 4);
    }
    {
        Host h{PSX_IR_UNSET, 8};                           // hand-edited 8
        PsxIrAdopted a = trip(h, 0, 0, 0);
        expect("legacy clamps like before", a.scale, 4);   // the row returns 4
        expect("legacy clamps save", a.save_ss, 4);
    }
    // A settings.toml "native" written by a build with the row, then an older
    // launcher: the preset must not reset the player's pick (the reported bug).
    {
        Host h{PSX_IR_NATIVE, 1};
        PsxIrAdopted a = trip(h, 0, 2, 0);                 // 1x -> 3x
        expect("native preset yields to pick", a.scale, 3);
        expect("native preset dropped", a.preset, PSX_IR_UNSET);
        expect("native preset key dropped", a.save_ir, PSX_IR_UNSET);
        expect("native preset save ss", a.save_ss, 3);
    }
    // A preset the older launcher cannot show (game.toml or hand-edited 4K):
    // the row starts on the nearest it can show (4x) and an untouched row
    // keeps the preset, first boot and soft-return alike.
    {
        Host h{2160, 1};
        PsxIrAdopted a = trip(h, 0, 0, 0);
        expect("hidden preset kept", a.preset, 2160);
        expect("hidden preset scale", a.scale, 9);
        expect("hidden preset save ss", a.save_ss, 4);
        expect("hidden preset save ir", a.save_ir, 2160);
        a = trip(h, 0, 0, 0);                              // soft-return, running 9x
        expect("hidden preset soft-return kept", a.scale, 9);
        a = trip(h, 0, 1, 0);                              // 4x -> 1x
        expect("hidden preset replaced", a.scale, 1);
        expect("hidden preset replaced preset", a.preset, PSX_IR_UNSET);
        expect("hidden preset replaced key", a.save_ir, PSX_IR_UNSET);
    }
    {
        Host h{PSX_IR_DISPLAY, 1};                         // 2160-px display
        PsxIrAdopted a = trip(h, 0, 0, 0);
        expect("display kept", a.scale, 9);
        a = trip(h, 0, 2, 0);                              // 4x -> 1x -> 2x
        expect("display replaced", a.scale, 2);
    }
    // Launcher with the row: the row owns the preset, as before.
    {
        Host h{PSX_IR_UNSET, 3};
        PsxIrAdopted a = trip(h, 1, 0, 0);                 // shows 720p, untouched
        expect("row untouched preset", a.preset, 720);
        expect("row untouched scale", a.scale, 3);
        expect("row untouched save", a.save_ir, 720);
        a = trip(h, 1, 0, 4320);                           // 8K
        expect("row 8k scale", a.scale, 18);
        expect("row 8k save ss", a.save_ss, 4);
        expect("row 8k save ir", a.save_ir, 4320);
        a = trip(h, 1, 0, 4320, 4);                        // software ceiling
        expect("row 8k software", a.scale, 4);
        a = trip(h, 1, 0, PSX_IR_NATIVE);
        expect("row native", a.scale, 1);
        expect("row native save", a.save_ir, PSX_IR_NATIVE);
    }
    // Invalid row values never reach the host: the seeded preset stands.
    {
        PsxIrAdopted a = psx_ir_adopt_launcher(1, 1080, 1, 1, 0, 240, 0, 32);
        expect("row invalid keeps preset", a.preset, 1080);
        a = psx_ir_adopt_launcher(1, PSX_IR_UNSET, 2, 2, -7, 240, 0, 32);
        expect("row invalid legacy factor", a.preset, 480);
        expect("row invalid legacy scale", a.scale, 2);
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
    expect("reject huge", psx_ir_parse("99999", &v), 0);
    expect("untouched on reject", v, 77);
    expect("unset scale", psx_resolve_internal_scale(PSX_IR_UNSET, 240, 0, 32), 1);
    expect("bad reference defaults to 240", psx_resolve_internal_scale(2160, 0, 0, 32), 9);

    // Legacy supersampling shown as a preset when one resolves to it.
    expect("legacy 1", psx_ir_from_supersampling(1, 240), PSX_IR_NATIVE);
    expect("legacy 2", psx_ir_from_supersampling(2, 240), 480);
    expect("legacy 3", psx_ir_from_supersampling(3, 240), 720);
    expect("legacy 4", psx_ir_from_supersampling(4, 240), 960);
    expect("legacy 5", psx_ir_from_supersampling(5, 240), 1080);
    expect("legacy 9", psx_ir_from_supersampling(9, 240), 2160);
    expect("legacy 18", psx_ir_from_supersampling(18, 240), 4320);
    // And the legacy value resolves back to the same scale.
    for (int n = 1; n <= 32; n++)
        expect("legacy round trip",
               psx_resolve_internal_scale(psx_ir_from_supersampling(n, 240), 240, 0, 32), n);

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
    launcher_round_trips();

    if (g_failures) return 1;
    std::printf("internal_resolution: all checks passed\n");
    return 0;
}
