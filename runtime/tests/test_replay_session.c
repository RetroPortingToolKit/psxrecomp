/* Replay recording and playback against stub host hooks: the anchor load,
 * the per-vblank P1 feed, the digest and END checks, takeover, power-on with
 * cards, the identity refusals, recordings that play back in sync, and the
 * compiled code held fixed from the start of each replay to its end. */
#include "replay_session.h"
#include "replay_identity.h"
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
/* Compiled code held fixed: frozen while a replay records or plays. */
static int s_can_freeze = 1, s_freezes, s_thaws;
int replay_host_code_freeze(void)
{
    if (!s_can_freeze) return 0;
    s_freezes++;
    return 1;
}
void replay_host_code_thaw(void) { s_thaws++; }
static int frozen(void) { return s_freezes - s_thaws; }
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
 * Digests: frame 0 and frame 60 with s_digest, frame 120 with `last`. */
static void write_replay(int power_on, const uint32_t last[4])
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
        memcpy(&digests[k][1], k == 2 ? last : s_digest, 16);
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

static void reset_host(void)
{
    s_at_power_on = s_loads = s_cards_in = s_cards_out = s_settings_in = s_settings_out = 0;
    s_load_result = 0;
    identity(&s_player);
    s_player_product = "exe_sha256=aa\nplatform=windows-x64\n";
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
    CHECK(frozen() == 1);
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
    CHECK(frozen() == 0);

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
    CHECK(frozen() == 0);

    /* Anchored recording: arms, waits for the anchor, records each boundary's
     * live word, and plays back in sync from that anchor. */
    reset_host();
    remove(REPLAY);
    s_anchor_take = 0;
    CHECK(replay_session_record_to(REPLAY) == 1);
    CHECK(replay_session_state() == REPLAY_ARMING && s_anchor_requests == 1);
    CHECK(frozen() == 1);
    CHECK(replay_session_record_to(REPLAY) == 0); /* one at a time */
    CHECK(replay_session_boundary(0xFFFFu, centre, &b, s) == 0); /* anchor pending */
    s_anchor_take = 1;
    CHECK(record(75, 0) == 75);
    CHECK(frozen() == 0);
    {
        ReplayFileInfo info;
        static ReplayStep steps[128];
        FILE *f = fopen(REPLAY, "rb");
        CHECK(f && replay_file_read(f, &info, steps, 128) == NULL);
        if (f) fclose(f);
        CHECK(info.frames == 75 && info.has_anchor && info.anchor_length == 48 && !info.power_on);
        CHECK(info.digest_count == 2 && !strcmp(info.settings, "cd_speed=1\n"));
        CHECK(strstr(info.product, "platform=windows-x64") != NULL);
    }
    CHECK(play_back(REPLAY, 75) && s_anchor_size == 48 && s_anchor_first == 0x5A);
    CHECK(replay_session_record_to(REPLAY) == 0); /* the file exists */

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
    CHECK(frozen() == 0);

    /* A host that cannot hold its compiled code refuses to play or record,
     * and nothing is left frozen. */
    reset_host();
    write_replay(0, s_digest);
    s_can_freeze = 0;
    CHECK(replay_session_play_file(REPLAY) == 0 && replay_session_state() == REPLAY_IDLE);
    CHECK(s_settings_in == 0 && verdict_has("overlay compiler"));
    remove(REPLAY);
    CHECK(replay_session_record_to(REPLAY) == 0 && replay_session_state() == REPLAY_IDLE);
    s_at_power_on = 1;
    CHECK(replay_session_record_power_on(REPLAY) == 0 && replay_session_state() == REPLAY_IDLE);
    s_can_freeze = 1;
    CHECK(frozen() == 0 && s_freezes > 0);

    remove(REPLAY);
    remove(VERDICT);
    if (failures) {
        fprintf(stderr, "replay_session_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("replay_session_test: ok\n");
    return 0;
}
