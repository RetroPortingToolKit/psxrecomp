# Optional PS1 memory-card manager

`PSX_MEMORY_CARD_MANAGER=ON` offers a pre-boot editor to a compatible recomp-ui.
No title-specific addresses, overlays, game logic or save conversions are involved.
The engine owns parsing, draft documents, icon/title metadata and all file access;
the UI talks to the optional versioned C service in `recomp_card_manager.h`.
Older UI pins compile without advertising the editor. The feature defaults OFF
in the framework. No personal defaults or card paths are installed.

## Formats and behavior

Raw 128 KiB PS1 cards (`.mcd`, `.mcr`, `.mc`, `.srm`, detected by content) and
complete MCS single-save import/export are supported. Wrapped/encrypted card
formats are rejected, not guessed or converted. Validated allocation chains,
directory XOR checksums, overlapping/orphaned blocks, duplicate names and free
space are checked. Bad-sector remaps are displayed read-only. Titles use Windows
CP932 decoding; unknown titles are labeled explicitly, never rewritten. The host
launcher must provide a suitable font for Japanese glyphs.

All edits are drafts. Copy preserves the complete block chain and leaves its
source unchanged. Save writes the current card only after explicit confirmation.
Save As requires a new filename. Open/Refresh/export never create backups.
The editor does not change the game's configured card assignments.

## Persistence boundary

The initial writable adapter supports local fixed NTFS volumes on Windows only.
Other platforms return no editor capability; there is no unsafe fallback.
Before Save/Save As, choose two backup folders in the UI, on distinct NTFS volumes.
Alternatively a host may set `PSX_CARD_BACKUP_PRIMARY` and
`PSX_CARD_BACKUP_SECONDARY`. UI-selected folders apply to that launcher session;
persistent backup-policy settings remain a maintainer decision.

Every changed input version is copied in full to both locations, flushed,
reopened and compared before publication. Content-addressed versions are reused
only after byte verification. Provenance notes are generated locally, not shipped.
Saving checks the original snapshot, rejects links/junctions/read-only files,
locks against data readers/writers, verifies a same-directory temporary image,
and atomically replaces through `FileRenameInfoEx` while the old image remains
open. No truncate, format-missing-card or non-atomic replacement fallback exists.
Interruption can leave a complete `.working-*` file. Backup/locking/replace errors
retain the original and the in-memory draft. Network/removable output is refused.
Close other emulators: an application holding a card only in RAM with no file
handle cannot universally be detected. This editor does not replace the runtime's
ordinary guest-save implementation and is unavailable during an active session.

## Tests (no ROM, BIOS or personal cards)

The standalone test project avoids all runtime submodules and generated game code:

```sh
cmake -S runtime/tests/card_manager -B build-card-tests \
  -DRECOMP_UI_ROOT=/path/to/recomp-ui \
  -DCARD_TEST_PRIMARY=/disposable/first-volume \
  -DCARD_TEST_SECONDARY=/disposable/second-volume
cmake --build build-card-tests
ctest --test-dir build-card-tests --output-on-failure
```

Use new empty disposable paths on two distinct local NTFS volumes. The tests
create only synthetic cards. Parser checks include 2,000 malformed images;
transaction tests cover locks, read-only files, stale snapshots, interrupted
publication, failed replacement, complete copies and Save As conflicts. The C
service test covers the shared UI contract and refusal of unconfirmed saves.

## Provenance and licensing

The parser, transactions, adapter and editor were independently written. No code
from MemcardRex (GPL-3.0), PCSX-Redux's editor (GPL-2.0-or-later), PS1VMC Tool
(GPL-3.0) or current DuckStation (CC-BY-NC-ND-4.0) was imported. They were reviewed
only as interface/format references. The format reference is
[psx-spx](https://psx-spx.consoledev.net/dev/ps1/sio/controllersandmemorycards/memory-card-data-format/).
No game save icons, payloads, fonts from external tools or keys are included.
New engine files use this repository's PolyForm Noncommercial licence; the
independently authored recomp-ui view/contract use that repository's MIT licence.
The UI never includes or copies the engine implementation.
