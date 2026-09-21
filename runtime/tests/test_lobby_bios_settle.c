/*
 * test_lobby_bios_settle.c — the netplay BIOS settle, without a lobby.
 *
 * The rule this guards: a match may run a retail BIOS only where every seated
 * peer can run THE SAME IMAGE. The previous shape asked "can you run retail?"
 * as a boolean and settled on the word "scph1001"; once one build could run
 * several retail images, two peers could both answer yes, both satisfy the
 * token with different dumps, and desync on the first rollback — with nothing
 * to catch it until a mid-match state transfer aborted the episode.
 *
 * So the settle takes the INTERSECTION of the peers' image lists, keyed on
 * CRC32, and yields a specific image's stem.
 *
 * This includes psx_lobby_client.c directly so it exercises the real
 * psx_lobby_settle_session_bios rather than a copy of it.
 *
 * Build/run: ctest -R lobby_bios_settle_test
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>

#include "../src/psx_lobby_client.c"

uint64_t psx_host_mono_ms(void);
uint64_t psx_host_mono_ms(void) { static uint64_t t; return ++t; }

static int g_failures;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); g_failures++; }
}

/* CRC32s of the real images, so a mistake reads like the thing it models. */
#define CRC_1001 0x37157331u
#define CRC_5501 0x8D8CB7E4u
#define CRC_5502 0x4D9E7C86u
#define CRC_5500 0xFF3EEB8Cu

static void add_image(PsxLobbyBiosOffer *o, const char *stem, uint32_t crc)
{
    if (o->image_count >= PSX_LOBBY_BIOS_IMAGES_MAX) return;
    snprintf(o->images[o->image_count].stem,
             sizeof(o->images[o->image_count].stem), "%s", stem);
    o->images[o->image_count].crc32 = crc;
    o->image_count++;
}

static void set_local(int can_open, int prefer_open, const char *prefer_stem)
{
    memset(&g_lc.bios_offer, 0, sizeof(g_lc.bios_offer));
    g_lc.bios_offer.valid = 1;
    g_lc.bios_offer.can_openbios = can_open;
    g_lc.bios_offer.prefer_openbios = prefer_open;
    if (prefer_stem)
        snprintf(g_lc.bios_offer.prefer_stem,
                 sizeof(g_lc.bios_offer.prefer_stem), "%s", prefer_stem);
}

static PsxLobbyMember *add_member(const char *id, int is_host, int offer_valid)
{
    PsxLobbyMember *m = &g_lc.members[g_lc.member_count++];
    memset(m, 0, sizeof(*m));
    snprintf(m->player_id, sizeof(m->player_id), "%s", id);
    snprintf(m->display_name, sizeof(m->display_name), "%s", id);
    m->bios_offer_valid = offer_valid;
    m->bios_can_openbios = 1;
    if (is_host)
        snprintf(g_lc.host_player_id, sizeof(g_lc.host_player_id), "%s", id);
    return m;
}

static void member_image(PsxLobbyMember *m, const char *stem, uint32_t crc)
{
    if (m->bios_image_count >= PSX_LOBBY_BIOS_IMAGES_MAX) return;
    snprintf(m->bios_images[m->bios_image_count].stem,
             sizeof(m->bios_images[m->bios_image_count].stem), "%s", stem);
    m->bios_images[m->bios_image_count].crc32 = crc;
    m->bios_image_count++;
}

static const char *settle(void)
{
    static char out[PSX_LOBBY_BIOS_STEM_LEN];
    out[0] = '\0';
    psx_lobby_settle_session_bios(out, sizeof(out));
    return out;
}

static void reset(void) { memset(&g_lc, 0, sizeof(g_lc)); }

/* Everyone holds the same single image -> that image. */
static void case_unanimous_single(void)
{
    printf("  unanimous single image\n");
    reset();
    set_local(1, 0, "SCPH5501");
    add_image(&g_lc.bios_offer, "SCPH5501", CRC_5501);
    PsxLobbyMember *h = add_member("h", 1, 1);
    member_image(h, "SCPH5501", CRC_5501);
    snprintf(h->bios_prefer_stem, sizeof(h->bios_prefer_stem), "SCPH5501");
    ck(strcmp(settle(), "scph5501") == 0, "settles on the shared image");
}

/* THE REGRESSION: both can run "retail", but not the same retail. */
static void case_different_retail_is_not_agreement(void)
{
    printf("  different retail images do NOT agree\n");
    reset();
    set_local(1, 0, "SCPH5501");
    add_image(&g_lc.bios_offer, "SCPH5501", CRC_5501);
    PsxLobbyMember *h = add_member("h", 1, 1);
    member_image(h, "SCPH5500", CRC_5500);   /* NTSC-J only */
    snprintf(h->bios_prefer_stem, sizeof(h->bios_prefer_stem), "SCPH5500");
    ck(strcmp(settle(), "openbios") == 0,
       "no common image -> OpenBIOS, never a shared token over different bytes");
}

/* Overlapping sets settle on the common member. */
static void case_intersection(void)
{
    printf("  intersection of overlapping sets\n");
    reset();
    set_local(1, 0, "SCPH1001");
    add_image(&g_lc.bios_offer, "SCPH1001", CRC_1001);
    add_image(&g_lc.bios_offer, "SCPH5502", CRC_5502);
    PsxLobbyMember *h = add_member("h", 1, 1);
    member_image(h, "SCPH5502", CRC_5502);
    snprintf(h->bios_prefer_stem, sizeof(h->bios_prefer_stem), "SCPH5502");
    ck(strcmp(settle(), "scph5502") == 0, "picks the only common image");
}

/* The host's pick wins when it survived the intersection. */
static void case_host_pick_wins(void)
{
    printf("  host preference within the intersection\n");
    reset();
    set_local(1, 0, "SCPH1001");
    add_image(&g_lc.bios_offer, "SCPH1001", CRC_1001);
    add_image(&g_lc.bios_offer, "SCPH5501", CRC_5501);
    PsxLobbyMember *h = add_member("h", 1, 1);
    member_image(h, "SCPH1001", CRC_1001);
    member_image(h, "SCPH5501", CRC_5501);
    snprintf(h->bios_prefer_stem, sizeof(h->bios_prefer_stem), "SCPH5501");
    ck(strcmp(settle(), "scph5501") == 0, "host's choice taken");
}

/* Anyone preferring OpenBIOS takes the room there. */
static void case_openbios_preference(void)
{
    printf("  an OpenBIOS preference wins\n");
    reset();
    set_local(1, 0, "SCPH5501");
    add_image(&g_lc.bios_offer, "SCPH5501", CRC_5501);
    PsxLobbyMember *h = add_member("h", 1, 1);
    member_image(h, "SCPH5501", CRC_5501);
    h->bios_prefer_openbios = 1;
    ck(strcmp(settle(), "openbios") == 0, "explicit OpenBIOS pick respected");
}

/* A peer with no offer (legacy, or not ready) cannot be agreed with. */
static void case_legacy_peer(void)
{
    printf("  a peer with no offer forces OpenBIOS\n");
    reset();
    set_local(1, 0, "SCPH5501");
    add_image(&g_lc.bios_offer, "SCPH5501", CRC_5501);
    add_member("legacy", 1, 0);
    ck(strcmp(settle(), "openbios") == 0,
       "no identity to agree on -> OpenBIOS");
}

/* A v1 peer sends can_scph1001 but no images: retail-incapable here. */
static void case_v1_peer_has_no_images(void)
{
    printf("  a v1 peer advertises no images\n");
    reset();
    set_local(1, 0, "SCPH5501");
    add_image(&g_lc.bios_offer, "SCPH5501", CRC_5501);
    PsxLobbyMember *h = add_member("v1", 1, 1);   /* valid offer, zero images */
    ck(strcmp(settle(), "openbios") == 0,
       "cannot name an image -> cannot agree on one");
}

/* Nobody seated: OpenBIOS. */
static void case_empty_room(void)
{
    printf("  empty room\n");
    reset();
    ck(strcmp(settle(), "openbios") == 0, "no peers -> OpenBIOS");
}

/* The token is always a legal session token. */
static void case_token_shape(void)
{
    printf("  token shape\n");
    reset();
    set_local(1, 0, "SCPH5501");
    add_image(&g_lc.bios_offer, "SCPH5501", CRC_5501);
    PsxLobbyMember *h = add_member("h", 1, 1);
    member_image(h, "SCPH5501", CRC_5501);
    snprintf(h->bios_prefer_stem, sizeof(h->bios_prefer_stem), "SCPH5501");
    ck(lobby_session_bios_token_ok(settle()), "settled token validates");
    ck(lobby_session_bios_token_ok("openbios"), "openbios validates");
    ck(!lobby_session_bios_token_ok("SCPH5501"), "upper case rejected");
    ck(!lobby_session_bios_token_ok(""), "empty rejected");
}

/* The images parser is what turns a peer's frame into identities. */
static void case_parse_images(void)
{
    PsxLobbyBiosImage img[PSX_LOBBY_BIOS_IMAGES_MAX];
    int n;
    printf("  images parser\n");
    n = lobby_parse_bios_images(
        "{\"v\":2,\"images\":[{\"stem\":\"SCPH1001\",\"crc32\":\"37157331\"},"
        "{\"stem\":\"SCPH5502\",\"crc32\":\"4D9E7C86\"}]}",
        img, PSX_LOBBY_BIOS_IMAGES_MAX);
    ck(n == 2, "two images parsed");
    ck(n == 2 && strcmp(img[0].stem, "SCPH1001") == 0, "first stem");
    ck(n == 2 && img[0].crc32 == CRC_1001, "first crc");
    ck(n == 2 && img[1].crc32 == CRC_5502, "second crc");
    n = lobby_parse_bios_images("{\"v\":1,\"can_scph1001\":true}", img,
                                PSX_LOBBY_BIOS_IMAGES_MAX);
    ck(n == 0, "a v1 frame yields no identities");
}

int main(void)
{
    printf("lobby BIOS settle\n");
    case_unanimous_single();
    case_different_retail_is_not_agreement();
    case_intersection();
    case_host_pick_wins();
    case_openbios_preference();
    case_legacy_peer();
    case_v1_peer_has_no_images();
    case_empty_room();
    case_token_shape();
    case_parse_images();
    if (g_failures) { printf("FAILED: %d\n", g_failures); return 1; }
    printf("PASS\n");
    return 0;
}
