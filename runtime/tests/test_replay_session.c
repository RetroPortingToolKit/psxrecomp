/* Replay recording and playback against stub host hooks: the anchor load,
 * the per-vblank P1 feed, the digest and END checks, takeover, power-on with
 * cards, the identity refusals, recordings that play back in sync, and the
 * compiled code held fixed from the start of each replay to its end. */
#include "replay_session.h"
#include "replay_identity.h"
#include "psx_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

#define FRAMES 130u
#define REPLAY "replay_session_test.psxrpl"
#define VERDICT "replay_session_test.json"

/* ---- Stub host ---- */
static uint8_t s_ram[2u << 20];
static uint64_t s_cycle = 5000;
static ReplayIdentity s_player;
static const char *s_player_product = "exe_sha256=aa\nplatform=windows-x64\n";
static int s_at_power_on, s_load_result, s_loads, s_cards_in, s_cards_out, s_settings_in, s_settings_out;
static size_t s_anchor_size;
static uint8_t s_anchor_first;
static uint32_t s_cards_mask;
static uint8_t s_card2_first;
static char s_settings[256];
static uint32_t s_digest[4] = {1, 2, 3, 4};

void replay_host_osd(const char *text, int ms) { (void)text; (void)ms; }
int replay_host_identity(ReplayIdentity *id, char *why, size_t cap)
{
    (void)why; (void)cap;
    *id = s_player;
    return 1;
}
void replay_host_product(char *out, size_t cap) { snprintf(out, cap, "%s", s_player_product); }
int replay_host_at_power_on(void) { return s_at_power_on; }
int replay_host_load_anchor(const void *data, size_t size)
{
    s_loads++;
    s_anchor_size = size;
    s_anchor_first = ((const uint8_t *)data)[0];
    return 1;
}
int replay_host_take_load_result(void) { return s_load_result; }
void replay_host_settings_apply(const char *settings, char *differs, size_t cap)
{
    s_settings_in++;
    snprintf(s_settings, sizeof s_settings, "%s", settings);
    if (cap) differs[0] = 0;
}
void replay_host_settings_restore(void) { s_settings_out++; }
int replay_host_cards_install(const uint8_t *images, uint32_t mask)
{
    s_cards_in++;
    s_cards_mask = mask;
    s_card2_first = images[REPLAY_FILE_CARD_BYTES];
    return 1;
}
void replay_host_cards_restore(void) { s_cards_out++; }
/* Native code during a replay: the recording's watcher, and what a
 * playback admits and loads (s_code_trace: "<op>:<unit>;" per call, op a
 * admitted, l loaded, p present, r rescan). */
static void (*s_watch)(char op, const char *unit);
static int s_admitting, s_unit_ok = 1;
static char s_code_trace[1024];
static void trace(const char *what, const char *unit)
{
    const size_t n = strlen(s_code_trace);
    snprintf(s_code_trace + n, sizeof s_code_trace - n, "%s:%s;", what, unit);
}
void replay_host_code_watch(void (*fn)(char op, const char *unit)) { s_watch = fn; }
void replay_host_code_admit(int on) { s_admitting = on; }
int replay_host_code_admit_unit(const char *unit)
{
    CHECK(s_admitting);
    trace("a", unit);
    return 1;
}
int replay_host_code_unit(const char *unit, int load, int wait)
{
    trace(load ? "l" : wait ? "w" : "p", unit);
    /* gcc/x/late.dll is never compiled; the start units are all here. */
    return s_unit_ok && strcmp(unit, "gcc/x/late.dll") ? 1 : 0;
}
void replay_host_code_rescan(void) { trace("r", "-"); }
/* The code set the host names, unsorted; NULL names none. */
#define CODE_LINES "gcc/x/b.dll\ngcc/x/a.dll\n"
static const char *s_code_lines = CODE_LINES;
static unsigned s_code_count = 2;
char *replay_host_code_set(unsigned *count)
{
    char *p;
    if (!s_code_lines || !(p = (char *)malloc(strlen(s_code_lines) + 1))) return NULL;
    strcpy(p, s_code_lines);
    *count = s_code_count;
    return p;
}
const uint8_t *replay_host_ram(void) { return s_ram; }
uint64_t replay_host_cycle(void) { return s_cycle; }
int replay_host_state_digest(uint32_t out[4])
{
    memcpy(out, s_digest, sizeof s_digest);
    return 1;
}

/* Recording hooks. */
static int s_can_record = 1, s_anchor_requests, s_anchor_take;
int replay_host_can_record(char *why, size_t cap)
{
    if (!s_can_record) snprintf(why, cap, "netplay is active");
    return s_can_record;
}
int replay_host_request_anchor(void) { s_anchor_requests++; return 1; }
int replay_host_take_anchor(uint8_t **data, size_t *size)
{
    if (s_anchor_take > 0) {
        *data = (uint8_t *)malloc(48);
        memset(*data, 0x5A, 48);
        *size = 48;
    }
    return s_anchor_take;
}
void replay_host_settings_capture(char *out, size_t cap) { snprintf(out, cap, "cd_speed=1\n"); }
int replay_host_cards_capture(uint8_t *images, uint32_t *mask)
{
    memset(images + REPLAY_FILE_CARD_BYTES, 0x77, REPLAY_FILE_CARD_BYTES);
    *mask = 2u;
    return 1;
}

/* ---- Fixtures ---- */
static void identity(ReplayIdentity *id)
{
    memset(id, 0, sizeof *id);
    strcpy(id->build, "0123456789abcdef0123456789abcdef01234567");
    strcpy(id->disc_serial, "SLUS-00000");
    strcpy(id->bios_stem, "scph1001");
    strcpy(id->boot_mode, "hle");
    id->disc_digest_kind = REPLAY_DISC_DIGEST_FILE;
    id->disc_digest[0] = 9;
}

static uint16_t word_at(uint32_t i) { return (uint16_t)(0xFFFFu ^ (1u << (i / 10 % 16))); }

/* Writes a replay whose END checkpoint matches the stub's RAM and cycle.
 * Digests: frame 0 with s_digest, frame 60 with `mid`, frame 120 with `last`. */
static void write_replay_digests(int power_on, const uint32_t mid[4], const uint32_t last[4])
{
    static ReplayWord words[FRAMES];
    static ReplayCheckpoint end;
    static uint8_t cards[REPLAY_FILE_CARD_BYTES];
    uint32_t digests[3][5];
    uint8_t anchor[64];
    ReplayIdentity id;
    ReplayFileOut o;
    FILE *f;
    identity(&id);
    for (uint32_t i = 0; i < FRAMES; ++i) {
        words[i].buttons = word_at(i);
        words[i].axes_ly_lx_ry_rx[0] = 0x10;
        words[i].axes_ly_lx_ry_rx[1] = 0x20;
        words[i].axes_ly_lx_ry_rx[2] = 0x30;
        words[i].axes_ly_lx_ry_rx[3] = 0x40;
    }
    replay_file_checkpoint(FRAMES, s_ram, s_cycle, &end);
    for (unsigned k = 0; k < 3; ++k) {
        digests[k][0] = 60u * k;
        memcpy(&digests[k][1], k == 2 ? last : k == 1 ? mid : s_digest, 16);
    }
    memset(anchor, 0xA5, sizeof anchor);
    memset(cards, 0x3C, sizeof cards);
    memset(&o, 0, sizeof o);
    o.id = &id;
    o.words = words;
    o.frames = FRAMES;
    o.end = &end;
    o.settings = "cd_speed=2\n";
    o.digests = (const uint32_t(*)[5])digests;
    o.digest_count = 3;
    o.product = "exe_sha256=aa\nplatform=windows-x64\n";
    if (power_on) {
        o.power_on = 1;
        o.cards = cards;
        o.cards_mask = 2u;
    } else {
        o.anchor = anchor;
        o.anchor_length = sizeof anchor;
    }
    f = fopen(REPLAY, "wb");
    CHECK(f && replay_file_write(f, &o) == NULL);
    if (f) fclose(f);
}

static void write_replay(int power_on, const uint32_t last[4]) { write_replay_digests(power_on, s_digest, last); }

static void reset_host(void)
{
    s_at_power_on = s_loads = s_cards_in = s_cards_out = s_settings_in = s_settings_out = 0;
    s_load_result = 0;
    identity(&s_player);
    s_player_product = "exe_sha256=aa\nplatform=windows-x64\n";
    s_code_lines = CODE_LINES;
    s_code_count = 2;
    s_code_trace[0] = 0;
    s_unit_ok = 1;
    remove(VERDICT);
}

/* Runs boundaries until playback ends; checks every fed word. */
static uint32_t run(uint16_t live, int *fed_ok)
{
    const uint8_t centre[4] = {0x80, 0x80, 0x80, 0x80};
    uint32_t fed = 0;
    *fed_ok = 1;
    for (int guard = 0; guard < 1000 && replay_session_state() != REPLAY_IDLE; ++guard) {
        uint16_t b = 0;
        uint8_t s[4] = {0, 0, 0, 0};
        if (replay_session_boundary(guard > 5 ? live : 0xFFFFu, centre, &b, s)) {
            if (b != word_at(fed) || s[0] != 0x20 || s[1] != 0x10 || s[2] != 0x40 || s[3] != 0x30) *fed_ok = 0;
            ++fed;
        }
    }
    return fed;
}

static uint16_t rec_word(uint32_t i) { return (uint16_t)(0xFFFFu ^ (1u << (i / 7 % 16))); }

/* Records `frames` boundaries of rec_word, then stops: by request (the file
 * is written at the boundary after) or by shutdown. */
static uint32_t record(uint32_t frames, int by_shutdown)
{
    uint32_t delivered = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const uint8_t sticks[4] = {(uint8_t)i, 0x80, 0x81, 0x82};
        uint16_t b = 0;
        uint8_t s[4];
        if (replay_session_boundary(rec_word(i), sticks, &b, s) && b == rec_word(i) && !memcmp(s, sticks, 4))
            ++delivered;
        CHECK(replay_session_owns_p1());
    }
    if (by_shutdown) {
        replay_session_shutdown();
    } else {
        const uint8_t centre[4] = {0x80, 0x80, 0x80, 0x80};
        uint16_t b;
        uint8_t s[4];
        replay_session_stop_record();
        CHECK(replay_session_state() == REPLAY_RECORDING);
        CHECK(replay_session_boundary(0xFFFFu, centre, &b, s) == 0);
    }
    CHECK(replay_session_state() == REPLAY_IDLE && !replay_session_owns_p1());
    return delivered;
}

/* Plays the file back; every fed word must be the recorded one. */
static int play_back(const char *path, uint32_t frames)
{
    const uint8_t centre[4] = {0x80, 0x80, 0x80, 0x80};
    uint32_t fed = 0, ok = 1;
    s_load_result = 1;
    if (!replay_session_play_file(path)) return 0;
    for (int guard = 0; guard < 1000 && replay_session_state() != REPLAY_IDLE; ++guard) {
        uint16_t b = 0;
        uint8_t s[4];
        if (replay_session_boundary(0xFFFFu, centre, &b, s)) {
            if (b != rec_word(fed) || s[0] != (uint8_t)fed || s[1] != 0x80 || s[2] != 0x81 || s[3] != 0x82) ok = 0;
            ++fed;
        }
    }
    return ok && fed == frames && replay_session_last_result() == REPLAY_RESULT_IN_SYNC;
}

static int verdict_has(const char *text)
{
    char buf[4096];
    FILE *f = fopen(VERDICT, "rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    return strstr(buf, text) != NULL;
}

int main(void)
{
    const uint8_t centre[4] = {0x80, 0x80, 0x80, 0x80};
    uint32_t av_only[4] = {1, 99, 3, 4}, core[4] = {77, 2, 3, 4}, frame;
    unsigned parts;
    int ok;
    uint16_t b;
    uint8_t s[4];
    for (uint32_t i = 0; i < sizeof s_ram; ++i) s_ram[i] = (uint8_t)(i * 31u);
    replay_session_set_verdict_path(VERDICT);

    /* Anchored, in sync: the anchor loads first, then every record is fed. */
    reset_host();
    write_replay(0, s_digest);
    CHECK(replay_session_play_file(REPLAY) == 1);
    CHECK(replay_session_state() == REPLAY_LOADING && s_loads == 1);
    CHECK(!s_admitting);   /* a file with no code log */
    CHECK(s_anchor_size == 64 && s_anchor_first == 0xA5);
    CHECK(s_settings_in == 1 && !strcmp(s_settings, "cd_speed=2\n"));
    CHECK(!replay_session_owns_p1());
    CHECK(replay_session_boundary(0xFFFFu, centre, &b, s) == 0); /* load pending */
    s_load_result = 1;
    CHECK(run(0xFFFFu, &ok) == FRAMES && ok);
    CHECK(replay_session_last_result() == REPLAY_RESULT_IN_SYNC);
    CHECK(replay_session_digests_checked() == 3);
    CHECK(s_settings_out == 1 && s_cards_in == 0);
    CHECK(verdict_has("\"result\": \"in_sync\"") && verdict_has("\"frames_played\": 130"));
    CHECK(!s_admitting);
    /* A file without a code set (as older recorders wrote) plays as before
     * and compares nothing. */
    CHECK(verdict_has("\"recorded_code_units\": null") && verdict_has("\"recorded_code_digest\": null"));
    CHECK(verdict_has("\"player_code_units\": 2") && verdict_has("\"code_set_match\": null"));

    /* AV alone differs: reported, still in sync. */
    reset_host();
    write_replay(0, av_only);
    s_load_result = 1;
    CHECK(replay_session_play_file(REPLAY) == 1);
    run(0xFFFFu, &ok);
    CHECK(replay_session_last_result() == REPLAY_RESULT_IN_SYNC);
    CHECK(replay_session_first_divergence(&frame, &parts) && frame == 120 && parts == REPLAY_DIGEST_AV);

    /* CORE differs: out of sync although RAM and cycle match at the end. */
    reset_host();
    write_replay(0, core);
    s_load_result = 1;
    CHECK(replay_session_play_file(REPLAY) == 1);
    run(0xFFFFu, &ok);
    CHECK(replay_session_last_result() == REPLAY_RESULT_OUT_OF_SYNC);
    CHECK(verdict_has("\"result\": \"diverged\"") && verdict_has("\"divergence_parts\": \"core\""));
    CHECK(verdict_has("\"first_failing_frame\": 120") && verdict_has("\"failing_parts\": \"core\""));

    /* AV differs first, CORE later: the later CORE divergence still fails it,
     * and the first divergence stays the AV one. */
    reset_host();
    write_replay_digests(0, av_only, core);
    s_load_result = 1;
    CHECK(replay_session_play_file(REPLAY) == 1);
    run(0xFFFFu, &ok);
    CHECK(replay_session_last_result() == REPLAY_RESULT_OUT_OF_SYNC);
    CHECK(replay_session_first_divergence(&frame, &parts) && frame == 60 && parts == REPLAY_DIGEST_AV);
    CHECK(verdict_has("\"result\": \"diverged\"") && verdict_has("\"first_divergence_frame\": 60"));
    CHECK(verdict_has("\"first_failing_frame\": 120") && verdict_has("\"failing_parts\": \"core\""));

    /* RAM differs at the end. */
    reset_host();
    write_replay(0, s_digest);
    s_ram[12345] ^= 1;
    s_load_result = 1;
    CHECK(replay_session_play_file(REPLAY) == 1);
    run(0xFFFFu, &ok);
    s_ram[12345] ^= 1;
    CHECK(replay_session_last_result() == REPLAY_RESULT_OUT_OF_SYNC);
    CHECK(verdict_has("\"differing_ram_pages\": 1"));

    /* Live input after a neutral pad takes over. */
    reset_host();
    write_replay(0, s_digest);
    s_load_result = 1;
    CHECK(replay_session_play_file(REPLAY) == 1);
    CHECK(run(0xBFFFu, &ok) == 6 && ok);
    CHECK(replay_session_last_result() == REPLAY_RESULT_TAKEN_OVER);
    CHECK(verdict_has("stopped_by_input") && s_settings_out == 1);

    /* The anchor does not load. */
    reset_host();
    write_replay(0, s_digest);
    s_load_result = -1;
    CHECK(replay_session_play_file(REPLAY) == 1);
    CHECK(replay_session_boundary(0xFFFFu, centre, &b, s) == 0);
    CHECK(replay_session_last_result() == REPLAY_RESULT_FAILED && replay_session_state() == REPLAY_IDLE);

    /* Power-on: only at the first vblank; the cards go in, record 1 is fed at
     * once, and the player's cards come back at the end. */
    reset_host();
    write_replay(1, s_digest);
    CHECK(replay_session_play_file(REPLAY) == 0);
    CHECK(replay_session_last_result() == REPLAY_RESULT_FAILED && verdict_has("power-on"));
    s_at_power_on = 1;
    CHECK(replay_session_play_file(REPLAY) == 1);
    CHECK(replay_session_state() == REPLAY_PLAYING && s_loads == 0);
    CHECK(s_cards_in == 1 && s_cards_mask == 2u && s_card2_first == 0x3C);
    CHECK(run(0xFFFFu, &ok) == FRAMES && ok);
    CHECK(replay_session_last_result() == REPLAY_RESULT_IN_SYNC && s_cards_out == 1);
    CHECK(verdict_has("\"power_on\": true"));

    /* Identity: another disc is refused; another build plays with a warning;
     * a shutdown mid-playback restores the player's settings. */
    reset_host();
    write_replay(0, s_digest);
    s_player.disc_digest[0] = 8;
    CHECK(replay_session_play_file(REPLAY) == 0);
    CHECK(s_settings_in == 0 && verdict_has("different game or disc"));
    reset_host();
    s_player.build[0] = 'f';
    CHECK(replay_session_play_file(REPLAY) == 1);
    CHECK(replay_session_play_file(REPLAY) == 0); /* one at a time */
    replay_session_shutdown();
    CHECK(replay_session_state() == REPLAY_IDLE && s_settings_out == 1);
    CHECK(verdict_has("\"player_build\": \"f123456789abcdef0123456789abcdef01234567\""));
    CHECK(replay_session_play_file("replay_session_missing.psxrpl") == 0);
    CHECK(!s_admitting && !s_watch);

    /* Anchored recording: arms, waits for the anchor, records each boundary's
     * live word, and plays back in sync from that anchor. */
    reset_host();
    remove(REPLAY);
    s_anchor_take = 0;
    CHECK(replay_session_record_to(REPLAY) == 1);
    CHECK(replay_session_state() == REPLAY_ARMING && s_anchor_requests == 1);
    CHECK(!s_watch);   /* the code log starts with the recording */
    CHECK(replay_session_record_to(REPLAY) == 0); /* one at a time */
    CHECK(replay_session_boundary(0xFFFFu, centre, &b, s) == 0); /* anchor pending */
    s_anchor_take = 1;
    CHECK(record(75, 0) == 75);
    CHECK(!s_watch);
    {
        ReplayFileInfo info;
        static ReplayStep steps[128];
        FILE *f = fopen(REPLAY, "rb");
        CHECK(f && replay_file_read(f, &info, steps, 128) == NULL);
        if (f) fclose(f);
        CHECK(info.frames == 75 && info.has_anchor && info.anchor_length == 48 && !info.power_on);
        CHECK(info.digest_count == 2 && !strcmp(info.settings, "cd_speed=1\n"));
        CHECK(strstr(info.product, "platform=windows-x64") != NULL);
        {
            /* The code set held at the start: sorted, digested, listed. */
            static const char sorted[] = "gcc/x/a.dll\ngcc/x/b.dll\n";
            char expect[256];
            uint8_t d[32];
            size_t n;
            psx_sha256_compute((const uint8_t *)sorted, strlen(sorted), d);
            n = (size_t)snprintf(expect, sizeof expect, "count=2\ndigest=");
            for (unsigned k = 0; k < 32; ++k) n += (size_t)snprintf(expect + n, sizeof expect - n, "%02x", d[k]);
            snprintf(expect + n, sizeof expect - n, "\nunit=gcc/x/a.dll\nunit=gcc/x/b.dll\n");
            CHECK(!strcmp(info.code_set, expect));
        }
        CHECK(info.code_log_length == strlen("0 S gcc/x/b.dll\n0 S gcc/x/a.dll\n"));
    }
    CHECK(play_back(REPLAY, 75) && s_anchor_size == 48 && s_anchor_first == 0x5A);
    CHECK(verdict_has("\"code_set_match\": true") && verdict_has("\"recorded_code_units\": 2"));
    /* Another code set is reported, and the replay still plays. */
    s_code_lines = "gcc/x/a.dll\n";
    s_code_count = 1;
    CHECK(play_back(REPLAY, 75));
    CHECK(verdict_has("\"code_set_match\": false") && verdict_has("\"recorded_code_units\": 2") &&
          verdict_has("\"player_code_units\": 1"));
    /* A player that names no code set compares nothing. */
    s_code_lines = NULL;
    CHECK(play_back(REPLAY, 75));
    CHECK(verdict_has("\"code_set_match\": null") && verdict_has("\"player_code_digest\": null"));
    s_code_lines = CODE_LINES;
    s_code_count = 2;
    CHECK(replay_session_record_to(REPLAY) == 0); /* the file exists */

    /* The compiler runs while recording: units that become runnable are
     * logged with the boundary they come before, and a playback admits each
     * at that frame, loading a published one and rescanning for an indexed
     * one. */
    reset_host();
    remove(REPLAY);
    s_at_power_on = 1;
    CHECK(replay_session_record_power_on(REPLAY) == 1 && s_watch);
    for (uint32_t i = 0; i < 40; ++i) {
        const uint8_t sticks[4] = {(uint8_t)i, 0x80, 0x81, 0x82};
        if (i == 10) s_watch('L', "gcc/x/c.dll");
        if (i == 25) { s_watch('I', "gcc/x/d.dll"); s_watch('I', "gcc/x/e.dll"); }
        replay_session_boundary(rec_word(i), sticks, &b, s);
    }
    replay_session_shutdown();
    CHECK(!s_watch);
    {
        static const char want[] = "0 S gcc/x/b.dll\n0 S gcc/x/a.dll\n10 L gcc/x/c.dll\n"
                                   "25 I gcc/x/d.dll\n25 I gcc/x/e.dll\n";
        ReplayFileInfo info;
        static ReplayStep steps[128];
        char text[256] = "";
        FILE *f = fopen(REPLAY, "rb");
        CHECK(f && replay_file_read(f, &info, steps, 128) == NULL);
        CHECK(f && info.code_log_length == strlen(want) && !fseek(f, info.code_log_offset, SEEK_SET) &&
              fread(text, 1, info.code_log_length, f) == info.code_log_length);
        if (f) fclose(f);
        CHECK(!strcmp(text, want));
    }
    {
        /* Playback, frame by frame: nothing is loaded before its frame. */
        const uint8_t centre4[4] = {0x80, 0x80, 0x80, 0x80};
        uint32_t fed = 0;
        int seen10 = 0, seen25 = 0;
        CHECK(replay_session_play_file(REPLAY) == 1 && s_admitting);
        CHECK(!strcmp(s_code_trace, "a:gcc/x/b.dll;p:gcc/x/b.dll;a:gcc/x/a.dll;p:gcc/x/a.dll;r:-;"));
        s_code_trace[0] = 0;
        for (int guard = 0; guard < 100 && replay_session_state() != REPLAY_IDLE; ++guard) {
            const int ok = replay_session_boundary(0xFFFFu, centre4, &b, s);
            if (fed == 10 && !seen10) {
                CHECK(!strcmp(s_code_trace, "a:gcc/x/c.dll;l:gcc/x/c.dll;"));
                seen10 = 1;
                s_code_trace[0] = 0;
            } else if (fed == 25 && !seen25) {
                CHECK(!strcmp(s_code_trace, "a:gcc/x/d.dll;w:gcc/x/d.dll;a:gcc/x/e.dll;w:gcc/x/e.dll;r:-;"));
                seen25 = 1;
                s_code_trace[0] = 0;
            } else {
                CHECK(!s_code_trace[0]);
            }
            fed += (uint32_t)ok;
        }
        CHECK(seen10 && seen25 && fed == 39 && replay_session_last_result() == REPLAY_RESULT_IN_SYNC);
        CHECK(!s_admitting);
        CHECK(verdict_has("\"code_log_events\": 5") && verdict_has("\"code_units_missing_at_start\": 0"));
    }
    /* A unit the player cannot compile in time stops the playback. */
    s_code_trace[0] = 0;
    s_unit_ok = 0;
    {
        const uint8_t centre4[4] = {0x80, 0x80, 0x80, 0x80};
        CHECK(replay_session_play_file(REPLAY) == 1);
        for (int guard = 0; guard < 100 && replay_session_state() != REPLAY_IDLE; ++guard)
            replay_session_boundary(0xFFFFu, centre4, &b, s);
        CHECK(replay_session_last_result() == REPLAY_RESULT_FAILED && !s_admitting);
        CHECK(verdict_has("not compiled in time") && verdict_has("\"frames_played\": 10"));
        CHECK(verdict_has("\"code_units_missing_at_start\": 2"));
    }
    s_unit_ok = 1;
    /* A file without a code log plays with no admission. */
    reset_host();
    write_replay(0, s_digest);
    s_load_result = 1;
    CHECK(replay_session_play_file(REPLAY) == 1 && !s_admitting);
    {
        int fed_ok;
        CHECK(run(0xFFFFu, &fed_ok) == FRAMES && fed_ok);
    }
    CHECK(!s_code_trace[0] && verdict_has("\"code_log_events\": null"));

    /* Power-on recording: only at boot; it keeps the inserted card, ends at
     * shutdown, and plays back with that card installed. */
    reset_host();
    remove(REPLAY);
    CHECK(replay_session_record_power_on(REPLAY) == 0);
    s_at_power_on = 1;
    CHECK(replay_session_record_power_on(REPLAY) == 1 && replay_session_state() == REPLAY_RECORDING);
    CHECK(record(40, 1) == 40);
    CHECK(play_back(REPLAY, 39) && s_cards_in == 1 && s_cards_mask == 2u && s_card2_first == 0x77);

    /* Refusals: the host cannot record; the anchor cannot be saved. */
    reset_host();
    remove(REPLAY);
    s_can_record = 0;
    CHECK(replay_session_record_to(REPLAY) == 0 && replay_session_state() == REPLAY_IDLE);
    s_can_record = 1;
    s_anchor_take = -1;
    CHECK(replay_session_record_to(REPLAY) == 1);
    CHECK(replay_session_boundary(0xFFFFu, centre, &b, s) == 0 && replay_session_state() == REPLAY_IDLE);
    s_anchor_take = 1;
    CHECK(replay_session_record_to(REPLAY) == 1);
    replay_session_shutdown(); /* armed, nothing recorded: no file */
    CHECK(replay_session_state() == REPLAY_IDLE && replay_session_play_file(REPLAY) == 0);
    CHECK(!s_watch);

    remove(REPLAY);
    remove(VERDICT);
    if (failures) {
        fprintf(stderr, "replay_session_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("replay_session_test: ok\n");
    return 0;
}
