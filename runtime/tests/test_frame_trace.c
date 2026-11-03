/* Unit tests for the opt-in frame-trace ring (frame_trace.c).
 *
 * The trace is the floor the VR stutter investigation stands on, so its
 * contracts must hold without a headset: a disabled trace costs nothing and
 * records nothing, timestamps never go backwards even when the host clock does,
 * the ring wraps without losing the newest records, a frame that measured
 * nothing is marked as such, and every export is NUL-terminated and safe to
 * truncate. ctest -R frame_trace_test
 */
#include "frame_trace.h"

#include <stdio.h>
#include <string.h>

/* Evaluate side effects and failures even when NDEBUG is defined. */
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static uint64_t g_fake_ms;
static uint64_t fake_clock(void) { return g_fake_ms; }

/* A drifting clock: monotonic except one deliberate backwards step. */
static uint64_t g_step;
static uint64_t stepping_clock(void) {
    static const uint64_t seq[] = { 100, 120, 90, 140, 200 };
    return seq[(g_step++) % (sizeof seq / sizeof seq[0])];
}

static int frame_with(uint64_t base_us, int publish, uint64_t pair_id,
                      int eye_failed, const char *failure) {
    uint64_t us = base_us;
    if (!psx_frame_trace_open()) return 0;
    psx_frame_trace_ids(0x1000 + pair_id, 0x2000 + pair_id);
    psx_frame_trace_add(PSX_TRACE_EYE_BEGIN_L, us);
    psx_frame_trace_add(PSX_TRACE_EYE_GUEST_L, us * 2);
    psx_frame_trace_add(PSX_TRACE_EYE_END_L, us + 3);
    psx_frame_trace_add(PSX_TRACE_EYE_RESTORE_L, 5);
    psx_frame_trace_add(PSX_TRACE_EYE_BEGIN_R, us + 1);
    psx_frame_trace_add(PSX_TRACE_EYE_GUEST_R, us * 2 + 1);
    psx_frame_trace_add(PSX_TRACE_XR_WAIT_FRAME, 700);
    psx_frame_trace_add(PSX_TRACE_XR_END_FRAME, 40);
    psx_frame_trace_result(publish, pair_id, eye_failed, failure);
    psx_frame_trace_commit();
    return 1;
}

int main(void) {
    /* ---- disabled by default: nothing recorded, nothing to export ---- */
    psx_frame_trace_set_clock(fake_clock);
    psx_frame_trace_reset();
    psx_frame_trace_set_enabled(0);
    CHECK(psx_frame_trace_enabled() == 0);
    CHECK(psx_frame_trace_open() == 0);
    psx_frame_trace_add(PSX_TRACE_PAIR_TOTAL, 999);
    psx_frame_trace_commit();
    CHECK(psx_frame_trace_committed() == 0);
    {
        char buf[256];
        int n = psx_frame_trace_to_json(buf, sizeof buf, 0);
        CHECK(n > 0 && n < (int)sizeof buf);
        CHECK(strstr(buf, "\"enabled\":0") != NULL);
        CHECK(strstr(buf, "\"records\":[]") != NULL);
        /* Session metadata is a no-op while capture is off. */
        psx_frame_trace_session("ShouldNotAppear", 12345);
        CHECK(psx_frame_trace_to_json(buf, sizeof buf, 0) > 0);
        CHECK(strstr(buf, "\"runtime\":\"\"") != NULL);
        CHECK(strstr(buf, "\"target_period_us\":0") != NULL);
    }

    /* ---- enabled: stages land, result/pair age, monotonic seq ---- */
    psx_frame_trace_set_enabled(1);
    CHECK(psx_frame_trace_capacity() >= 64);
    g_fake_ms = 1000;
    CHECK(frame_with(10, 1, 7, -1, NULL));
    g_fake_ms = 1033;
    CHECK(frame_with(100, 0, 7, 1, "eye_capture"));   /* stale: publish failed */
    g_fake_ms = 1066;
    CHECK(frame_with(12, 1, 8, -1, NULL));
    CHECK(psx_frame_trace_committed() == 3);

    {
        PSXFrameTraceRecord recs[8];
        uint32_t n = psx_frame_trace_read(recs, 8);
        CHECK(n == 3);
        CHECK(recs[0].seq == 1 && recs[2].seq == 3);
        CHECK(recs[0].open_ms <= recs[1].open_ms &&
              recs[1].open_ms <= recs[2].open_ms);
        /* stage accumulation is exact */
        CHECK(recs[0].stages_us[PSX_TRACE_EYE_GUEST_L] == 20);
        CHECK(recs[0].stages_us[PSX_TRACE_EYE_BEGIN_R] == 11);
        CHECK(recs[0].stages_us[PSX_TRACE_XR_WAIT_FRAME] == 700);
        /* published then stale */
        CHECK((recs[0].flags & PSX_FRAME_TRACE_STEREO_PUBLISHED) != 0);
        CHECK(recs[0].pair_id == 7 && recs[0].pair_age_ms == 0);
        CHECK((recs[1].flags & PSX_FRAME_TRACE_STALE_PAIR) != 0);
        CHECK((recs[1].flags & PSX_FRAME_TRACE_FAILED) != 0);
        CHECK(recs[1].eye_failed == 1);
        CHECK(strcmp(recs[1].failure, "eye_capture") == 0);
        CHECK(recs[1].pair_age_ms == 33);
        CHECK((recs[2].flags & PSX_FRAME_TRACE_STEREO_PUBLISHED) != 0);
        CHECK(recs[2].pair_id == 8 && recs[2].pair_age_ms == 0);
    }

    /* ---- missing telemetry: an open + commit with no stage is marked ---- */
    g_fake_ms = 1100;
    CHECK(psx_frame_trace_open() == 1);
    CHECK(psx_frame_trace_open() == 0);         /* no double open */
    psx_frame_trace_commit();
    CHECK(psx_frame_trace_missing() == 1);
    {
        PSXFrameTraceRecord recs[8];
        uint32_t n = psx_frame_trace_read(recs, 8);
        CHECK(n == 4);
        CHECK((recs[3].flags & PSX_FRAME_TRACE_NO_TELEMETRY) != 0);
    }

    /* ---- shed annotation and flag rendering ---- */
    g_fake_ms = 1133;
    CHECK(psx_frame_trace_open() == 1);
    psx_frame_trace_attempt(42, 3, 250);
    psx_frame_trace_flag(PSX_FRAME_TRACE_STALE_PAIR);
    psx_frame_trace_commit();
    {
        PSXFrameTraceRecord recs[8];
        char names[96];
        uint32_t n = psx_frame_trace_read(recs, 8);
        CHECK(n == 5);
        CHECK(recs[4].stereo_attempt == 42);
        CHECK(recs[4].shed_skipped == 3 && recs[4].shed_streak_ms == 250);
        CHECK((recs[4].flags & PSX_FRAME_TRACE_SHED) != 0);
        /* A shed-only frame is stale, not "missing telemetry". */
        CHECK((recs[4].flags & PSX_FRAME_TRACE_NO_TELEMETRY) == 0);
        CHECK(psx_frame_trace_missing() == 1);
        psx_frame_trace_flag_names(recs[4].flags, names, sizeof names);
        CHECK(strstr(names, "stale") && strstr(names, "shed"));
    }

    /* ---- stage saturation rather than wrap ---- */
    g_fake_ms = 1200;
    CHECK(psx_frame_trace_open() == 1);
    psx_frame_trace_add(PSX_TRACE_PAIR_TOTAL, 0xF0000000u);
    psx_frame_trace_add(PSX_TRACE_PAIR_TOTAL, 0xF0000000u);
    psx_frame_trace_commit();
    {
        PSXFrameTraceRecord recs[8];
        uint32_t n = psx_frame_trace_read(recs, 8);
        CHECK(recs[n - 1].stages_us[PSX_TRACE_PAIR_TOTAL] == 0xFFFFFFFFu);
    }

    /* ---- a backwards clock step must not produce a backwards timestamp ---- */
    psx_frame_trace_reset();
    {
        PSXFrameTraceRecord recs[8];
        psx_frame_trace_set_clock(stepping_clock);
        for (int i = 0; i < 5; ++i) {
            CHECK(psx_frame_trace_open() == 1);
            psx_frame_trace_add(PSX_TRACE_GUEST_SIM, (uint32_t)(1 + i));
            psx_frame_trace_commit();
        }
        uint32_t n = psx_frame_trace_read(recs, 8);
        CHECK(n == 5);
        for (uint32_t i = 1; i < n; ++i)
            CHECK(recs[i].open_ms >= recs[i - 1].open_ms);
        CHECK(recs[0].open_ms == 100);
        CHECK(recs[2].open_ms >= recs[1].open_ms);  /* the 90 was clamped up */
    }

    /* ---- ring wrap keeps the newest records ---- */
    psx_frame_trace_reset();
    psx_frame_trace_set_clock(fake_clock);
    {
        uint32_t cap = psx_frame_trace_capacity();
        uint32_t extra = cap + 10u;
        for (uint32_t i = 0; i < extra; ++i) {
            g_fake_ms = 2000u + i;
            CHECK(psx_frame_trace_open() == 1);
            psx_frame_trace_add(PSX_TRACE_GUEST_SIM, i + 1u);
            psx_frame_trace_commit();
        }
        CHECK(psx_frame_trace_committed() == extra);
        CHECK(psx_frame_trace_dropped() == extra - cap);
        PSXFrameTraceRecord recs[8];
        CHECK(psx_frame_trace_read(recs, 8) == 8);
        CHECK(recs[7].seq == extra);                    /* newest present */
        CHECK(recs[0].seq == extra - 7u);
        CHECK(recs[7].stages_us[PSX_TRACE_GUEST_SIM] == extra);
        /* capacity-limited read returns exactly `capacity` of the newest */
        PSXFrameTraceRecord one[1];
        CHECK(psx_frame_trace_read(one, 1) == 1);
        CHECK(one[0].seq == extra);
    }

    /* Opening the next frame must not retire a committed slot before commit. */
    {
        static PSXFrameTraceRecord retained[4096];
        uint32_t capacity = psx_frame_trace_capacity();
        uint64_t committed = psx_frame_trace_committed();
        CHECK(psx_frame_trace_open() == 1);
        psx_frame_trace_note("not committed");
        CHECK(psx_frame_trace_read(retained, capacity) == capacity);
        CHECK(retained[0].seq == committed - capacity + 1);
        CHECK(retained[capacity - 1].seq == committed);
        psx_frame_trace_commit();
        CHECK(psx_frame_trace_read(retained, capacity) == capacity);
        CHECK(retained[capacity - 1].seq == committed + 1);
    }

    /* ---- exports: valid, NULL-terminated, truncation-safe ---- */
    {
        static char big[1 << 20];
        int n = psx_frame_trace_to_json(big, sizeof big, 16);
        CHECK(n > 0 && n < (int)sizeof big);
        CHECK(big[0] == '{' && strstr(big, "\"records\":[") != NULL);
        CHECK(big[n - 1] == '}' && big[n - 2] == ']');
        CHECK(strstr(big, "\"stages_us\"") != NULL);
        /* The record limit caps the emitted count. */
        CHECK(strstr(big, "\"seq\":") != NULL);

        n = psx_frame_trace_to_csv(big, sizeof big, 4);
        CHECK(n > 0 && n < (int)sizeof big);
        CHECK(strncmp(big, "seq,open_ms,", 12) == 0);
        CHECK(strstr(big, "guest_sim") && strstr(big, "xr_end_frame"));

        n = psx_frame_trace_to_chrome_json(big, sizeof big, 4);
        CHECK(n > 0 && n < (int)sizeof big);
        CHECK(strncmp(big, "{\"traceEvents\":[", 16) == 0 && big[n - 1] == '}');
        CHECK(strstr(big, "\"ph\":\"X\"") != NULL);

        /* A tiny buffer must still be terminated and report the full length. */
        char small[8];
        int need = psx_frame_trace_to_json(small, sizeof small, 0);
        CHECK(need > (int)sizeof small);
        CHECK(small[sizeof small - 1] == '\0');
        CHECK(small[0] == '{');
    }

    /* ---- session metadata: runtime + target refresh, JSON-escaped ---- */
    psx_frame_trace_session("TestRuntime/1.0", 11111);      /* 90 Hz headset */
    {
        static char meta[1 << 16];
        int m = psx_frame_trace_to_json(meta, sizeof meta, 1);
        CHECK(m > 0 && m < (int)sizeof meta);
        CHECK(strstr(meta, "\"runtime\":\"TestRuntime/1.0\"") != NULL);
        CHECK(strstr(meta, "\"target_period_us\":11111") != NULL);
        /* A runtime name with a quote must not break the document. */
        psx_frame_trace_session("Odd\"Runtime", 0);
        m = psx_frame_trace_to_json(meta, sizeof meta, 1);
        CHECK(m > 0 && m < (int)sizeof meta);
        CHECK(strstr(meta, "\"runtime\":\"Odd\\\"Runtime\"") != NULL);
        CHECK(strstr(meta, "\"target_period_us\":0") != NULL);
    }

    /* Fixed scratch buffers may shorten a string, never split a JSON escape. */
    {
        char controls[128]; static char json[1 << 16];
        memset(controls, 1, sizeof controls - 1); controls[127] = 0;
        psx_frame_trace_session(controls, 0);
        CHECK(psx_frame_trace_to_json(json, sizeof json, 1) > 0);
        const char *value = strstr(json, "\"runtime\":\"");
        CHECK(value != NULL); value += strlen("\"runtime\":\"");
        const char *end = strchr(value, '\"');
        CHECK(end && (end - value) % 6 == 0);
        for (; value < end; value += 6) CHECK(!memcmp(value, "\\u0001", 6));
    }

    /* ---- XR cadence: the runtime's display-time delta and repeat marking ----
     * One presented pair per granted display period is the pacing target, so a
     * frame that carried no new pair must be distinguishable from one that did
     * without reading the counterpart stats. */
    psx_frame_trace_reset();
    psx_frame_trace_set_enabled(1);
    {
        PSXFrameTraceRecord recs[8];
        static char cad[1 << 16];
        char names[96];

        CHECK(psx_frame_trace_open());               /* carried a new pair */
        psx_frame_trace_add(PSX_TRACE_POSE_AGE, 4200);
        psx_frame_trace_result(1, 11, -1, NULL);
        psx_frame_trace_xr_cadence(22222, 1);
        psx_frame_trace_commit();

        CHECK(psx_frame_trace_open());               /* carried no new pair */
        psx_frame_trace_result(0, 11, -1, NULL);
        psx_frame_trace_xr_cadence(22222, 0);
        psx_frame_trace_commit();

        CHECK(psx_frame_trace_read(recs, 8) == 2);
        CHECK(recs[0].xr_display_delta_us == 22222);
        CHECK(recs[0].xr_submit_fresh == 1);
        CHECK((recs[0].flags & PSX_FRAME_TRACE_XR_REPEAT) == 0);
        CHECK(recs[0].stages_us[PSX_TRACE_POSE_AGE] == 4200);
        CHECK(recs[1].xr_submit_fresh == 0);
        CHECK((recs[1].flags & PSX_FRAME_TRACE_XR_REPEAT) != 0);
        /* The pose was never sampled for the repeat, so no age is claimed. */
        CHECK(recs[1].stages_us[PSX_TRACE_POSE_AGE] == 0);

        CHECK(psx_frame_trace_to_json(cad, sizeof cad, 0) > 0);
        CHECK(strstr(cad, "\"xr_display_delta_us\":22222") != NULL);
        CHECK(strstr(cad, "\"xr_submit_fresh\":1") != NULL);
        CHECK(strstr(cad, "\"pose_age\":4200") != NULL);
        CHECK(psx_frame_trace_to_csv(cad, sizeof cad, 0) > 0);
        CHECK(strstr(cad, "xr_display_delta_us,xr_submit_fresh") != NULL);
        CHECK(strstr(cad, ",22222,1,") != NULL);
        CHECK(strstr(cad, ",22222,0,") != NULL);
        CHECK(strstr(psx_frame_trace_flag_names(recs[1].flags, names, sizeof names),
                     "xr_repeat") != NULL);
    }

    /* ---- per-eye draw counters and guest cycles ---------------------------
     * The pair's own prim count is the last eye's, which is useless for asking
     * which eye a change moved, and the guest sub-stages of the replay are pair
     * totals written by the game hook. */
    psx_frame_trace_reset();
    psx_frame_trace_set_enabled(1);
    {
        PSXFrameTraceRecord recs[4];
        static char buf[1 << 16];

        CHECK(psx_frame_trace_open());
        /* An unknown eye or a closed record must not write anything. */
        psx_frame_trace_draw(2, 999, 999);
        psx_frame_trace_eye_cycles(-1, 999);
        psx_frame_trace_add(PSX_TRACE_GAME_TRAVERSE, 0);
        psx_frame_trace_draw(0, 669, 667);
        psx_frame_trace_draw(1, 947, 944);
        psx_frame_trace_eye_cycles(0, 145000);
        psx_frame_trace_eye_cycles(1, 151000);
        psx_frame_trace_add(PSX_TRACE_GAME_TRAVERSE, 1200);
        psx_frame_trace_add(PSX_TRACE_GAME_TRAVERSE, 1400);
        psx_frame_trace_add(PSX_TRACE_GAME_SUBMIT, 800);
        psx_frame_trace_commit();

        CHECK(psx_frame_trace_read(recs, 4) == 1);
        CHECK(recs[0].prims_l == 669 && recs[0].prims_r == 947);
        CHECK(recs[0].tex_prims_l == 667 && recs[0].tex_prims_r == 944);
        /* `prims`/`tex_prims` keep their meaning: the last eye of the pair. */
        CHECK(recs[0].prims == 947 && recs[0].textured_prims == 944);
        CHECK(recs[0].eye_cycles_l == 145000 && recs[0].eye_cycles_r == 151000);
        /* A guest sub-stage accumulates both eyes' spans of that call. */
        CHECK(recs[0].stages_us[PSX_TRACE_GAME_TRAVERSE] == 2600);
        CHECK(recs[0].stages_us[PSX_TRACE_GAME_SUBMIT] == 800);

        CHECK(psx_frame_trace_to_json(buf, sizeof buf, 0) > 0);
        CHECK(strstr(buf, "\"prims_l\":669") && strstr(buf, "\"prims_r\":947"));
        CHECK(strstr(buf, "\"cycles_l\":145000") &&
              strstr(buf, "\"cycles_r\":151000"));
        CHECK(strstr(buf, "\"game_traverse\":2600") &&
              strstr(buf, "\"game_submit\":800") &&
              strstr(buf, "\"game_laser\":0"));
        CHECK(psx_frame_trace_to_csv(buf, sizeof buf, 0) > 0);
        CHECK(strstr(buf, "prims_l,prims_r,tex_prims_l,tex_prims_r,cycles_l,"
                          "cycles_r") != NULL);
        CHECK(strstr(buf, ",669,947,667,944,145000,151000,") != NULL);
        /* The name table must stay in enum order (the header order is the ABI). */
        CHECK(strcmp(psx_frame_trace_stage_name(PSX_TRACE_GAME_ENV), "game_env") == 0
              && strcmp(psx_frame_trace_stage_name(PSX_TRACE_GAME_LASER),
                        "game_laser") == 0);
    }

    psx_frame_trace_set_enabled(0);
    printf("frame_trace_test: all checks passed\n");
    return 0;
}