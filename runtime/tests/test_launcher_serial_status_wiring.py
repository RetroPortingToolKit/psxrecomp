#!/usr/bin/env python3
"""Guard that the host tells the launcher whether a disc's serial is listed.

A player gave a build made for SLES-03396 a disc of another release,
SLES-03398. The launcher's disc panel showed "Serial  SLES-03398" with a tick,
because it ticked any serial that was read, and the only sentence on the panel
was the online-play note about the TOC fingerprint. He could not tell that he
had the wrong release.

The launcher (recomp-ui, tests/launcher_disc_serial_test.c) now draws a cross
and says "This disc is X. This build needs Y." when the host says the serial
is not listed. This guard is the host's half: ae_disc_verify says so. The
callback needs a disc image and the launcher's window, so it is checked in the
source.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

start = MAIN.index("int ae_disc_verify(const char* disc_path, RecompLauncherCDiscVerify* out) {")
body = MAIN[start : MAIN.index("int ae_memcard_inspect(", start)]

# The fields exist only in a recomp-ui that has them: an older launcher still
# compiles, and gets the old row.
guard = body.index("#if defined(RECOMP_LAUNCHER_HAS_SERIAL_STATUS)")
block = body[guard : body.index("#endif", guard)]

# The host speaks only for a build that lists a serial, and says listed or not
# listed only for a disc it could read. A disc that did not open is the ISO
# header row's to explain.
listed = block.index("if (id.expected_serial_given) {")
readable = block.index("if (id.opened && id.has_header) {", listed)
assert re.search(
    r"out->serial_status = id\.serial_matches \? RECOMP_SERIAL_LISTED\s*: RECOMP_SERIAL_NOT_LISTED;",
    block,
), "the host must say listed or not listed from the identity's own match"

# What the build needs is the serial this disc was checked against: the set's
# per-disc value, or the game's own.
assert re.search(
    r'std::snprintf\(out->expected_serials, sizeof\(out->expected_serials\),\s*"%s", expect_serial\.c_str\(\)\);',
    block,
), "the sentence needs the serial the disc was checked against"

# The row shows what was read from the disc. Before this it fell back to the
# EXPECTED serial when nothing was read, so a disc with no readable serial and
# a file that did not open both showed the right serial with a tick. This is
# outside the "could read it" test: it holds for a file that did not open too.
shown = re.search(
    r'if \(!id\.serial_matches\)\s*std::snprintf\(out->serial, sizeof\(out->serial\), "%s",\s*id\.detected_serial\.c_str\(\)\);',
    block,
)
assert shown, "a disc that does not match must show the serial that was read, not the expected one"
assert listed < shown.start() < readable, "the row's serial must not depend on the disc having opened"
assert block.index("out->serial_status = ") > readable
assert block.index("out->expected_serials") > readable

# A set, and an image its disc list does not know (a copy under another name,
# or another game's disc): no serial is expected for it, so the branch above
# says nothing. It is judged by its serial against every serial of the set
# (disc_roster_judge_serial, run by test_disc_roster.cpp). An image that is in
# the disc list is judged by its own entry, as before. The judgement is made
# outside the launcher's #if: the verdict must not depend on which launcher is
# compiled in.
judged = body.index("const PSXRecompV4::DiscSetSerial set_serial =")
assert judged < guard, "the set judgement must not sit inside the launcher's #if"
judgement = body[judged:guard]
# The two decisions are functions of disc_roster.h, so that test_disc_roster.cpp
# runs what the launcher runs, with the lists a real kit carries. A kit
# built for a single-disc game has a disc list of ONE entry: it takes
# this path for every other file the player selects. Before this judgement
# the launcher ticked another game's disc.
assert re.search(
    r"PSXRecompV4::disc_roster_outside_list\(\s*g_disc_metadata_roster, g_disc_serials,\s*"
    r"std::filesystem::path\(disc_path\), id\.expected_serial_given,\s*"
    r"id\.opened && id\.has_header, id\.detected_serial\);",
    judgement,
), "only an image outside the disc list, that opened, with no expected serial, is judged by the set"
# Wrong disc: what was read is none of the list's serials, and that
# includes a disc on which no serial was found.
assert re.search(
    r"const bool set_wrong_disc = PSXRecompV4::disc_roster_wrong_disc\(set_serial\);",
    judgement,
)
# The expected serial comes from the same header, from the same three lists.
expected = MAIN[MAIN.index("static std::string expected_serial_for_disc("):]
expected = expected[: expected.index("\n}\n")]
assert re.search(
    r"return PSXRecompV4::disc_roster_expected_serial\(\s*"
    r"g_disc_metadata_roster, g_disc_serials, g_program_discs, disc, fallback\);",
    expected,
)
assert re.search(
    r"expected_serial_for_disc\(std::filesystem::path\(disc_path\),\s*g_lnch_expected_serial\)",
    body,
), "the launcher's disc check must take its expected serial from expected_serial_for_disc"

# The row and the sentence: the mark from the judgement, and every serial of
# the set.
set_block = block[block.index("} else if (set_serial != PSXRecompV4::DiscSetSerial::NotJudged) {"):]
assert re.search(
    r"out->serial_status = set_serial == PSXRecompV4::DiscSetSerial::Listed\s*"
    r"\? RECOMP_SERIAL_LISTED : RECOMP_SERIAL_NOT_LISTED;",
    set_block,
)
assert "PSXRecompV4::disc_roster_serial_list(g_disc_serials).c_str()" in set_block, (
    "the sentence for a set names every serial of the set"
)
assert "out->verdict" not in block, "the launcher's #if decides the row and the sentence, never the verdict"

# The verdict: wrong disc for a serial that does not match its entry, and for
# an image outside the set whose serial is none of the set's. Headline "Disc
# verification failed", PLAY greyed, as in a single-disc build. The status is
# set from the same identity, before the verdict: the two cannot disagree.
verdict = re.search(
    r"else if \(\(id\.expected_serial_given && !id\.serial_matches\) \|\|\s*"
    r"set_wrong_disc\)\s*out->verdict = 3; // wrong disc",
    body,
)
assert verdict, "a set must refuse a disc whose serial is none of the set's"
assert guard < verdict.start()
assert body.count("PSXRecompV4::identify_disc(") == 1

# The run report's line for the disc row (launcher_status) says the
# same as the panel. For an image outside the disc list there is no single
# expected serial: the line names the list, and gives the serial as the reason
# when that is why the disc is refused, not the online-play note.
row = body[body.index("const std::string expected_for_row ="):]
row = row[: row.index('psx_start_note_launcher("disc", row);')]
assert re.search(
    r"const std::string expected_for_row = id\.expected_serial_given\s*\? expect_serial\s*"
    r": set_serial != PSXRecompV4::DiscSetSerial::NotJudged\s*"
    r"\? PSXRecompV4::disc_roster_serial_list\(g_disc_serials\)\s*: std::string\(\);",
    row,
)
assert re.search(
    r": \(\(id\.expected_serial_given && !id\.serial_matches\) \|\| set_wrong_disc\)\s*"
    r"\? \"the disc does not carry the expected serial\"\s*: id\.netplay_detail\.c_str\(\);",
    row,
), "a wrong disc outside the list must give the serial as the reason in the report"
assert re.search(r"id\.detected_serial\.c_str\(\), expected_for_row\.c_str\(\),", row)
assert "expect_serial.c_str()" not in row

# The start check (a start that skips the launcher: --disc on the command
# line, a shortcut, a script). It used to warn only: "Disc Image Warning ...
# The runtime will try to run it anyway.", and on a build with a list it did
# not even warn for an image outside the disc list. Now
# a disc the panel refuses for its serial is refused here too, with the
# panel's title and sentence. It holds on a build with a list and on one
# without, and for a disc on which no serial was found.
check = MAIN[MAIN.index("static DiscValidation validate_disc_image("):]
check = check[: check.index("\n}\n")]
assert re.search(
    r"PSXRecompV4::disc_roster_outside_list\(\s*g_disc_metadata_roster, g_disc_serials, selected_path,\s*"
    r"id\.expected_serial_given, id\.opened && id\.has_header, id\.detected_serial\);",
    check,
), "the start check must judge an image outside the disc list as the panel does"
assert "const bool outside_wrong = PSXRecompV4::disc_roster_wrong_disc(outside);" in check
assert "v.id_matches = expect.empty() ? !outside_wrong : id.serial_matches;" in check, (
    "an image with no expected serial matches only when it is not the wrong disc"
)
assert "expect.empty() ? true" not in check
# The sentence is the panel's, made by the header's function: the serial read
# (or none), and the one expected serial or every serial of the list. Only a
# readable disc gets it: one that does not open, or has no header, has its own
# message.
assert re.search(
    r"if \(id\.opened && id\.has_header && !v\.id_matches\)\s*"
    r"v\.wrong_disc = PSXRecompV4::disc_roster_wrong_disc_sentence\(\s*id\.detected_serial,\s*"
    r"expect\.empty\(\) \? PSXRecompV4::disc_roster_serial_list\(g_disc_serials\)\s*"
    r": PSXRecompV4::disc_roster_serial_list\(\{uppercase_ascii\(expect\)\}\)\);",
    check,
)
launch = MAIN[MAIN.index("static bool validate_disc_for_launch("):]
launch = launch[: launch.index("\n}\n")]
# Refused before the warning, and before the disc could be accepted.
refuse = re.search(
    r"if \(!v\.wrong_disc\.empty\(\)\) \{\s*s_wrong_disc_sentence = v\.wrong_disc;\s*"
    r"wrong_disc_notice\(v\.wrong_disc\);\s*return false;\s*\}",
    launch,
)
assert refuse, "a start with the wrong disc must be refused"
assert launch.index("s_wrong_disc_sentence.clear();") < launch.index("validate_disc_image(path, game_id)"), (
    "the sentence must be the last checked disc's, never an earlier one's"
)
warn = launch.index('launcher_warning("Disc Image Warning"')
assert refuse.end() < warn
# What the warning still covers: an image with no ISO header. It starts.
assert "if (!v.has_header) {" in launch[refuse.end():warn]
assert "The runtime will try to run it anyway." in launch
assert "return false" not in launch[warn : launch.index("resolve_disc_path", warn)]
# The box opens for a person only; a scripted start gets the line on stderr.
notice = MAIN[MAIN.index("static void wrong_disc_notice("):]
notice = notice[: notice.index("\n}\n")]
assert '"Disc verification failed"' in notice
assert re.search(r"if \(s_start_interactive && !g_headless\)\s*SDL_ShowSimpleMessageBox\(", notice)
assert notice.count("SDL_ShowSimpleMessageBox(") == 1
assert "++s_player_message_seq;" in notice and "s_player_message_text  = sentence;" in notice, (
    "the notice must count as the start's message, so that the refusal opens no second box"
)
# The run report gets the kind, the title and the sentence.
start = MAIN[MAIN.index("resolved_disc = resolve_disc_for_runtime("):]
start = start[: start.index('return refuse_start("no_disc"')]
assert re.search(
    r"if \(resolved_disc\.empty\(\) && !s_wrong_disc_sentence\.empty\(\)\) \{.*?"
    r"return refuse_start\(\"wrong_disc\", \"Disc verification failed\",\s*"
    r"s_wrong_disc_sentence, disc_messages\);",
    start, re.S,
), "a start refused for the wrong disc must be reported as wrong_disc, before no_disc"
DOC = (ROOT / "docs" / "RUN_REPORT_START.md").read_text(encoding="utf-8")
assert "| `wrong_disc` |" in DOC

print("launcher serial status wiring: ok")
