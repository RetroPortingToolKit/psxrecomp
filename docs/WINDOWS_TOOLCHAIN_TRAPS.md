# Windows toolchain traps

Failure modes specific to building and testing this project on a Windows host. They share one
shape: **a wrapper reports a verdict that disagrees with the thing it wrapped.** A process that
could not start, a decoder that could not decode, or a matcher that found nothing all produce
output that reads like a genuine finding about the code. Every one of these cost real debugging
time before it was recognised.

If a failure here looks like a product defect, check this list before believing it.

## Before you build, test or quote a count: the recipe

Use it in every shell, PowerShell and Git Bash alike. The traps below explain each step.

1. Put the `bin` folder of the toolchain that builds the tools first on `PATH`. Do it before
   you configure, build or test, in the shell that runs them:

       PowerShell   $env:PATH = "<toolchain>\mingw64\bin;$env:PATH"
       Git Bash     export PATH="<toolchain>/mingw64/bin:$PATH"

   `<toolchain>` is the WinLibs UCRT folder, or `C:\msys64` for the MSYS2 MINGW64 packages
   that [`BUILDING.md`](BUILDING.md) installs. The UCRT toolchain of MSYS2 has no `mingw64`
   folder: the folder to put first there is `C:\msys64\ucrt64\bin` (`/c/msys64/ucrt64/bin`
   in Git Bash).
2. Build into a short directory, for example `C:\t\tools`. A long one makes CMake print
   `CMAKE_OBJECT_PATH_MAX` warnings that bury the rest of the configure output.
3. Run `psxrecomp-game --help` from the build directory. It must exit 0 and print usage.
   Run ctest only after that.
4. When a run is red, look for the symptom, a non-zero exit with both streams empty. Do not
   look for one exit code.

**The cause is the order of `PATH`, not the shell.** "Git Bash breaks the test programs, use
PowerShell" is a tempting rule, and it is wrong on the next machine. Git Bash fails because it
puts Git's own `mingw64\bin` first. A native shell fails the same way (exit -1073741511,
nothing on either stream) when another toolchain's `libstdc++-6.dll` comes first. A machine
with Git and a Python distribution early on `PATH` and the build toolchain far down the list
works in PowerShell only because PowerShell does not add Git's folder. With step 1 both shells
work.

## A binary that cannot load reads as a stale binary

`psxrecomp-game` links `libstdc++-6.dll` and `libgcc_s_seh-1.dll`. If a different toolchain's
copy appears earlier on `PATH` than the one it was built against, the image never loads: Windows
exits with an NTSTATUS and the process writes nothing to either stream.

    0xC0000135  STATUS_DLL_NOT_FOUND
    0xC0000139  STATUS_ENTRYPOINT_NOT_FOUND   (DLL found, wrong vintage)
    0xC0000142  STATUS_DLL_INIT_FAILED

**The exit code has more than one spelling, and searching for the wrong one hides half the
cases.** The same binary, the same build, only `PATH` differing:

    correct toolchain first      exit 0, prints usage
    foreign libstdc++-6.dll first, launched from a native shell
                                 exit -1073741511  (0xC0000139 as a signed int)
    the same, launched from Git Bash
                                 exit 127

Git Bash reports it as 127, which reads as "command not found" and sends the reader looking for a
missing file rather than a shadowed DLL. Both spellings print nothing on either stream. Grep the
symptom, not the number: **a non-zero exit with both streams empty.**

Note also what this failure mode *cannot* be: it makes tests fail, never pass. A green suite is
never a symptom of it, so it can invalidate a red run and never a clean one.

A caller checking only "non-zero exit, no output" cannot tell that from a binary too old to
understand the flag it was asked about. `tools/compile_overlays.py` reports a recompiler that
fails `--codegen-hash` this way as one that predates the flag, and advises a rebuild. When the
binary is current, a rebuild cannot help: the launcher's `PATH` is wrong. Before rebuilding, run
`psxrecomp-game --help` in the same shell; if it also exits non-zero with no output, fix `PATH`.

Observed with MSYS2's `libstdc++-6.dll` (GCC 13-era) ahead of a WinLibs GCC 16 build.

**Rule:** a test harness must never put a compiler's `bin` directory *ahead* of the runtime the
recompiler was built against. Pass the compiler by absolute path and **append** its directory:
Windows resolves a spawned image's DLLs from its own directory first, so appending costs the
compile nothing.

## Subprocess output decoded with the host codepage

`subprocess.run(..., text=True)` with no `encoding=` decodes the child's output using the host
ANSI codepage. On any non-Latin-1 Windows host, a byte the codepage cannot map raises
`UnicodeDecodeError` — inside `subprocess`'s reader thread, so the traceback appears without an
obvious owner and the capture is lost mid-build.

Observed with the cp1253 (Greek) codepage: byte `0x9c` in ordinary GCC diagnostics.

**Rule:** always pass `encoding='utf-8', errors='replace'` when capturing subprocess output.
A bare `text=True` is a portability defect, not a style preference. It is invisible on the Linux
CI runners and on English-locale Windows, so it reaches only contributors outside those locales
and survives review indefinitely.

## `MSYS_NO_PATHCONV` is scoped to ref arguments

Under Git Bash, `git show <ref>:<path>` can return empty because the shell rewrites the `<path>`
half of the pair. `MSYS_NO_PATHCONV=1` fixes that, and breaks anything that takes a real path:

    MSYS_NO_PATHCONV=1 git worktree add /c/src/x        # creates C:\c\src\x
    MSYS_NO_PATHCONV=1 cmake -S /c/...                  # "directory does not exist"

**Rule:** scope it to the single `git show` invocation. Never export it across a block that also
passes paths to `cd`, `cmake -S/-B`, or `git worktree add`. Split a command that needs both kinds
of argument.

## ctest counts unbuilt targets as failures

Building a single target and then running the whole suite reports every test whose executable was
never built as `***Not Run`, and counts it toward the failure total — a partial build can read as
a catastrophically broken tree:

    Could not find executable .../control_flow_metadata_test.exe
    1/192 Test #1: control_flow_metadata_test ...***Not Run

**Rule:** before quoting a suite number, build every target and confirm `Not Run: 0`. A run with
non-zero `Not Run` is not a measurement of the tree.

**Second half of the same rule:** also confirm the recompiler actually loads, because a suite
whose spawned binary cannot start reports a plausible pass rate rather than an error. The cheap
gate, before ctest:

    psxrecomp-game --help   # must exit 0 and print usage

Without it, a tree where every test that spawns the recompiler died at image load reported
`84% tests passed, 30 tests failed out of 189` — a number with a percentage, a denominator and
no meaning at all.

Some tests are `DISABLED` deliberately in `recompiler/CMakeLists.txt` and report as
`Not Run (Disabled)`: the three known-failing guards always, and three more when Python is older
than 3.11. Those are expected and are not breakage.

## A hand-rolled hash of CMake-read files is wrong on a CRLF checkout

`PSX_OVERLAY_CODEGEN_HASH` is SHA-256 over the files in
`runtime/codegen_hash_sources.cmake`, concatenated in order, first 8 hex characters
(`runtime/hash_codegen.cmake`). CMake's `file(READ)` **normalises CRLF to LF**. A script that
reads the same files as raw bytes on a CRLF checkout therefore hashes something else — and still
returns eight plausible hex characters that compare cleanly against nothing.

**Rule:** do not hand-roll it. Use the built binary's `psxrecomp-game --codegen-hash`, which is
authoritative, or run `runtime/hash_codegen.cmake` itself in script mode
(`cmake -DOUT=<header> -DPSXRECOMP_CODEGEN_HASH_ROOT=<repo> -P runtime/hash_codegen.cmake`),
which reads the files the way the build does. If you must use another implementation, **run it
first against a tree whose hash is already known**: a control run is the only thing separating
a correct implementation from a confidently wrong one.

## Known-issue notes decay into permanent excuses

A note beside a failing test that names its cause is believed long after anyone checked it. If
the note is wrong, it gives every reader a confident explanation for a failure that is really
one of the traps above, and nobody looks further.

**Rule:** a known-issue note needs the date it was verified and the evidence it rests on. Without
those it becomes a standing excuse for whatever fails next under that test's name. The response
to a red test is never to remove its registration; use ctest's `DISABLED` property, which keeps
it counted and visible.
