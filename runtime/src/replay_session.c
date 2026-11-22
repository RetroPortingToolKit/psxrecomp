/* Player replay playback. See replay_session.h. */
#include "replay_session.h"
#include "replay_identity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_BYTES 1024u
/* A stick counts as input past this distance from centre (0x80). */
#define TAKEOVER_DEADZONE 0x30

static ReplayState s_state = REPLAY_IDLE;
static ReplayResult s_result = REPLAY_RESULT_NONE;

static ReplayStep *s_steps;
static uint32_t s_play_steps, s_play_frames, s_play_step, s_play_left, s_play_frame;
static ReplayCheckpoint s_end;
static int s_takeover_armed;
static int s_settings_switched;
static uint32_t (*s_digests)[5];
static uint32_t s_digest_count, s_digest_next;
static unsigned s_digests_checked, s_div_parts, s_fail_parts;
static uint32_t s_div_frame, s_fail_frame;
static int s_diverged;
static int s_power_on, s_cards_installed;

/* What the verdict file reports. */
static char s_verdict_path[PATH_BYTES];
static char s_play_path[PATH_BYTES];
static char s_rec_build[REPLAY_FILE_TEXT], s_player_build[REPLAY_FILE_TEXT];
static char s_rec_exe[80], s_player_exe[80];
static char s_rec_platform[32], s_player_platform[32];
static char s_rec_codegen[16], s_player_codegen[16];
static int s_cross_platform;
static uint64_t s_end_cycle, s_end_recorded_cycle;
static unsigned s_end_pages;
static int s_end_reached;

ReplayState replay_session_state(void) { return s_state; }
ReplayResult replay_session_last_result(void) { return s_result; }
int replay_session_owns_p1(void) { return s_state == REPLAY_PLAYING; }
unsigned replay_session_digests_checked(void) { return s_digests_checked; }

int replay_session_first_divergence(uint32_t *frame, unsigned *parts)
{
    if (!s_diverged) return 0;
    if (frame) *frame = s_div_frame;
    if (parts) *parts = s_div_parts;
    return 1;
}

void replay_session_set_verdict_path(const char *path)
{
    snprintf(s_verdict_path, sizeof s_verdict_path, "%s", path ? path : "");
}

static const char *digest_parts_text(unsigned parts, char *out, size_t cap)
{
    static const char *const names[4] = {"core", "av", "aux", "ext"};
    size_t n = 0;
    out[0] = 0;
    for (unsigned k = 0; k < 4; ++k)
        if (parts & (1u << k)) n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "%s%s", n ? "+" : "", names[k]);
    if (!out[0]) snprintf(out, cap, "none");
    return out;
}

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; ++s) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void write_verdict(ReplayResult result, const char *reason)
{
    static const char *const names[] = {"incomplete", "in_sync", "diverged", "stopped_by_input", "failed"};
    char parts[32];
    FILE *f;
    if (!s_verdict_path[0] || !(f = fopen(s_verdict_path, "wb"))) return;
    fprintf(f, "{\n  \"schema\": \"psxrecomp-replay-verdict/1\",\n  \"result\": \"%s\",\n",
            names[result <= REPLAY_RESULT_FAILED ? result : 0]);
    fprintf(f, "  \"frames_played\": %u,\n  \"frames_total\": %u,\n", (unsigned)s_play_frame,
            (unsigned)s_play_frames);
    if (s_diverged)
        fprintf(f, "  \"first_divergence_frame\": %u,\n  \"divergence_parts\": \"%s\",\n", (unsigned)s_div_frame,
                digest_parts_text(s_div_parts, parts, sizeof parts));
    else
        fprintf(f, "  \"first_divergence_frame\": null,\n  \"divergence_parts\": null,\n");
    /* The first divergence that decides the result: not AV alone. */
    if (s_fail_parts)
        fprintf(f, "  \"first_failing_frame\": %u,\n  \"failing_parts\": \"%s\",\n", (unsigned)s_fail_frame,
                digest_parts_text(s_fail_parts, parts, sizeof parts));
    else
        fprintf(f, "  \"first_failing_frame\": null,\n  \"failing_parts\": null,\n");
    fprintf(f, "  \"digests_checked\": %u,\n", s_digests_checked);
    if (s_end_reached)
        fprintf(f, "  \"end_cycle\": %llu,\n  \"recorded_end_cycle\": %llu,\n  \"differing_ram_pages\": %u,\n",
                (unsigned long long)s_end_cycle, (unsigned long long)s_end_recorded_cycle, s_end_pages);
    fputs("  \"recorded_build\": ", f); json_string(f, s_rec_build);
    fputs(",\n  \"player_build\": ", f); json_string(f, s_player_build);
    fputs(",\n  \"recorded_exe_sha256\": ", f); json_string(f, s_rec_exe);
    fputs(",\n  \"player_exe_sha256\": ", f); json_string(f, s_player_exe);
    fputs(",\n  \"recorded_platform\": ", f); json_string(f, s_rec_platform);
    fputs(",\n  \"player_platform\": ", f); json_string(f, s_player_platform);
    fputs(",\n  \"recorded_codegen\": ", f); json_string(f, s_rec_codegen);
    fputs(",\n  \"player_codegen\": ", f); json_string(f, s_player_codegen);
    fprintf(f, ",\n  \"cross_platform\": %s", s_cross_platform ? "true" : "false");
    fprintf(f, ",\n  \"power_on\": %s", s_power_on ? "true" : "false");
    fputs(",\n  \"replay\": ", f); json_string(f, s_play_path);
    fputs(",\n  \"reason\": ", f); json_string(f, reason ? reason : "");
    fputs("\n}\n", f);
    fclose(f);
}

static void end_playback(ReplayResult result, const char *osd)
{
    write_verdict(result, osd);
    if (s_settings_switched) replay_host_settings_restore();
    s_settings_switched = 0;
    if (s_cards_installed) replay_host_cards_restore();
    s_cards_installed = 0;
    free(s_steps);
    s_steps = NULL;
    free(s_digests);
    s_digests = NULL;
    s_digest_count = s_digest_next = 0;
    s_state = REPLAY_IDLE;
    s_result = result;
    if (osd) replay_host_osd(osd, 2400);
}

static int refuse_play(const char *why)
{
    char msg[320];
    snprintf(msg, sizeof msg, "Replay not played: %s", why);
    fprintf(stderr, "replay: %s\n", msg);
    replay_host_osd(msg, 2600);
    s_result = REPLAY_RESULT_FAILED;
    write_verdict(REPLAY_RESULT_FAILED, msg);
    return 0;
}

/* Reads `n` bytes at `offset` into a new buffer. */
static void *read_at(FILE *f, long offset, size_t n)
{
    void *p = malloc(n ? n : 1);
    if (p && (fseek(f, offset, SEEK_SET) || fread(p, 1, n, f) != n)) {
        free(p);
        p = NULL;
    }
    return p;
}

int replay_session_play_file(const char *path)
{
    ReplayFileInfo *info = NULL;
    ReplayStep *steps = NULL;
    ReplayIdentity player;
    ReplayIdentityMatch match = REPLAY_IDENTITY_REFUSE;
    uint8_t *anchor = NULL, *cards = NULL;
    unsigned char *dig = NULL;
    uint32_t (*digests)[5] = NULL;
    const char *error = NULL;
    char why[256] = "", product[REPLAY_FILE_PRODUCT_MAX + 1] = "", differs[512] = "";
    FILE *f;
    if (s_state != REPLAY_IDLE) return refuse_play("a replay is already playing");
    snprintf(s_play_path, sizeof s_play_path, "%s", path ? path : "");
    s_rec_build[0] = s_player_build[0] = s_rec_exe[0] = s_player_exe[0] = 0;
    s_rec_platform[0] = s_player_platform[0] = s_rec_codegen[0] = s_player_codegen[0] = 0;
    s_cross_platform = s_power_on = s_diverged = s_end_reached = 0;
    s_play_frame = s_play_frames = 0;
    s_digests_checked = s_div_parts = s_fail_parts = 0;
    s_fail_frame = 0;
    if (!path || !(f = fopen(path, "rb"))) return refuse_play("cannot open the file");
    info = (ReplayFileInfo *)malloc(sizeof *info);
    steps = (ReplayStep *)calloc(REPLAY_FILE_MAX_STEPS, sizeof *steps);
    error = !info || !steps ? "out of memory" : replay_file_read(f, info, steps, REPLAY_FILE_MAX_STEPS);
    if (!error && info->power_on) {
        s_power_on = 1;
        if (!replay_host_at_power_on()) error = "it starts at power-on: play it from startup";
        else if (!(cards = (uint8_t *)calloc(2, REPLAY_FILE_CARD_BYTES))) error = "out of memory";
        else if (fseek(f, info->cards_offset, SEEK_SET)) error = "cannot read the memory cards";
        for (uint32_t c = 0; !error && c < 2; ++c)
            if ((info->cards_mask >> c & 1u) &&
                fread(cards + c * REPLAY_FILE_CARD_BYTES, 1, REPLAY_FILE_CARD_BYTES, f) != REPLAY_FILE_CARD_BYTES)
                error = "cannot read the memory cards";
    } else if (!error && !(anchor = (uint8_t *)read_at(f, info->anchor_offset, info->anchor_length))) {
        error = "cannot read the anchor";
    }
    if (!error && info->digest_count) {
        const size_t n = (size_t)info->digest_count * REPLAY_FILE_DIGEST_BYTES;
        digests = (uint32_t(*)[5])malloc(info->digest_count * sizeof *digests);
        if (!digests || !(dig = (unsigned char *)read_at(f, info->digest_offset, n)))
            error = "cannot read the state digests";
        for (uint32_t i = 0; !error && i < info->digest_count; ++i)
            for (uint32_t k = 0; k < 5; ++k)
                digests[i][k] = replay_file_le32(dig + i * REPLAY_FILE_DIGEST_BYTES + 4 * k);
        for (uint32_t i = 0; !error && i < info->digest_count; ++i)
            if (digests[i][0] > info->frames || (i && digests[i][0] <= digests[i - 1][0]))
                error = "state digest order";
    }
    free(dig);
    fclose(f);
    if (!error && !replay_host_identity(&player, why, sizeof why)) error = why;
    if (!error) {
        replay_host_product(product, sizeof product);
        match = replay_identity_check(&info->id, info->product, &player, product, why, sizeof why);
        if (match == REPLAY_IDENTITY_REFUSE) error = why;
    }
    if (error) {
        char e[256];
        snprintf(e, sizeof e, "%s", error);
        free(info); free(steps); free(anchor); free(cards); free(digests);
        return refuse_play(e);
    }
    snprintf(s_rec_build, sizeof s_rec_build, "%s", info->id.build);
    snprintf(s_player_build, sizeof s_player_build, "%s", player.build);
    replay_identity_line_value(info->product, "exe_sha256", s_rec_exe, sizeof s_rec_exe);
    replay_identity_line_value(product, "exe_sha256", s_player_exe, sizeof s_player_exe);
    replay_identity_line_value(info->product, "platform", s_rec_platform, sizeof s_rec_platform);
    replay_identity_line_value(product, "platform", s_player_platform, sizeof s_player_platform);
    replay_identity_line_value(info->product, "codegen", s_rec_codegen, sizeof s_rec_codegen);
    replay_identity_line_value(product, "codegen", s_player_codegen, sizeof s_player_codegen);
    /* The same commit built for another platform has another exe by
     * construction: the same build, not a different one. */
    s_cross_platform = !strcmp(s_rec_build, s_player_build) && s_rec_platform[0] && s_player_platform[0] &&
                       strcmp(s_rec_platform, s_player_platform);
    replay_host_settings_apply(info->settings, differs, sizeof differs);
    s_settings_switched = 1;
    s_steps = steps;
    s_digests = digests;
    if (s_power_on) {
        s_cards_installed = replay_host_cards_install(cards, info->cards_mask);
        free(cards);
        if (!s_cards_installed) {
            free(info); free(anchor);
            end_playback(REPLAY_RESULT_FAILED, "Replay not played: its memory cards cannot be inserted");
            return 0;
        }
    } else if (!replay_host_load_anchor(anchor, info->anchor_length)) {
        free(info); free(anchor);
        end_playback(REPLAY_RESULT_FAILED, "Replay not played: the anchor state cannot be loaded");
        return 0;
    }
    free(anchor);
    s_end = info->end;
    s_play_steps = info->steps;
    s_play_frames = info->frames;
    s_play_step = 0;
    s_play_left = s_play_steps ? steps[0].frames : 0;
    s_takeover_armed = 0;
    s_digest_count = info->digest_count;
    s_digest_next = 0;
    s_result = REPLAY_RESULT_NONE;
    /* A power-on replay feeds record 1 at this boundary, as it was recorded. */
    s_state = s_power_on ? REPLAY_PLAYING : REPLAY_LOADING;
    if (s_cross_platform) {
        char msg[160];
        snprintf(msg, sizeof msg, "Replay from the same build on another platform (%s)", s_rec_platform);
        replay_host_osd(msg, 2600);
    } else if (match == REPLAY_IDENTITY_OTHER_BUILD) {
        replay_host_osd("Replay from a different build: it may go out of sync", 2600);
        fprintf(stderr, "replay: recorded on build %s (exe %s), this is %s (exe %s)\n", s_rec_build,
                s_rec_exe[0] ? s_rec_exe : "?", s_player_build, s_player_exe[0] ? s_player_exe : "?");
    } else if (differs[0]) {
        char msg[640];
        snprintf(msg, sizeof msg, "Replay may go out of sync: different %s", differs);
        replay_host_osd(msg, 2600);
    } else {
        replay_host_osd("Replay playing", 1200);
    }
    if (differs[0]) fprintf(stderr, "replay: settings this build cannot switch: %s\n", differs);
    fprintf(stdout, "replay_playing: path=%s frames=%u other_build=%d power_on=%d cross_platform=%d\n", path,
            (unsigned)info->frames, match == REPLAY_IDENTITY_OTHER_BUILD, s_power_on, s_cross_platform);
    fflush(stdout);
    free(info);
    return 1;
}

static int live_input(uint16_t buttons, const uint8_t sticks[4])
{
    if (buttons != 0xFFFFu) return 1;
    for (int i = 0; i < 4; ++i) {
        const int d = (int)sticks[i] - 0x80;
        if (d > TAKEOVER_DEADZONE || d < -TAKEOVER_DEADZONE) return 1;
    }
    return 0;
}

/* Compares the state digest recorded for this boundary, if any. */
static void check_digest(void)
{
    uint32_t d[4];
    unsigned parts = 0;
    char text[32];
    const uint32_t *want;
    if (s_digest_next >= s_digest_count || s_digests[s_digest_next][0] != s_play_frame) return;
    want = &s_digests[s_digest_next++][1];
    if (!replay_host_state_digest(d)) return;
    s_digests_checked++;
    for (unsigned k = 0; k < 4; ++k) parts |= (unsigned)(d[k] != want[k]) << k;
    if (!parts) return;
    /* AV alone is reported but does not fail a replay, so the checks go on
     * looking for the first divergence that does. */
    if ((parts & ~REPLAY_DIGEST_AV) && !s_fail_parts) {
        s_fail_frame = s_play_frame;
        s_fail_parts = parts;
        if (s_diverged) {
            fprintf(stdout, "replay_diverged: frame=%u parts=%s failing\n", (unsigned)s_play_frame,
                    digest_parts_text(parts, text, sizeof text));
            fflush(stdout);
        }
    }
    if (s_diverged) return;
    s_diverged = 1;
    s_div_frame = s_play_frame;
    s_div_parts = parts;
    fprintf(stdout, "replay_diverged: frame=%u parts=%s core=%08x/%08x av=%08x/%08x aux=%08x/%08x ext=%08x/%08x\n",
            (unsigned)s_play_frame, digest_parts_text(parts, text, sizeof text), d[0], want[0], d[1], want[1],
            d[2], want[2], d[3], want[3]);
    fflush(stdout);
}

static void finish(void)
{
    ReplayCheckpoint *now = (ReplayCheckpoint *)malloc(sizeof *now);
    unsigned differing = 0;
    int ram, cyc, state;
    char text[32];
    if (!now) {
        end_playback(REPLAY_RESULT_FAILED, "Replay stopped: out of memory");
        return;
    }
    replay_file_checkpoint(s_play_frame, replay_host_ram(), replay_host_cycle(), now);
    ram = !memcmp(now->ram_sha256, s_end.ram_sha256, 32);
    cyc = now->cycle == s_end.cycle;
    for (uint32_t p = 0; p < REPLAY_FILE_RAM_PAGES; ++p) differing += now->pages[p] != s_end.pages[p];
    /* AV alone does not fail a replay (see REPLAY_DIGEST_AV). */
    state = !s_fail_parts;
    s_end_reached = 1;
    s_end_cycle = now->cycle;
    s_end_recorded_cycle = s_end.cycle;
    s_end_pages = differing;
    free(now);
    fprintf(stdout,
            "replay_end: frames=%u result=%s cycle=%llu recorded_cycle=%llu differing_pages=%u "
            "digests_checked=%u first_divergence=%d divergence_parts=%s\n",
            (unsigned)s_play_frame, ram && cyc && state ? "in_sync" : "out_of_sync",
            (unsigned long long)s_end_cycle, (unsigned long long)s_end.cycle, differing, s_digests_checked,
            s_diverged ? (int)s_div_frame : -1, digest_parts_text(s_diverged ? s_div_parts : 0, text, sizeof text));
    fflush(stdout);
    if (ram && cyc && state) {
        end_playback(REPLAY_RESULT_IN_SYNC, "Replay finished (in sync)");
    } else {
        char msg[160];
        snprintf(msg, sizeof msg, "Replay finished (out of sync: %u RAM pages, cycle %+lld)", differing,
                 (long long)(s_end_cycle - s_end.cycle));
        end_playback(REPLAY_RESULT_OUT_OF_SYNC, msg);
    }
}

static int play_boundary(uint16_t live, const uint8_t live_sticks[4], uint16_t *out_buttons,
                         uint8_t out_sticks[4])
{
    const ReplayStep *s;
    check_digest();
    if (s_play_frame == s_play_frames) {
        finish();
        return 0;
    }
    /* Live input takes over once the pad has been neutral after the start
     * (the press that started playback does not count). */
    if (live_input(live, live_sticks)) {
        if (s_takeover_armed) {
            end_playback(REPLAY_RESULT_TAKEN_OVER, "Playback stopped: you have control");
            return 0;
        }
    } else {
        s_takeover_armed = 1;
    }
    s = &s_steps[s_play_step];
    *out_buttons = s->buttons;
    /* File order LY,LX,RY,RX; host order LX,LY,RX,RY. */
    out_sticks[0] = s->axes_ly_lx_ry_rx[1];
    out_sticks[1] = s->axes_ly_lx_ry_rx[0];
    out_sticks[2] = s->axes_ly_lx_ry_rx[3];
    out_sticks[3] = s->axes_ly_lx_ry_rx[2];
    s_play_frame++;
    if (--s_play_left == 0 && ++s_play_step < s_play_steps) s_play_left = s_steps[s_play_step].frames;
    return 1;
}

int replay_session_boundary(uint16_t live, const uint8_t live_sticks[4], uint16_t *out_buttons,
                            uint8_t out_sticks[4])
{
    switch (s_state) {
    case REPLAY_IDLE:
        return 0;
    case REPLAY_LOADING: {
        const int r = replay_host_take_load_result();
        if (r == 0) return 0;
        if (r < 0) {
            end_playback(REPLAY_RESULT_FAILED, "Replay not played: its anchor state does not load in this build");
            return 0;
        }
        s_state = REPLAY_PLAYING;
        return play_boundary(live, live_sticks, out_buttons, out_sticks);
    }
    case REPLAY_PLAYING:
        return play_boundary(live, live_sticks, out_buttons, out_sticks);
    }
    return 0;
}

void replay_session_shutdown(void)
{
    if (s_state != REPLAY_IDLE) end_playback(REPLAY_RESULT_NONE, NULL);
}
