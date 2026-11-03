/*
 * test_lobby_chat_ring.c — the lobby chat ring, without a lobby.
 *
 * The server echoes every line to everyone in the room INCLUDING the sender,
 * and that echo is the copy the ring keeps: the client never appends its own
 * send. So the order on screen is the room's order, "mine" is a question
 * about player ids rather than about who called send, and the ring's only
 * jobs are to keep the newest lines and to number them monotonically.
 *
 * This includes psx_lobby_client.c directly so it exercises the real
 * chat_push / chat_get rather than a copy of them.
 *
 * Build/run: ctest -R lobby_chat_ring_test
 * (registered in runtime/CMakeLists.txt, which owns the source list.)
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>

#include "../src/psx_lobby_client.c"

/* The real recomp_net and the real WS helpers are linked in (see
 * runtime/CMakeLists.txt) rather than stubbed: nothing here reaches a socket,
 * so a wall of fakes would buy nothing and could drift from what the runtime
 * actually links. The one exception is the host clock, which lives in the
 * runtime's own main TU. */
uint64_t psx_host_mono_ms(void);
uint64_t psx_host_mono_ms(void)
{
    /* Monotonic and cheap. The ring does not read the clock; this only has to
     * exist and never go backwards. */
    static uint64_t t;
    return ++t;
}

static int g_failures;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); g_failures++; }
}

static void case_room_order(void)
{
    PsxLobbyChatMsg got;
    printf("  room order\n");
    memset(&g_lc, 0, sizeof(g_lc));
    snprintf(g_lc.player_id, sizeof(g_lc.player_id), "%s", "me");

    chat_push("them", "Them", "first", NULL, NULL, 0);
    chat_push("me",   "Me",   "second", NULL, NULL, 0);
    chat_push("",     "",     "third", NULL, NULL, 1);

    ck(psx_lobby_chat_count() == 3, "three lines land");
    ck(psx_lobby_chat_get(0, &got) && strcmp(got.text, "first") == 0,
       "index 0 is the oldest");
    ck(psx_lobby_chat_get(2, &got) && strcmp(got.text, "third") == 0,
       "index 2 is the newest");

    ck(psx_lobby_chat_get(0, &got) && got.is_local == 0,
       "a peer's line is not local");
    ck(psx_lobby_chat_get(1, &got) && got.is_local == 1,
       "our own echoed line is local");
    ck(psx_lobby_chat_get(2, &got) && got.is_system == 1 && got.is_local == 0,
       "a system line is neither ours nor a player's");

    {
        uint32_t a, b;
        (void)psx_lobby_chat_get(0, &got); a = got.seq;
        (void)psx_lobby_chat_get(2, &got); b = got.seq;
        ck(b > a, "seq increases with arrival order");
    }
    ck(!psx_lobby_chat_get(3, &got), "reading past the end is refused");
    ck(!psx_lobby_chat_get(-1, &got), "so is a negative index");
}

static void case_wrap_drops_oldest(void)
{
    PsxLobbyChatMsg got;
    char buf[32];
    int i;
    printf("  wrap\n");
    memset(&g_lc, 0, sizeof(g_lc));
    snprintf(g_lc.player_id, sizeof(g_lc.player_id), "%s", "me");

    /* Overfill by ten. The ring must drop the OLDEST: a chat that discards
     * what was just said is worse than no chat at all. */
    for (i = 0; i < PSX_LOBBY_CHAT_RING + 10; ++i) {
        snprintf(buf, sizeof(buf), "line%d", i);
        chat_push("them", "Them", buf, NULL, NULL, 0);
    }
    ck(psx_lobby_chat_count() == PSX_LOBBY_CHAT_RING,
       "the ring stops at its capacity");
    ck(psx_lobby_chat_get(0, &got) && strcmp(got.text, "line10") == 0,
       "the oldest surviving line is the 11th sent");
    snprintf(buf, sizeof(buf), "line%d", PSX_LOBBY_CHAT_RING + 9);
    ck(psx_lobby_chat_get(PSX_LOBBY_CHAT_RING - 1, &got) &&
       strcmp(got.text, buf) == 0,
       "the newest line is the last one sent");
}

static void case_empty_and_clear(void)
{
    PsxLobbyChatMsg m;
    uint32_t before;
    printf("  empty/clear\n");
    memset(&g_lc, 0, sizeof(g_lc));

    chat_push("them", "Them", "", NULL, NULL, 0);
    chat_push("them", "Them", NULL, NULL, NULL, 0);
    ck(psx_lobby_chat_count() == 0, "an empty line is not a line");

    chat_push("them", "Them", "hello", NULL, NULL, 0);
    ck(psx_lobby_chat_count() == 1, "a real line is");
    (void)psx_lobby_chat_get(0, &m);
    before = m.seq;

    psx_lobby_chat_clear();
    ck(psx_lobby_chat_count() == 0, "clear empties the room log");

    chat_push("them", "Them", "new room", NULL, NULL, 0);
    (void)psx_lobby_chat_get(0, &m);
    /* seq must NOT restart. A UI tracking "newest seen" would otherwise
     * mistake the first line of a new room for one it had already scrolled
     * past, and never scroll to it. */
    ck(m.seq > before, "seq keeps counting across a clear");
}

static void case_long_line_is_truncated_not_dropped(void)
{
    PsxLobbyChatMsg got;
    char big[PSX_LOBBY_CHAT_TEXT_LEN * 2];
    printf("  long line\n");
    memset(&g_lc, 0, sizeof(g_lc));
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    chat_push("them", "Them", big, NULL, NULL, 0);
    ck(psx_lobby_chat_count() == 1, "an over-long line still arrives");
    ck(psx_lobby_chat_get(0, &got), "and reads back");
    ck(strlen(got.text) == PSX_LOBBY_CHAT_TEXT_LEN - 1,
       "clipped to the field, not past it");
    ck(got.text[PSX_LOBBY_CHAT_TEXT_LEN - 1] == '\0',
       "and still NUL-terminated");
}

static void case_mod_caps_roundtrip(void)
{
    PsxLobbyMatchCaps caps = {0};
    char wire[4096];
    caps.valid = 1;
    caps.aspect_num = 4;
    caps.aspect_den = 3;
    caps.rollback = 1;
    caps.mod_count = 2;
    memset(caps.mod_plan_fp, 'a', 64);
    strcpy(caps.mods[0].id, "content.cars");
    strcpy(caps.mods[0].ver, "1.0.0");
    strcpy(caps.mods[0].name, "Cars \"Original\"");
    strcpy(caps.mods[0].feats, "roster,view=mode~fit");
    strcpy(caps.mods[1].id, "content.arena");
    strcpy(caps.mods[1].ver, "0.1.0");
    strcpy(caps.mods[1].feats, "arena");
    ck(append_match_caps_json(wire, sizeof(wire), &caps), "serialize host mod caps");
    ingest_match_caps_from_json(wire);
    ck(g_lc.match_caps.valid && g_lc.match_caps.rollback &&
       g_lc.match_caps.mod_count == 2, "roundtrip two required packages");
    ck(!strcmp(g_lc.match_caps.mod_plan_fp, caps.mod_plan_fp) &&
       !strcmp(g_lc.match_caps.mods[0].id, "content.cars") &&
       !strcmp(g_lc.match_caps.mods[0].feats, "roster,view=mode~fit") &&
       !strcmp(g_lc.match_caps.mods[1].ver, "0.1.0"),
       "roundtrip fingerprint, versions, features and options");
    ck(strstr(wire, "resources") == NULL && strstr(wire, "path") == NULL,
       "lobby plan carries no donor paths or asset bytes");
    {
        PsxLobbyMatchCaps received = {0};
        char tiny[32];
        ck(psx_lobby_encode_match_caps(wire, sizeof(wire), &caps), "LAN uses same host caps serializer");
        ck(psx_lobby_decode_match_caps(wire, &received) && received.mod_count == 2 &&
           !strcmp(received.mod_plan_fp, caps.mod_plan_fp) &&
           !strcmp(received.mods[0].feats, caps.mods[0].feats), "LAN preserves host selections and options");
        ck(!psx_lobby_encode_match_caps(tiny, sizeof(tiny), &caps), "truncated LAN plans fail closed");
        wire[strlen(wire) - 2] = '\0';
        ck(!psx_lobby_decode_match_caps(wire, &received), "incomplete LAN caps rejected");
        psx_lobby_set_local_match_caps(&caps);
        ck(psx_lobby_match_caps()->mod_count == 2, "LAN plan supplied to launch provider");
        psx_lobby_set_local_match_caps(NULL);
        ck(!psx_lobby_match_caps()->valid, "leaving LAN retires host plan");
    }
    parse_match_caps_object("{\"mod_plan_fp\":\"invalid!\",\"mods\":[]}", &caps);
    ck(!caps.mod_plan_fp[0], "invalid fingerprint rejected");
}

static void case_profile_settings_roundtrip(void)
{
    PsxLobbyMatchCaps caps = {0}, received = {0};
    char wire[4096];
    caps.valid = 1;
    caps.aspect_num = 4;
    caps.aspect_den = 3;
    caps.mod_count = 1;
    strcpy(caps.mods[0].id, "runtime.test");
    strcpy(caps.mods[0].ver, "1.0.0");
    strcpy(caps.mods[0].feats, "vblank-plugin=cameras~split");
    ck(psx_lobby_encode_match_caps(wire, sizeof(wire), &caps) &&
       !psx_lobby_decode_match_caps(wire, &received), "settings without an identity cannot enter a LAN session");
    strcpy(caps.mod_plan_fp, "24a3aeaf6034d1a59df3ac27334227343237550d4938b7b00c6ae77dfec21367");
    ck(psx_lobby_encode_match_caps(wire, sizeof(wire), &caps) &&
       psx_lobby_decode_match_caps(wire, &received) && received.mod_count == 1 &&
       !strcmp(received.mod_plan_fp, caps.mod_plan_fp) &&
       !strcmp(received.mods[0].feats, caps.mods[0].feats), "trusted settings survive the real LAN codec");
    ingest_match_caps_from_json(wire);
    ck(g_lc.match_caps.mod_count == 1 &&
       !strcmp(g_lc.match_caps.mod_plan_fp, caps.mod_plan_fp) &&
       !strcmp(g_lc.match_caps.mods[0].feats, caps.mods[0].feats), "trusted settings survive the online lobby codec");
}

static void case_full_title_profile_version(void)
{
    static char version[64];
    static PSXModNetplayProfile profile = {0};
    char wire[768];
    memset(version, 'v', sizeof(version) - 1);
    version[sizeof(version) - 1] = '\0';
    profile.plugin_id = "lobby.profile.test";
    profile.compatibility_id = version;
    ck(psx_mod_register_netplay_profile(&profile), "register maximum-length title profile");
    psx_lobby_set_game_identity("Profile Fixture", version);
    ck(!strcmp(g_lc.filter_game_version, version), "retain all 63 profile bytes in local metadata");
    /* Suppress discovery: this fixture checks the native codec/filter only. */
    g_list_rtt_active = 1;
    snprintf(wire, sizeof(wire), "{\"op\":\"lobby_list\",\"lobbies\":[{"
             "\"lobby_id\":\"profile-room\",\"name\":\"Profile Room\","
             "\"game_name\":\"Profile Fixture\",\"game_version\":\"%s\"}]}", version);
    handle_server_json(wire);
    ck(g_lc.list_count == 1, "matching full profile remains visible");
    ck(!strcmp(g_lc.list[0].game_version, version), "room metadata retains full profile");
    /* A difference beyond the former 31-byte prefix must still reject. */
    char *last = strstr(wire, version);
    if (last) last[62] = 'w';
    ck(last != NULL, "locate profile in fixture");
    handle_server_json(wire);
    ck(g_lc.list_count == 0, "reject profile differing only in final byte");
    g_list_rtt_active = 0;
}

static void case_start_retains_relay_path(void)
{
    PsxLobbyMatchCaps caps = {0}, decoded;
    char wire[4096];
    int prefer, host;
    const int old_pref = g_relay_via_ice_pref;
    printf("  START keeps the advertised relay path locally\n");
    for (prefer = 0; prefer < 2; ++prefer) {
        for (host = 0; host < 2; ++host) {
            memset(&g_lc, 0, sizeof(g_lc));
            /* An unhandshaken fixture queues START without touching a socket. */
            g_lc.connected = g_lc.in_lobby = g_lc.is_host = 1;
            g_lc.fd = 0;
            caps.valid = 1;
            caps.relay_host = host;
            caps.relay_via_ice = 0; /* fresh launcher caps, as in the regression */
            strcpy(caps.session_bios, "openbios");
            psx_lobby_set_relay_via_ice_pref(prefer);
            ck(psx_lobby_request_start(&caps) == 0, "queue host START");
            ck(psx_lobby_encode_match_caps(wire, sizeof(wire), &caps) &&
               psx_lobby_decode_match_caps(wire, &decoded), "decode advertised START caps");
            ck(g_lc.match_caps.relay_via_ice == decoded.relay_via_ice,
               "local ICE path matches the wire before LAUNCH arrives");
            ck(room_relays_via_ice() == (host && prefer && rnet_host_ice_available()),
               "waiting-room agents remain active for an ICE host START");
        }
    }
    memset(&g_lc, 0, sizeof(g_lc));
    g_lc.fd = -1;
    psx_lobby_set_relay_via_ice_pref(old_pref);
}

static void init_mod_caps(PsxLobbyMatchCaps *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->valid = 1;caps->aspect_num = 4;caps->aspect_den = 3;
    strcpy(caps->session_bios, "openbios");strcpy(caps->language, "en");
    memset(caps->mod_plan_fp, 'c', 64);
}

static void case_complete_start_plan(void)
{
    PsxLobbyMatchCaps caps, received = {0};
    char wire[PSX_LOBBY_START_JSON_CAP], tiny[640];
    printf("  START preserves the complete X4 option plan\n");
    init_mod_caps(&caps);caps.mod_count = 1;
    strcpy(caps.mods[0].id, "mmx4.coop");strcpy(caps.mods[0].ver, "0.0.1");
    strcpy(caps.mods[0].name, "X + Zero Co-op (Playtest)");caps.mods[0].builtin = 1;
    strcpy(caps.mods[0].feats,
        "coop=black_zero~true+cameras~unified+hud_layout~side_by_side+ultimate_armor_x~true");
    ck(psx_lobby_start_message(&caps, wire, sizeof(wire)) == 0,
       "long real title options fit the START codec");
    ck(strlen(wire) > sizeof(tiny), "regression plan exceeds former 640-byte nested buffer");
    ck(psx_lobby_decode_match_caps(wire, &received) && received.mod_count == 1 &&
       !strcmp(received.mods[0].feats, caps.mods[0].feats), "all four agreed options roundtrip through START");
    memset(tiny, '!', sizeof(tiny));
    ck(psx_lobby_start_message(&caps, tiny, sizeof(tiny)) == -1 && !tiny[0],
       "small START output refuses and clears partial JSON");
    {
        char nested[PSX_LOBBY_MATCH_CAPS_JSON_CAP];
        int length = append_match_caps_json(nested, sizeof(nested), &caps);
        ck(length > 0, "measure complete nested capability field");
        ck(length >= (int)sizeof(tiny), "real package metadata and side-by-side HUD exceed the old nested bound");
        /* START adds its operation and closing brace, so a 640-byte START
         * failure does not imply its smaller nested field cannot fit 640.
         * Refuse exactly one byte short of the measured nested capacity. */
        size_t insufficient = length > 0 && (size_t)length < sizeof(tiny)
            ? (size_t)length : sizeof(tiny);
        ck(!append_match_caps_json(tiny, insufficient, &caps) && !tiny[0],
           "nested caps codec also refuses and clears truncation");
    }
    memset(&g_lc, 0, sizeof(g_lc));g_lc.connected = g_lc.in_lobby = g_lc.is_host = 1;
    ck(psx_lobby_request_start(&caps) == 0 && g_lc.pending_n == 1,
       "production host START queues the complete message");
    ck(!strcmp(g_lc.pending_tx[0], wire) &&
       psx_lobby_decode_match_caps(g_lc.pending_tx[0], &received) &&
       !strcmp(received.mods[0].feats, caps.mods[0].feats), "actual queued START retains the option plan");
    ck(psx_lobby_start_message(NULL, wire, sizeof(wire)) == 0 &&
       !strcmp(wire, "{\"op\":\"start\"}"), "legacy no-caps START stays compatible");
}

static void case_maximum_mod_plan(void)
{
    PsxLobbyMatchCaps caps, received = {0};
    char wire[PSX_LOBBY_START_JSON_CAP];
    char oversized[PSX_LOBBY_MESSAGE_JSON_CAP + 1];
    printf("  declared package/field limits and transport buffers\n");
    init_mod_caps(&caps);caps.mod_count = PSX_LOBBY_MAX_MODS;
    for (int i = 0; i < caps.mod_count; ++i) {
        PsxLobbyModPkg *pkg = &caps.mods[i];
        memset(pkg->id, 'a' + i, sizeof(pkg->id) - 1);
        memset(pkg->ver, 'v', sizeof(pkg->ver) - 1);
        memset(pkg->name, '"', sizeof(pkg->name) - 1);
        /* Delimiters inside quoted names must not terminate nested JSON. */
        pkg->name[0] = '}';pkg->name[1] = ']';pkg->name[2] = '\\';
        memset(pkg->feats, 'f', sizeof(pkg->feats) - 1);
        pkg->size = UINT32_MAX;
    }
    ck(psx_lobby_encode_match_caps(wire, sizeof(wire), &caps), "encode all 12 maximum-length packages");
    ck(strlen(wire) > 8192, "maximum plan exceeds previous queue and decoder limits");
    ck(psx_lobby_decode_match_caps(wire, &received) && received.mod_count == PSX_LOBBY_MAX_MODS,
       "decode all maximum-length packages without dropping entries");
    for (int i = 0; i < caps.mod_count; ++i)
        ck(!strcmp(received.mods[i].id, caps.mods[i].id) &&
           !strcmp(received.mods[i].name, caps.mods[i].name) &&
           !strcmp(received.mods[i].feats, caps.mods[i].feats) &&
           received.mods[i].size == UINT32_MAX, "maximum package fields remain exact");
    ck(psx_lobby_start_message(&caps, wire, sizeof(wire)) == 0,
       "START supports the complete declared plan capacity");
    memset(&g_lc, 0, sizeof(g_lc));g_lc.connected = g_lc.in_lobby = g_lc.is_host = 1;
    ck(psx_lobby_request_start(&caps) == 0 && g_lc.pending_n == 1 &&
       !strcmp(g_lc.pending_tx[0], wire), "production queue does not clip an 8K-plus START");
    ck(psx_lobby_set_match_caps(&caps) == 0 && g_lc.pending_n == 2 &&
       psx_lobby_decode_match_caps(g_lc.pending_tx[1], &received) &&
       received.mod_count == PSX_LOBBY_MAX_MODS, "host capability updates preserve maximum plans");
    ck(psx_lobby_create("Fixture", "Fixture", "dev", "", "0.0.0.0:7777", &caps) == 0 &&
       g_lc.pending_n == 3 && psx_lobby_decode_match_caps(g_lc.pending_tx[2], &received),
       "room creation uses complete nested capabilities");
    g_lc.mod_offer.valid = 1;g_lc.mod_offer.count = caps.mod_count;
    memcpy(g_lc.mod_offer.pkgs, caps.mods, sizeof(caps.mods));
    ck(psx_lobby_join("room", "", "0.0.0.0:7778") == 0 && g_lc.pending_n == 4 &&
       parse_mod_pkg_array(g_lc.pending_tx[3], "pkgs", received.mods, PSX_LOBBY_MAX_MODS) == PSX_LOBBY_MAX_MODS,
       "guest offer preserves complete package list");
    /* Exercise the actual inbound WebSocket frame buffer, without a socket. */
    size_t length = strlen(g_lc.pending_tx[1]);
    g_lc.ws_pending[0] = 0x81;g_lc.ws_pending[1] = 126;
    g_lc.ws_pending[2] = (uint8_t)(length >> 8);g_lc.ws_pending[3] = (uint8_t)length;
    memcpy(g_lc.ws_pending + 4, g_lc.pending_tx[1], length);
    g_lc.ws_pending_len = length + 4;g_lc.match_caps.valid = 0;
    /* Inbound op is lobby_update, not the outgoing set_match_caps operation. */
    const char *caps_field = strstr(g_lc.pending_tx[1], ",\"match_caps\"");
    ck(caps_field != NULL, "find full inbound capability fixture");
    if (caps_field) {
        char frame[PSX_LOBBY_MESSAGE_JSON_CAP];
        snprintf(frame, sizeof(frame), "{\"op\":\"lobby_update\"%s", caps_field);
        length = strlen(frame);g_lc.ws_pending[2] = (uint8_t)(length >> 8);g_lc.ws_pending[3] = (uint8_t)length;
        memcpy(g_lc.ws_pending + 4, frame, length);g_lc.ws_pending_len = length + 4;
        drain_ws_pending();
        ck(g_lc.ws_pending_len == 0 && g_lc.match_caps.valid &&
           g_lc.match_caps.mod_count == PSX_LOBBY_MAX_MODS &&
           !strcmp(g_lc.match_caps.mods[11].feats, caps.mods[11].feats), "full plan survives real incoming frame dispatch");
    }
    memset(oversized, 'x', sizeof(oversized) - 1);oversized[sizeof(oversized) - 1] = '\0';
    int before = g_lc.pending_n;
    ck(!queue_send(oversized) && g_lc.pending_n == before, "oversized queue refuses instead of truncating");
    caps.mod_count = PSX_LOBBY_MAX_MODS + 1;
    PsxLobbyMatchCaps previous = g_lc.match_caps;
    ck(psx_lobby_request_start(&caps) == -1 && g_lc.pending_n == before &&
       !memcmp(&previous, &g_lc.match_caps, sizeof(previous)), "invalid nested START changes neither queue nor local caps");
    ck(psx_lobby_set_match_caps(&caps) == -1 && g_lc.pending_n == before &&
       !memcmp(&previous, &g_lc.match_caps, sizeof(previous)), "invalid host update cannot silently omit the plan");
    ck(psx_lobby_create("Fixture", "Fixture", "dev", "", "", &caps) == -1 &&
       g_lc.pending_n == before, "invalid room caps are refused before enqueue");
    g_lc.mod_offer.count = PSX_LOBBY_MAX_MODS + 1;
    ck(psx_lobby_join("room", "", "") == -1 && g_lc.pending_n == before,
       "invalid guest offer cannot silently become vanilla");
    caps.mod_count = 1;memset(caps.mods[0].feats, 'f', sizeof(caps.mods[0].feats));
    ck(!psx_lobby_encode_match_caps(wire, sizeof(wire), &caps) && !wire[0],
       "unterminated fixed-size feature data is refused safely");
    char oversized_field[PSX_LOBBY_MOD_FEATS_LEN + 1];
    memset(oversized_field, 'f', sizeof(oversized_field) - 1);oversized_field[sizeof(oversized_field) - 1] = '\0';
    snprintf(wire, sizeof(wire), "{\"match_caps\":{\"mods\":[{\"id\":\"fixture\",\"feats\":\"%s\"}]}}", oversized_field);
    ck(!psx_lobby_decode_match_caps(wire, &received), "overlong incoming options are refused instead of shortened");
    memset(&g_lc, 0, sizeof(g_lc));g_lc.fd = -1;
}

int main(void)
{
    case_room_order();
    case_wrap_drops_oldest();
    case_empty_and_clear();
    case_long_line_is_truncated_not_dropped();
    case_mod_caps_roundtrip();
    case_profile_settings_roundtrip();
    case_full_title_profile_version();
    case_start_retains_relay_path();
    case_complete_start_plan();
    case_maximum_mod_plan();
    if (g_failures == 0) {
        printf("lobby_chat_ring_test: ok\n");
        return 0;
    }
    fprintf(stderr, "lobby_chat_ring_test: %d failure(s)\n", g_failures);
    return 1;
}
