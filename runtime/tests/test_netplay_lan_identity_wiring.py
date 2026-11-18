#!/usr/bin/env python3
"""Guard that the LAN / Direct IP lobby checks a guest's build and game.

test_netplay_lan_version.c proves the two rules. This proves main.cpp applies
them: the JOIN carries both lines, both host JOIN handlers refuse before they
seat anyone, the guest shows the reason, and the same-machine room file is
offered only to the title that wrote it. The lobby code runs inside the
launcher and needs sockets and a second peer, so the wiring is checked in the
source, as test_launcher_pad_mode_wiring.py does for the launcher.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")


def body(signature: str) -> str:
    """The text of the function that starts at `signature`, to its closing brace."""
    start = MAIN.index(signature)
    depth = 0
    for i in range(MAIN.index("{", start), len(MAIN)):
        if MAIN[i] == "{":
            depth += 1
        elif MAIN[i] == "}":
            depth -= 1
            if depth == 0:
                return MAIN[start : i + 1]
    raise AssertionError(f"unterminated body: {signature}")


def block_after(marker: str) -> str:
    """The braced block that follows the first `marker` (an if condition)."""
    return body(marker)


# The guest's JOIN: version on line 7, game name on line 8, after the BIOS CRC.
join = body("static void ae_np_append_lan_bios_join(char* msg, size_t msg_cap, int* io_off) {")
crc = join.index("netplay_bios_format_crc(")
version = join.index("psx_lobby_game_version()")
game = join.index("ae_np_lan_game_name()")
assert crc < version < game, "JOIN tail order must be CRC, version, game name"

# The host's check: build first, then game, each with its own refusal.
check = body("static bool ae_np_lan_join_identity_ok(const char* tail, const sockaddr_in& from) {")
for needle in (
    "netplay_lan_join_version(tail,",
    "netplay_lan_version_ok(psx_lobby_game_version(),",
    '"MOTK1 ERR\\nversion_mismatch\\n"',
    "NETPLAY_LAN_VERSION_HOST_TEXT",
    "netplay_lan_join_game(tail,",
    "netplay_lan_game_ok(",
    '"MOTK1 ERR\\ngame_mismatch\\n"',
    "NETPLAY_LAN_GAME_HOST_TEXT",
):
    assert needle in check, f"the host's JOIN check lost: {needle}"
assert check.index("netplay_lan_version_ok(") < check.index("netplay_lan_game_ok("), (
    "the build is checked before the game, so an older guest is told about its build"
)
assert "ae_np_lan_game_name()" in check, "the host must compare with its own game name"

# Both JOIN handlers refuse before they seat the guest.
for marker in (
    'if (std::strncmp(buf, "MOTK3 JOIN\\n", 11) == 0 && g_lnch_hosting_lan) {',
    'if (std::strncmp(buf, "MOTK1 JOIN\\n", 11) == 0 && g_lnch_hosting_lan) {',
):
    handler = block_after(marker)
    assert "ae_np_lan_join_identity_ok(" in handler, f"no build and game check in: {marker}"
    assert handler.index("ae_np_lan_join_identity_ok(") < handler.index("ae_np_lan_seat_guest("), (
        f"the guest is seated before it is checked in: {marker}"
    )
    assert "bad_password" in handler and handler.index("bad_password") < handler.index(
        "ae_np_lan_join_identity_ok("
    ), "a wrong password must still be answered first"
# A legacy JOIN has no tail at all, so the check gets none.
assert "ae_np_lan_join_identity_ok(nullptr, from)" in block_after(
    'if (std::strncmp(buf, "MOTK1 JOIN\\n", 11) == 0 && g_lnch_hosting_lan) {'
)

# The guest shows the reason for either refusal, on the first JOIN and in a room.
wait = body("static int ae_np_lan_wait_join_ack(")
for needle in ('"version_mismatch"', "NETPLAY_LAN_VERSION_GUEST_TEXT", '"game_mismatch"', "NETPLAY_LAN_GAME_GUEST_TEXT"):
    assert needle in wait, f"the first JOIN's error handling lost: {needle}"
assert MAIN.count("NETPLAY_LAN_GAME_GUEST_TEXT") >= 2, "the in-room ERR handler must name a game mismatch too"
assert MAIN.count("NETPLAY_LAN_VERSION_GUEST_TEXT") >= 2

# The same-machine room file is offered only to the title that wrote it.
reader = body("static bool ae_np_read_lan_file_state(AeLanLobbyState* state) {")
assert "netplay_lan_game_ok(ae_np_lan_game_name().c_str(), state->game.c_str())" in reader, (
    "the room file must be refused when its recorded game is another title"
)
assert "state.game = ae_np_lan_game_name();" in MAIN, "the room file must record this title's game name"

# One name on the wire and in the file: no control characters, at most a lobby name.
name = body("static std::string ae_np_lan_game_name(void) {")
assert "g_lnch_netplay_game_name" in name and "PSX_LOBBY_NAME_LEN" in name

print("netplay lan identity wiring test: PASS")
