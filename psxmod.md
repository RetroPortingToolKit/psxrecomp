# Making a `.psxmod` by hand

You can make PSXRecomp mods with a text editor, a ZIP packer, and the usual
tools for editing or understanding game data. No AI service, account, or
generated assets are required. Many mods need no C/C++ and no game rebuild.

The extension is **`.psxmod`** (singular). It is a ZIP with a `manifest.toml`
at its root. The manifest describes the mod, launcher controls, and supported
operations. The game keeps using the player's stock disc; mod operations are
applied in memory or when reading sectors.

Check the target game's pinned framework version. This guide describes
manifest format **10**; an older game release may need an update to read it.

- [Choose the right kind of mod](#choose-the-right-kind-of-mod)
- [First package: your own bezel artwork](#first-package-your-own-bezel-artwork)
- [Make a guarded gameplay patch](#make-a-guarded-gameplay-patch)
- [Add options](#add-options)
- [Replace disc data with an overlay](#replace-disc-data-with-an-overlay)
- [When you need C or C++](#when-you-need-c-or-c)
- [Test and distribute](#test-and-distribute)
- [Troubleshooting](#troubleshooting)

## Choose the right kind of mod

| Goal | Mechanism | Rebuild the game? |
| --- | --- | --- |
| Change known bytes in the main executable | Guarded `[[patch]]` with `target = "main_exe"` | Usually no |
| Change a few known bytes on the disc | Guarded `disc_raw` or `disc_user` patch | Usually no |
| Replace a larger range of asset data | File-backed `[[overlay]]` | Usually no |
| Use an existing host feature | `[[plugin]]` naming an implementation the game already contains | No, if that implementation is present |
| Add new host logic or a recurring gameplay callback | Write and statically link a trusted C/C++ plugin | Yes |

An overlay replaces bytes at a particular location. It is not an automatic
"replace an ISO file by filename" service. Formats, pointers, compression,
and file sizes still matter. A package also cannot load a DLL, compile C, or
invent a plugin implementation by naming it.

A **package** is what you install and version. Its **features** are independent
switches; each feature can have **options**. An operation names its owning
feature and may have conditions on that feature's options. The loader resolves
the whole enabled set, checks compatibility and conflicting byte ranges, and
produces a plan before boot. Runtime guards verify original bytes when the
corresponding executable or disc data is encountered.

## First package: your own bezel artwork

This example uses the existing `psx.bezel` plugin to draw a player-selected
image behind the game frame in OpenGL margins. It requires a game build that
contains that plugin and supports format 10. It does not require a compiler.
You can draw the image yourself with any ordinary image editor.

### 1. Create a source folder

Make a folder named `my-bezel` containing `manifest.toml` and `README.txt`.
Use UTF-8 text and save the manifest with that exact name, not
`manifest.toml.txt`. Python **3.11 or newer** can run all commands in this guide.

Save this complete manifest:

```toml
format_version = 10
id = "yourname.my-bezel"
version = "1.0.0"
name = "My Bezel"
author = "Your Name"
description = "Show my chosen artwork behind the game frame."
license = "MIT"
resolver = "declarative"
save_compatibility = "shared"

[[target]]
game_id = "*"

[[feature]]
id = "bezel"
name = "My Bezel"
description = "Choose an image, then launch with an OpenGL renderer."
group = "Visual"
default_enabled = false

[[resource]]
feature = "bezel"
id = "artwork"
label = "Bezel image"
format = "file"
file_patterns = "*.png,*.jpg,*.jpeg,*.bmp"
file_description = "Image files"
required = false

[[plugin]]
feature = "bezel"
id = "psx.bezel"
```

Here `game_id = "*"` is appropriate because the existing bezel implementation
is a generic presentation feature. Game-specific patches should target exact
game IDs and supported revisions instead.

Use your own package namespace when publishing. Package and feature IDs are
machine identifiers: use lowercase letters, numbers, dots, underscores, and
hyphens. Names and labels are the human-facing text. `format_version` is the
schema version, while `version` is your mod's version. Keep the package ID
stable and increase the version when distributing changes.

The image is a **resource**: the player chooses its path, and the package does
not contain or copy it. A plugin must understand the resource ID; this plugin
specifically looks for `artwork`. With no image selected, it draws nothing.
If you want packaged artwork loaded automatically, you need an implementation
that supports that behavior; adding an image to the ZIP is not sufficient.

### 2. Check and pack it

Put target-build requirements and instructions in `README.txt`. From the
**framework repository root**, run:

```sh
python -c "import pathlib,tomllib; tomllib.loads(pathlib.Path('my-bezel/manifest.toml').read_text(encoding='utf-8')); print('TOML syntax OK')"
python tools/psxmod_pack.py my-bezel dist/my-bezel-1.0.0.psxmod
python -m zipfile -l dist/my-bezel-1.0.0.psxmod
python -m zipfile -t dist/my-bezel-1.0.0.psxmod
```

The packer uses only Python's standard library. It packages files in a stable
order with fixed ZIP timestamps. It checks that a manifest exists, but does
**not** validate the manifest schema or the mod's behavior. Keep its output
outside the source folder: it includes every file under that folder.

The archive must look like this:

```text
my-bezel-1.0.0.psxmod
  manifest.toml
  README.txt
```

A graphical ZIP utility works too: archive the folder's **contents**, use
ordinary ZIP storage or DEFLATE without encryption, and rename `.zip` to
`.psxmod`. The loader accepts up to 4096 entries and 256 MiB expanded. Absolute
paths and `..` paths are rejected.

### 3. Install and try it

Open the target game's launcher and its **Mods** package manager, install the
archive, enable **My Bezel**, select your image, and launch with OpenGL. Use
a window/aspect that leaves margins so there is somewhere to see the artwork.
Disable any other enabled bezel feature: two features claiming `psx.bezel`
conflict instead of taking turns.

Disable your feature and relaunch to verify the normal presentation returns.
Changes generally require a new launch. Management button labels can vary
with the game's pinned recomp-ui version.

## Make a guarded gameplay patch

A guarded patch means "at this location, expect these original bytes, then
replace them with these bytes." The guard catches a wrong revision or a
mistaken address. It is not a substitute for understanding the change.

### 1. Find and verify the change

Use the game's symbols, disassembly, debugger, and existing mod source to find
the relevant code or data. Record:

- The exact game ID, disc revision, and game build.
- The location and whether it is a guest address or a disc-stream offset.
- The original and replacement bytes, with a written explanation.
- When the code/data is loaded and what should change during play.

For example, suppose you have verified a MIPS instruction that initializes a
value to 3: the word `0x24020003` (`addiu v0, zero, 3`). PS1 words are
little-endian, so its bytes are `03 00 02 24`. Changing the immediate to 5
gives `05 00 02 24`. That only means "starting lives" if you have established
that the game uses the result that way.

### 2. Write the package

This is a complete **template**, not a cheat for a particular game. Replace
the target placeholders and the illustrative address with surveyed values.
Do not paste the address into an unrelated title.

```toml
format_version = 10
id = "yourname.starting-lives"
version = "1.0.0"
name = "Starting Lives"
author = "Your Name"
description = "Start a new game with five lives."
resolver = "declarative"
save_compatibility = "shared"

[[target]]
game_id = "REPLACE_WITH_EXACT_GAME_ID"
disc_sha256 = "REPLACE_WITH_STOCK_DISC_SHA256"

[[feature]]
id = "starting-lives"
name = "Starting Lives"
description = "Apply the surveyed new-game lives change."
group = "Gameplay"
default_enabled = false

[[patch]]
feature = "starting-lives"
target = "main_exe"
address = 0x80012340
expected = "03 00 02 24"
replace = "05 00 02 24"
```

`main_exe` uses a **guest virtual address**, not an offset in a BIN, ISO, or
PS-X EXE. These writes are applied after the BIOS loads the main executable
and before its entry point. A runtime variable that is initialized later can
overwrite your write; a streamed code overlay is not automatically part of
the main executable. Choose the operation that matches the actual load path.

For a standard PS-X EXE, the payload begins at file offset `0x800`, and its
load address is the little-endian word at header offset `0x18`. For a location
inside that payload:

```text
guest address = load address + (PS-X EXE file offset - 0x800)
```

This formula does not apply to arbitrary files or runtime-relocated overlays.
Confirm the address in a debugger. Guard a complete instruction when changing
an instruction, keep ordinary replacements equal in length to `expected`, and
account for MIPS delay slots and register use when editing logic.

### 3. Get the right hashes

Copy known target identities from the game's manifests and source where
possible. `exe_sha256`, when used, hashes the loose **PS-X EXE file**, not the
native PC executable; some releases do not carry that loose file, so requiring
it can make an otherwise correct package incompatible with those releases.

For a typical BIN/CUE, `disc_sha256` hashes the resolved BIN data track,
**not the CUE text**. For CHD, the current runtime decodes the disc and hashes
its complete sequence of raw 2352-byte sectors; hashing the compressed CHD
file will give the wrong identity. Check the target title's pinned
`mod_runtime_commit()` and disc-resolution code, especially for multiple
tracks and older releases. Do not guess a hash from the disc's display name.

For a BIN data file whose identity you have confirmed, this command prints its
hash without loading the whole disc into memory:

```sh
python -c "import hashlib; f=open('game.bin','rb'); print(hashlib.file_digest(f,'sha256').hexdigest()); f.close()"
```

Use lowercase 64-character hashes. Additional `[[target]]` rows are
alternatives, not a way to translate addresses across revisions. Separate
packages or implementations may be needed if the actual byte locations differ.

## Add options

To make the example's value configurable, add this option to that package:

```toml
[[option]]
feature = "starting-lives"
id = "count"
label = "Starting lives"
type = "integer"
min = 1
max = 9
step = 1
default = 3
```

Then **replace**, rather than add to, its original `[[patch]]`:

```toml
[[patch]]
feature = "starting-lives"
target = "main_exe"
address = 0x80012340
expected = "03 00 02 24"
replace_from = { option = "count", encoding = "u16le", offset = 0 }
```

The loader copies the expected bytes and fills the selected field with the
option value. `u8`, `u16le`, and `u32le` encode unsigned values. The declared
range must fit the encoding; the loader will not infer signed arithmetic or
arbitrary expressions. The linked schema guide covers split MIPS immediates
and sparse field ownership for more complex patches.

Boolean and choice options can select operations with feature-local
conditions. For example, after declaring a `style` choice on this feature
with values `stock` and `extra`, a patch may contain:

```toml
when = { style = "extra" }
```

All named conditions must match. Declare choice values with `[[option.choice]]`
entries immediately after their owning option; `default` must name one of
those values. Options alone do not change the game: patches consume them, or
a trusted plugin must query and implement them.

## Replace disc data with an overlay

Use `[[overlay]]` for a larger asset range stored in a separate package file.
Find the original asset and its format with ordinary disc/file tools, edit it
by hand, and export the replacement bytes. Preserve the game's expected
layout or explicitly implement the additional pointer/size changes needed.

The two disc coordinate systems are:

| Target | Offset calculation | Typical use |
| --- | --- | --- |
| `disc_raw` | `LBA * 2352 + byte within raw sector` | Raw-sector-aware changes, including formats outside a 2048-byte data payload |
| `disc_user` | `LBA * 2048 + byte within user data` | Logical 2048-byte data sectors |

LBA is the logical sector number in the runtime's disc reader. A file-relative
offset is not an LBA. Match the runtime's track mapping; do not treat a
multi-track CUE as one flat file. A raw 2352-byte sector contains headers and
other bytes besides its data payload. The runtime maps Mode 2 Form 1 raw data
to its 2048-byte payload; `disc_user` is not the mechanism for XA/Form 2 audio
or CD audio sectors.

Small disc `[[patch]]` writes use `offset` instead of `address` and cannot
cross a sector boundary. File-backed overlays may span sectors. For example,
add this fragment to a package that already declares a feature named `art`:

```toml
[[overlay]]
feature = "art"
target = "disc_user"
offset = 204800
file = "assets/replacement.bin"
sha256 = "REPLACE_WITH_PAYLOAD_SHA256"
expected_sha256 = "REPLACE_WITH_ORIGINAL_RANGE_SHA256"
```

The illustrative offset is LBA 100, byte 0. Use your own verified location.
`file` is relative to the package root. Its length determines the replacement
range; it does not insert space into the disc. Hash the replacement file for
`sha256`. The optional `expected_sha256` hashes the **same-length original
range in the selected target stream**, not the whole disc or raw sectors
when the target is `disc_user`.

Every target row must declare an exact `disc_sha256` when using feature disc
overlays. Include the payload in the archive, update its hash whenever it
changes, and pack using the same command as the first example. The loader
checks payloads during installation/scanning and checks enabled payloads again
during resolution.

To adapt an existing disc mod, inspect and understand its changes, then
reimplement the behavior as guarded operations, overlays, or a trusted plugin
against the stock disc. Do not just rename an IPS/PPF/VCDIFF to `.psxmod`.
The legacy `derived_disc` path is not supported for new feature-style mods.

## When you need C or C++

Use a trusted plugin for new host behavior or a change that cannot be
expressed by the declarative operations. It lives in the **game repository**,
is reviewed and compiled into that game, and is selected by a manifest ID.

A minimal implementation in `src/hello_mod.c` looks like this:

```c
#include <stdio.h>
#include "mod_plugins.h"

static void hello_activate(void) {
    fprintf(stdout, "My PSX mod is active.\n");
    fflush(stdout);
}

PSX_MOD_CONSTRUCTOR(register_hello_mod) {
    (void)psx_mod_register_activation_plugin(
        "yourname.hello.startup", hello_activate);
}
```

Add it with `target_sources(MyGame PRIVATE src/hello_mod.c)`, using the game's
actual target name, and follow that repository's build instructions. A package
with the normal metadata, target, and a feature named `hello` selects it with:

```toml
[[plugin]]
feature = "hello"
id = "yourname.hello.startup"
```

The plugin ID is a registry key, not a filename or exported symbol. Shipping
that C file inside a package does not install the implementation. Players
need the game build containing it.

Activation callbacks run before renderer/window initialization. Deterministic
guest-VBlank callbacks can be registered with
`psx_mod_register_vblank_plugin()`; gate gameplay work until
`psx_mod_game_started()`. The narrow guest read/write services and option and
resource queries are in [`mod_plugins.h`](runtime/include/mod_plugins.h).
For guest instructions, use `psx_mod_write_code_word()` rather than an ordinary
data write so executable-RAM handling sees the change. Test actual execution
and save-state restoration; compilation alone cannot establish correctness.

For assets or settings, read the committed package/feature option or resource
instead of opening a player's `state.toml` yourself. Register no unconditional
gameplay effects merely because the source file is linked. Disabling the
feature must restore stock behavior on a fresh launch.

## Test and distribute

Use a separate game installation and fresh mod selections, with normal saves
backed up. Test the **packed archive**, including its actual assets.

- Verify the intended game revision and build, feature on/off behavior, and
  each option including range limits.
- Exercise what changed: new-game initialization, relevant stages or menus,
  transitions, death/retry, audio, and save/load as appropriate.
- Test wrong original bytes and missing/changed assets. A rejected guard is
  evidence to investigate, not a reason to remove the guard.
- Test relevant mod combinations. Different writes to the same owned bytes
  fail rather than letting a load order choose a winner. Match the reported
  package, feature, target, and range to the source operations.
- Validate netplay in actual supported sessions before claiming it. Matching
  compilation or version labels alone does not prove compatible simulation.

Use `channel = "experimental"` on an unvalidated feature; it ships with a
badge and defaults off. `channel = "developer"` is for local development and
is unavailable in release builds. Features can declare their own channel.
Elective enhancements should be opt-in; engine correctness fixes
belong in the normal implementation.

Repository release policy excludes **CD Speed**
(`psx.enhancement.cd-speed`) and **Fast Loading / host pacing**
(`psx.enhancement.fast-loading`) entirely from game releases. Do not ship
their packages or controls. Development catalogs keep both hidden and off.

Installed and bundled packages have different owners:

```text
<game executable directory>/mods/
  bundled/<package-id>/<version>/    staged build output; rebuilt by the game
  installed/<package-id>/<version>/  player packages installed by the launcher
  state.toml                        player selections
```

Keep your editable sources outside both staged and installed trees. Game
maintainers put bundled sources under
`mods/preloaded/packages/<package-id>/<version>/` and pass the catalog through
`PRELOADED_MODS_DIR` when creating the runtime target. The framework stages it.

Distribute the `.psxmod` with source, license/credits, supported game IDs and
hashes, minimum tested build, known limits, and a short changelog. State when
a custom executable is needed. The package's license covers only work you can
license; include only assets/code you have permission to redistribute. Stock
disc/BIOS images, personal saves, and `state.toml` do not belong in the ZIP.
`save_compatibility` metadata is not a promise that a gameplay change can load
every existing save; validate that behavior explicitly.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| No root manifest | Archive the contents, not an extra parent folder. |
| Unsupported format version | The game's pinned framework may predate format 10. Update the game or deliberately use only fields its older schema supports. |
| Package absent / manifest ignored | Read the game's scan error for the path and parsing reason. Syntax validation alone is not schema validation. |
| Target incompatible | Match exact game ID and actual runtime disc/EXE hash inputs. |
| Expected-byte guard failed | Recheck revision, load timing, byte order, and address/offset coordinates. |
| Overlay payload checksum failed | Pack the right file and update its lowercase SHA-256. |
| Missing trusted plugin | Use a game build containing the implementation. An ID alone cannot add code. |
| Conflicting features | Inspect the reported owned range or plugin ID and disable one; ZIP order is not patch priority. |
| Bezel enabled but invisible | Select an image, use OpenGL, and leave visible margins. |
| Package version already installed | Bump `version` or remove your prior test version through the manager. |
| Changes vanish after rebuilding | Edit source catalogs, not the build's `mods/bundled` output. |

See [`docs/MOD_PACKAGES.md`](docs/MOD_PACKAGES.md) for advanced conditions,
sparse fields, dependencies, and resolution rules; the implementations are
[`mod_packages.cpp`](runtime/src/mod_packages.cpp) and
[`mod_runtime.cpp`](runtime/src/mod_runtime.cpp).
