/* Why a start ended before the first frame, as the run report prints it
 *. Plain checks, not assert(): Release test builds define NDEBUG. */
#include "start_refusal.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    ++failures; } } while (0)

/* 1 when every byte of s is part of a well-formed UTF-8 sequence. */
static int is_utf8(const char *text)
{
    const unsigned char *s = (const unsigned char *)text;
    while (*s) {
        int n = *s < 0x80 ? 1 : (*s >= 0xC2 && *s <= 0xDF) ? 2 :
                (*s >= 0xE0 && *s <= 0xEF) ? 3 : (*s >= 0xF0 && *s <= 0xF4) ? 4 : 0;
        int i;
        if (!n) return 0;
        for (i = 1; i < n; i++)
            if ((s[i] & 0xC0) != 0x80) return 0;
        s += n;
    }
    return 1;
}

static const char *cut(const char *text)
{
    static char out[512];
    psx_start_refusal_without_folders(out, sizeof(out), text);
    return out;
}

int main(void)
{
    char out[8192];
    int n;

    /* A start that ran: nothing recorded, and the report gets `null`. */
    psx_start_refusal_reset();
    CHECK(psx_start_refusal_kind() == NULL);
    n = psx_start_refusal_json(out, sizeof(out));
    CHECK(n == 4 && strcmp(out, "null") == 0);
    n = psx_start_launcher_status_json(out, sizeof(out));
    CHECK(n == 4 && strcmp(out, "null") == 0);

    /* A refusal: the kind and the sentence the player was shown. A line break
     * in the sentence is \n in the report, a quote and a backslash are
     * escaped, and any other control character is a space. */
    psx_start_refusal_set("no_bios", "BIOS Mismatch",
        "That BIOS (524288 bytes, CRC32 318178BF) is not an image this build "
        "was compiled from.\r\n\nIt says \"no\" \\ and a\ttab");
    CHECK(psx_start_refusal_kind() != NULL &&
          strcmp(psx_start_refusal_kind(), "no_bios") == 0);
    n = psx_start_refusal_json(out, sizeof(out));
    CHECK(n == (int)strlen(out));
    CHECK(strcmp(out,
        "{\"kind\": \"no_bios\", \"title\": \"BIOS Mismatch\", \"message\": "
        "\"That BIOS (524288 bytes, CRC32 318178BF) is not an image this build "
        "was compiled from.\\n\\nIt says \\\"no\\\" \\\\ and a tab\"}") == 0);

    /* The first refusal of a start is kept: a later exit path must not
     * replace the reason. */
    psx_start_refusal_set("video_init", "Game window could not be opened", "later");
    CHECK(strcmp(psx_start_refusal_kind(), "no_bios") == 0);

    /* No kind, no refusal. */
    psx_start_refusal_reset();
    psx_start_refusal_set(NULL, "t", "m");
    psx_start_refusal_set("", "t", "m");
    CHECK(psx_start_refusal_kind() == NULL);

    /* A missing title or sentence is an empty string, not a crash. */
    psx_start_refusal_set("no_disc", NULL, NULL);
    psx_start_refusal_json(out, sizeof(out));
    CHECK(strcmp(out, "{\"kind\": \"no_disc\", \"title\": \"\", \"message\": \"\"}") == 0);

    /* The report names a file by its base name, never by its folder: players
     * paste the report into a chat. A full path runs to the end of its line. */
    CHECK(strcmp(cut("Selected path:\nC:\\Users\\Jos\xC3\xA9\\PS1 Games\\Diablo (Europe).cue"),
                 "Selected path:\nDiablo (Europe).cue") == 0);
    CHECK(strcmp(cut("could not be read.\n\nD:/test dir/recomps/game.toml\n\nbad key"),
                 "could not be read.\n\ngame.toml\n\nbad key") == 0);
    CHECK(strcmp(cut("Supply the file at /home/alex/PS1 Games/Game.sbi, then retry."),
                 "Supply the file at Game.sbi, then retry.") == 0);
    CHECK(strcmp(cut("\\\\nas\\share\\bios\\SCPH-5552.BIN"), "SCPH-5552.BIN") == 0);
    CHECK(strcmp(cut("Mounted as:\nZ:\\Emulators\\"), "Mounted as:\n") == 0);
    /* What is not a full path stays: a relative path, a web address, a lone
     * slash, a drive letter with no separator, a ratio. */
    CHECK(strcmp(cut("Expected next to the executable:\nbios/SCPH5552.BIN"),
                 "Expected next to the executable:\nbios/SCPH5552.BIN") == 0);
    CHECK(strcmp(cut("See https://example.org/a/b for help"),
                 "See https://example.org/a/b for help") == 0);
    CHECK(strcmp(cut("a .cue / .bin pair, 4:3, C: is full, and/or"),
                 "a .cue / .bin pair, 4:3, C: is full, and/or") == 0);
    CHECK(strcmp(cut(""), "") == 0);
    CHECK(strcmp(cut(NULL), "") == 0);
    /* The stored refusal and the launcher rows are cut the same way. */
    psx_start_refusal_reset();
    psx_start_refusal_set("disc_not_mounted", "Disc Could Not Be Mounted",
        "The drive could not mount it.\n\nSelected:\nC:\\Games\\PS1\\Wipeout (Europe).cue");
    psx_start_note_launcher("bios", "BIOS file not found: D:\\bios\\x.bin");
    psx_start_refusal_json(out, sizeof(out));
    CHECK(strstr(out, "Selected:\\nWipeout (Europe).cue\"}") != NULL);
    CHECK(strstr(out, "Games") == NULL && strstr(out, "C:") == NULL);
    psx_start_launcher_status_json(out, sizeof(out));
    CHECK(strcmp(out, "{\"bios\": \"BIOS file not found: x.bin\", \"disc\": \"\"}") == 0);

    /* The report is valid UTF-8 whatever the bytes of a file name are. Text
     * that is UTF-8 stays as it is; a byte that is not part of a UTF-8
     * sequence (a Windows name in the system code page, here Latin-1 e-acute
     * and a cut sequence) is written as \u00XX. */
    psx_start_refusal_reset();
    psx_start_refusal_set("no_disc", "Disc Image Not Found",
        "Jos\xC3\xA9 \xCE\xB1\xCE\xB2 \xE2\x82\xAC \xF0\x9F\x8E\xAE | Jos\xE9.cue | cut \xE2\x82 | lone \x80 | over \xC0\xAF");
    n = psx_start_refusal_json(out, sizeof(out));
    CHECK(n == (int)strlen(out));
    CHECK(is_utf8(out));
    CHECK(strstr(out, "Jos\xC3\xA9 \xCE\xB1\xCE\xB2 \xE2\x82\xAC \xF0\x9F\x8E\xAE | ") != NULL);
    CHECK(strstr(out, "Jos\\u00e9.cue | cut \\u00e2\\u0082 | lone \\u0080 | over \\u00c0\\u00af\"}") != NULL);
    psx_start_note_launcher("disc", "verdict=refused; \xFF");
    psx_start_launcher_status_json(out, sizeof(out));
    CHECK(is_utf8(out) && strstr(out, "\\u00ff") != NULL);

    /* A sentence longer than the cap is cut, and the value is still a
     * complete JSON object, also when every byte needs six characters. */
    {
        static char longtext[4 * PSX_START_REFUSAL_TEXT_CAP];
        memset(longtext, 'a', sizeof(longtext) - 1);
        longtext[sizeof(longtext) - 1] = '\0';
        psx_start_refusal_reset();
        psx_start_refusal_set("config_unreadable", "Configuration could not be read", longtext);
        n = psx_start_refusal_json(out, sizeof(out));
        CHECK(n > PSX_START_REFUSAL_TEXT_CAP - 8 && n < PSX_START_REFUSAL_TEXT_CAP + 160);
        CHECK(out[0] == '{' && out[n - 1] == '}' && out[n - 2] == '"');
        memset(longtext, 0xE9, sizeof(longtext) - 1);
        psx_start_refusal_reset();
        psx_start_refusal_set("no_disc", "t", longtext);
        n = psx_start_refusal_json(out, sizeof(out));
        CHECK(n > 6 * (PSX_START_REFUSAL_TEXT_CAP - 8) && n < (int)sizeof(out));
        CHECK(is_utf8(out) && out[0] == '{' && out[n - 1] == '}');
    }

    /* A buffer too small for the object gets `null`, never half an object. */
    {
        char small[24];
        n = psx_start_refusal_json(small, sizeof(small));
        CHECK(n == 4 && strcmp(small, "null") == 0);
        CHECK(psx_start_refusal_json(small, 0) == 0);
        CHECK(psx_start_refusal_json(NULL, 16) == 0);
    }

    /* The launcher's rows: the last text of each, and only those two rows. */
    psx_start_refusal_reset();
    psx_start_note_launcher("bios", "CRC32 318178BF (this build expects SCPH-5552, CRC32 D786F0B9).");
    psx_start_note_launcher("disc", "first");
    psx_start_note_launcher("disc", "verdict=refused serial=SLES-02913 expected=SLES-01156 tracks=4");
    psx_start_note_launcher("memcard", "ignored");
    psx_start_note_launcher(NULL, "ignored");
    n = psx_start_launcher_status_json(out, sizeof(out));
    CHECK(n == (int)strlen(out));
    CHECK(strcmp(out,
        "{\"bios\": \"CRC32 318178BF (this build expects SCPH-5552, CRC32 D786F0B9).\", "
        "\"disc\": \"verdict=refused serial=SLES-02913 expected=SLES-01156 tracks=4\"}") == 0);
    /* The rows do not make a start refused. */
    CHECK(psx_start_refusal_kind() == NULL);
    /* One row alone is still reported; an empty text clears a row. */
    psx_start_note_launcher("bios", "");
    psx_start_launcher_status_json(out, sizeof(out));
    CHECK(strncmp(out, "{\"bios\": \"\", \"disc\": \"verdict=refused", 36) == 0);
    psx_start_note_launcher("disc", NULL);
    psx_start_launcher_status_json(out, sizeof(out));
    CHECK(strcmp(out, "null") == 0);

    /* The game runs: the runtime forgets the rows, so a start that ran
     * reports `null` for both values. */
    psx_start_note_launcher("bios", "SCPH5552.BIN (CRC OK).");
    psx_start_note_launcher("disc", "verdict=ok serial=SLES-01156 expected=SLES-01156 tracks=1");
    psx_start_refusal_reset();
    psx_start_refusal_json(out, sizeof(out));
    CHECK(strcmp(out, "null") == 0);
    psx_start_launcher_status_json(out, sizeof(out));
    CHECK(strcmp(out, "null") == 0);

    if (failures) {
        fprintf(stderr, "start refusal: %d check(s) failed\n", failures);
        return 1;
    }
    printf("start refusal: ok\n");
    return 0;
}
