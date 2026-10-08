# Third-Party Attribution

## OpenBIOS — PCSX-Redux's free PS1 BIOS

[OpenBIOS](https://github.com/grumpycoders/pcsx-redux) (src/mips/openbios) by
the PCSX-Redux authors, licensed **MIT** (notice: `bios/OpenBIOS.LICENSE`;
the binary also links permissively-licensed code from
[uC-sdk](https://github.com/grumpycoders/uC-sdk), noted there too). Vendored
as the prebuilt image `bios/openbios.bin` (pin and build recipe recorded in
`bios/OpenBIOS.toml`) and statically recompiled by
`psxrecomp-bios --config bios/OpenBIOS.toml` exactly like the retail BIOS.
Normal runtime builds stage it automatically so players supply only a disc;
`bios/OpenBIOS.LICENSE` always rides alongside the shipped image.


## libchdr — CHD disc-image decompressor

[libchdr](https://github.com/rtissera/libchdr) by Romain Tisserand and
contributors, licensed **BSD-3-Clause** (notice: `LICENSE.txt` inside the
archive). Vendored as the pinned source archive
`third_party/libchdr-<commit>.tar.gz`; the commit, digest, and upstream URL are
recorded in `third_party/deps.manifest`, and `runtime/chd_dependency.cmake`
verifies the archive against that digest before building it. It is compiled
into the runtime as a static library, so the BSD notice must ship with any
binary that links it: the notice text is `runtime/licenses/libchdr-NOTICES.txt`
and the release packagers copy `runtime/licenses/` into the package as
`licenses/`.

The archive also carries libchdr's own bundled decompressors — Zstandard
(BSD-3-Clause / GPL-2.0 dual), LZMA SDK (public domain), and miniz (MIT) —
built from the same pinned tree; `WITH_SYSTEM_ZLIB`/`WITH_SYSTEM_ZSTD` are
forced OFF so the disc decoder cannot change with the host's packages.

## libjuice — ICE transport for netplay

[libjuice](https://github.com/paullouisageneau/libjuice) by Paul-Louis Ageneau
and contributors, licensed **MPL-2.0**. Only netplay builds link it
(`PSX_NETPLAY=ON` with recomp-net's `RNET_ENABLE_ICE`, the default); single-player
builds do not. Vendored unmodified as `third_party/libjuice-v1.7.2.tar.gz`, the
same URL and SHA-256 recomp-net pins; `runtime/netplay_dependency.cmake` declares
it for recomp-net and stops the configure if the two pins ever differ. It is
compiled in statically, so its notice and the source location ship with any
netplay binary: `runtime/licenses/libjuice-NOTICES.txt`. MPL-2.0 is file-level
copyleft: a change to a libjuice file must be published under MPL-2.0. The build
changes none.

## libwebp - static texture-image decoder

[libwebp 1.6.0](https://chromium.googlesource.com/webm/libwebp/+/refs/tags/v1.6.0)
by Google Inc. and WebM contributors is licensed **BSD-3-Clause**. The runtime
builds only its static decoder, without command-line tools, the encoder, or a
libwebp DLL. The dependency is unmodified and pinned to commit
`4fa21912338357f89e4fd51cf2368325b59e9bd9`, with immutable archive SHA-256
`923f3382a47a2af185c3240c954cf004428b237bd7317413a95146d01eb4b94b` in
`third_party/deps.manifest`. Its original COPYING, PATENTS, and AUTHORS texts
ship in `runtime/licenses/libwebp-NOTICES.txt`, which release packagers carry
into `licenses/`. Developers can use the verified archive or an explicit
`FETCHCONTENT_SOURCE_DIR_PSX_LIBWEBP` / `PSX_LIBWEBP_SOURCE_DIR` source override.
PNG and JPEG continue to use the existing shared stb_image implementation.

## xBR — edge-directed texture filter (Hyllian)

The `texture_filtering = "xbr"` mode in `runtime/src/gpu_gl_renderer.c`
(`xbr_d`, `xbr_sample`) follows Hyllian's xBR algorithm: its 5x5 neighbourhood,
YUV-weighted colour distance and the `4*d(H,F)` / `4*d(E,I)` edge sums. It is
an adaptation (CLUT texel fetches, primitive UV clamping, an anti-aliased corner
cut), not a copy of a shader file, but it derives from that work, licensed MIT:

```
Copyright (C) 2011-2016 Hyllian - sergiogdb@gmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## Vendored libraries

These are checked in under `recompiler/lib/` and `runtime/third_party/` with
their upstream license files intact. All are permissive, so nothing here
constrains this repository's own terms; the obligation is to carry the notice.

| Component | License | Linked into | Notice |
| --- | --- | --- | --- |
| [toml11](https://github.com/ToruNiina/toml11) by Toru Niina | MIT | recompiler **and** runtime | `recompiler/lib/toml11/LICENSE`, shipped as `runtime/licenses/toml11-NOTICES.txt` |
| [stb_image](http://nothings.org/stb) by Sean Barrett | MIT **or** public domain (Unlicense), at your option | runtime | notice in `runtime/third_party/stb_image.h`, shipped as `runtime/licenses/stb_image-NOTICES.txt` |
| [xxHash 0.8.3](https://github.com/Cyan4973/xxHash/tree/v0.8.3) by Yann Collet | BSD-2-Clause | runtime texture-pack interoperability | `runtime/third_party/xxhash.LICENSE`, shipped as `runtime/licenses/xxhash-NOTICES.txt` |
| [rabbitizer](https://github.com/Decompollaborate/rabbitizer) by Decompollaborate | MIT | recompiler only | `recompiler/lib/rabbitizer/LICENSE` |
| [ELFIO](https://github.com/serge1/ELFIO) by Serge Lamikhov-Center | MIT | recompiler only | `recompiler/lib/ELFIO/LICENSE.txt` |
| [{fmt}](https://github.com/fmtlib/fmt) by Victor Zverovich | MIT | recompiler only | `recompiler/lib/fmt/LICENSE.rst` |

Anything that reaches a **player** binary needs its notice in
`runtime/licenses/`, which both release packagers copy wholesale into the
package as `licenses/` — that is the one place to add a notice when a new
runtime dependency lands. The recompiler-only entries are developer tooling
and are not in the shipped package; if that ever changes, their notices have
to ship too.

The unmodified xxHash header comes directly from upstream tag `v0.8.3`,
`xxhash.h` (SHA-256
`17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b`).
Its BSD notice remains in the header and the separate notice files. The
DuckStation-compatible filename parser, matching, PNG dumping and cache are
independent psxrecomp implementations; no DuckStation implementation is
included. Format observations and the upstream reference pin are recorded in
[DUCKSTATION_TEXTURE_FORMAT.md](docs/DUCKSTATION_TEXTURE_FORMAT.md).

## TinyCC (TCC) — toolchain-free overlay compiler shipped to players

[TinyCC](https://bellard.org/tcc/) by Fabrice Bellard and contributors, licensed
**LGPL-2.1**. Not vendored in this repository — no TinyCC source or binary is
tracked here. It is invoked as a **separate subprocess** by
`tools/compile_overlays.py` (`--compiler tcc`, `--tcc &lt;binary&gt;`) to build overlay
shards into a DLL, so players need no compiler of their own. Nothing in the
runtime links against libtcc, so this is aggregation with a separate program
rather than LGPL linkage.

The runtime expects an end-user bundle at
`&lt;exe_dir&gt;/overlay_toolchain/{python/, tcc/tcc.exe, compile_overlays.py, …}`
(`runtime/src/main.cpp`). `tools/release_stage.py` (`stage_toolchain`) populates
it on Windows from the pinned, unmodified upstream TinyCC 0.9.27 win64 binary
zip. That zip carries no license file, so the notice — LGPL-2.1 text, copyright
and the exact source URL — lives at `runtime/licenses/TinyCC-LICENSE.txt`
(shipped as `licenses/TinyCC-LICENSE.txt`) and is also staged beside the binary
as `overlay_toolchain/tcc/COPYING`. Staging fails if the notice does not name the
pinned version and archive hash, and the release zip verifier
(`docs/ci/templates/game-release.yml`) rejects a zip that bundles `tcc.exe`
without both copies.

Developers with `gcc` on `PATH` use the gcc tier instead; the bundled tcc matters
only for end-user release packages (`docs/BUILDING.md`).

## JRickey / gba-recomp — verified-enhancement shadow + screen color science

The verified-enhancement QoL layer (`feat/shadow-enhancements`) reuses two
engine-agnostic pieces originally authored by Jrickey in
[JRickey/gba-recomp](https://github.com/JRickey/gba-recomp), licensed
**MIT OR Apache-2.0**, used with permission:

- **`ShadowVerifier`** — the envelope-correlation differential self-check,
  probation auto-gain calibration, and prove/strike/pause state machine.
  Original: `crates/gba-core/src/shadow.rs`.
  This repo: `runtime/src/audio_shadow.c`, `runtime/include/audio_shadow.h`
  (C re-implementation, via the gbarecomp C++ port `src/gba/audio_shadow.*`
  and the snesrecomp C port `runner/src/snes/audio_shadow.*`; the algorithm is
  unchanged).

- **Color-science core** (xyY→XYZ, primaries→matrix, Bradford chromatic
  adaptation, sRGB OETF) used to bake the present-time screen-color LUT.
  Original: `crates/screen/src/{color,profile,lut}.rs`.
  This repo: `runtime/src/color_lut.c`, `runtime/include/color_lut.h`
  (C re-implementation, via the gbarecomp C++ port `src/runtime/color_lut.*`).

### PSX-specific work (ours)

- The **CRT / composite / Trinitron** display panel models in `color_lut.c`
  (the GBA port modelled a handheld LCD; a console scanned out to a TV needs a
  CRT/composite model instead) — SMPTE-C / Trinitron-class phosphor gamuts,
  CRT gamma, black-lift.
- The **SPU float shadow render** (`runtime/src/spu_shadow.c`,
  `runtime/include/spu_shadow.h`): 4-point cubic resampling + float headroom
  re-render of the PS1 SPU ADPCM voice mix, driven from a read-only tap on the
  canon `spu.c` voice state. This is console-specific (the SNES analog re-renders
  the S-DSP; the GBA analog re-renders the MP2K software mixer).
- The tap plumbing in `runtime/src/spu.c` and `runtime/include/spu.h`.

All reuse keeps the original copyright and dual MIT/Apache-2.0 license.

## retcomm-studio — multi-disc project tooling

[retcomm-studio](https://github.com/RetroPortingToolKit/Retro-Studio) by
Alex Vanderveen, licensed **MIT** (notice: `LICENSE` in that repository).
psxrecomp is PolyForm-NC, so this is permissive vendored into stricter — the
MIT notice must ride along and is why this entry exists.

Vendored into `tools/new_project_layout/` at commit `06bb918b`:

- **`verify_disc_set.py`** — verifies that N probed images form one buildable
  set (identical program across differing per-disc serials). New to this
  repository.
- **`probe_disc.py`** — synced forward. psxrecomp carried a stale fork
  predating multi-disc support, 90 lines behind; its only 8 unique lines were
  older revisions of the same functions, so nothing psxrecomp-specific was
  dropped. A single-disc project still renders `disc = "..."` byte-identically.

`update_disc_set.py` beside them is **not** vendored — it is psxrecomp's own,
written here because neither repository had it: `probe_disc.py
--write-game-toml` renders a complete game.toml, which is correct when
scaffolding a project and destructive on a live one.

Keep the pin above accurate when re-syncing. These files are the reason a
standalone setup-wizard install and a Retro build produce the same
multi-disc `game.toml`; if the two drift, so do those two paths.

## OpenXR SDK loader - optional PC headset backend

[OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) by The Khronos Group
Inc. and contributors is licensed Apache-2.0. Its bundled JsonCpp sources are
distributed under their MIT terms. Only `PSX_OPENXR=ON` links the static loader;
the SDK provides the official host API/runtime discovery implementation needed
to connect to the user's separately installed OpenXR runtime. The feature is
currently Win32/OpenGL-only and defaults OFF, so ordinary builds do not fetch it.

The dependency is pinned by annotated tag object
`b76b80adaf65ac3ad6cc1ce61974fb29a5d02352`, resolving to commit
`c15d38cb4bb10a5b7e075f74493ff13896e2597a`. Enabled builds fetch and compile that
source, including bundled JsonCpp; they need network on a cold cache (the pinned
archive is verified against the SHA256 in `third_party/deps.manifest`), a vendored
archive (`tools/ci/vendor_deps.sh psx_openxr_sdk`), or a
`FETCHCONTENT_SOURCE_DIR_PSX_OPENXR_SDK` / `PSX_OPENXR_SDK_SOURCE_DIR` override. No proprietary headset SDK,
runtime binary, API layer or vendor driver is redistributed.

The complete SDK and JsonCpp notices are in
`runtime/licenses/OpenXR-SDK-NOTICES.txt`; both existing release packagers copy
that directory into player packages. The source dependency is unmodified.
