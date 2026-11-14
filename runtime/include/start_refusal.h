/* start_refusal.h — why a start ended before the first frame.
 *
 * A start that is refused (a BIOS this build was not compiled from, no disc,
 * an image the drive cannot mount) returns from main() before any guest code
 * runs. The run report then read reason "atexit", exit_origin "unknown",
 * frame 0 for every one of those causes, and for a closed launcher window
 * too, so a player's report could not say why the game did not start.
 *
 * This module keeps the kind of the refusal and the sentence the player was
 * shown. crash_trace.c prints both into psx_last_run_report.json. It also
 * keeps what the launcher's BIOS and disc rows said last, because a launcher
 * that blocks Play ends as "launcher closed" with the reason on those rows.
 *
 * The report names a file by its base name, never by its folder: players
 * paste that file into a chat, and it held no path before. The box, which
 * only the player sees, may show the full path. See docs/RUN_REPORT_START.md.
 *
 * Fixed buffers, no allocation: the report writer also runs on the crash path.
 */
#ifndef PSXRECOMP_START_REFUSAL_H
#define PSXRECOMP_START_REFUSAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_START_REFUSAL_KIND_CAP  48
#define PSX_START_REFUSAL_TITLE_CAP 96
#define PSX_START_REFUSAL_TEXT_CAP  1024
#define PSX_START_LAUNCHER_ROW_CAP  320

/* Record that this start is refused. `kind` is a short stable word for tools
 * ("no_bios", "disc_not_mounted"); `title` and `message` are what the player
 * was shown. Every full path in them is cut to its base name, and longer text
 * is cut at the cap. The first refusal of a start is kept: a later exit path
 * must not replace the reason. */
void psx_start_refusal_set(const char *kind, const char *title, const char *message);

/* The kind of the recorded refusal, or NULL when the start was not refused. */
const char *psx_start_refusal_kind(void);

/* What a launcher row said last. `row` is "bios" or "disc"; other names are
 * ignored. An empty text clears the row. Full paths are cut as above. */
void psx_start_note_launcher(const char *row, const char *text);

/* The report values, as JSON text: `null`, or an object.
 *   start_refused:   {"kind": "...", "title": "...", "message": "..."}
 *   launcher_status: {"bios": "...", "disc": "..."}
 * Each writes at most cap - 1 characters and a terminator, and returns the
 * length written. A buffer too small for the value gets `null`. The text is
 * valid UTF-8 whatever the bytes of a file name were: a byte that is not part
 * of a UTF-8 sequence is written as \u00XX. */
int psx_start_refusal_json(char *out, size_t cap);
int psx_start_launcher_status_json(char *out, size_t cap);

/* Nonfatal mounted-disc warning, retained after the first guest instruction.
 * Set an empty string on a warning-free mount. It is separate from refusals. */
void psx_disc_warning_set(const char *message);
int psx_disc_warning_json(char *out, size_t cap);

/* `src` with every full path cut to its base name (what the report stores).
 * A path starts at a drive ("C:\"), a share ("\\server") or a rooted path of
 * two parts or more, and runs to the end of its line. */
void psx_start_refusal_without_folders(char *dst, size_t cap, const char *src);

/* Forget the refusal and launcher rows; a disc warning stays. The runtime
 * calls it when the game starts to run, so a start that ran reports `null`
 * for both values; tests call it between cases. */
void psx_start_refusal_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_START_REFUSAL_H */
