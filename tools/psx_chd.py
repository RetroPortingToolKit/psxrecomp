#!/usr/bin/env python3
"""Read MAME-compatible .chd disc images from Python through libchdr.

The runtime mounts .chd natively, so a player can pick one in the launcher and
see a valid disc identity. The Python side of setup -- ``verify-disc``,
``tools/prepare_disc.py`` and ``probe_disc.py`` -- hashes and parses raw
2352-byte track data, which a CHD stores compressed. This module bridges the
two: it loads the libchdr shared library that ``recompiler/`` builds next to
the emitters and reproduces, byte for byte, the ``.bin`` files a Redump dump
of the same disc would contain. Nothing here decompresses on its own; libchdr
carries the LZMA, zlib, FLAC and zstd codecs a real CHD needs, and it is the
same decoder the runtime links.

Layout facts mirrored from ``runtime/src/iso_reader.cpp`` (the runtime is the
reference for how a CHD maps to disc sectors; keep the two in step):

* every CD frame is 2448 bytes: 2352 of sector data followed by 96 of subcode;
* a track's frames are stored consecutively, then the next track starts on a
  4-frame boundary;
* a ``PREGAP`` whose ``PGTYPE`` starts with ``V`` is stored inside the track's
  ``FRAMES``; any other pregap is virtual and occupies no frames;
* audio frames are stored big-endian and are byte-swapped to the little-endian
  PCM a ``.bin`` holds.

Two output layouts exist because Redump ships both: one ``.bin`` per track
(what the runtime and ``prepare_disc`` expect for CDDA titles) or a single
``.bin`` holding every track. ``[prepare_disc]`` digests identify which one a
title was catalogued from; :func:`digests` computes both in one pass so the
caller can pick.

Python 3.9 compatible on purpose: the kits run on whatever python3 the
player's distribution ships.
"""

from __future__ import annotations

import ctypes
import hashlib
import os
import re
import sys
from array import array
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterator, Optional, Sequence

CD_FRAME_SIZE = 2448
RAW_SECTOR_SIZE = 2352
SUBCODE_SIZE = 96
CD_TRACK_PADDING = 4
CD_MAX_TRACKS = 99

CHD_OPEN_READ = 1
CHDERR_NONE = 0
CHDERR_METADATA_NOT_FOUND = 19  # enum _chd_error in libchdr/chd.h


def _make_tag(a: str, b: str, c: str, d: str) -> int:
    return (ord(a) << 24) | (ord(b) << 16) | (ord(c) << 8) | ord(d)


CDROM_TRACK_METADATA_TAG = _make_tag("C", "H", "T", "R")
CDROM_TRACK_METADATA2_TAG = _make_tag("C", "H", "T", "2")

# Track types chdman writes, mapped to the cue mode that names the same
# 2352-byte frame. Cooked types (MODE1, MODE2, MODE2_FORM1, ...) store fewer
# than 2352 bytes of payload per frame and have no Redump .bin equivalent, so
# they are refused rather than guessed at; PS1 dumps are MODE2_RAW + AUDIO.
_CUE_MODE = {
    "MODE1_RAW": "MODE1/2352",
    "MODE2_RAW": "MODE2/2352",
    "AUDIO": "AUDIO",
}

_LIB_NAMES = {
    "win32": ("libchdr.dll", "chdr.dll"),
    "darwin": ("libchdr.dylib", "libchdr.0.dylib", "libchdr.0.3.dylib"),
}
_LIB_NAMES_POSIX = ("libchdr.so", "libchdr.so.0", "libchdr.so.0.3")
LIB_ENV = "PSXRECOMP_LIBCHDR"


class ChdError(Exception):
    """A CHD could not be opened, is not a CD image, or is not supported."""


# ---------------------------------------------------------------------------
# libchdr binding
# ---------------------------------------------------------------------------

_CHD_MD5_BYTES = 16
_CHD_SHA1_BYTES = 20


class _ChdHeader(ctypes.Structure):
    """Mirror of ``struct _chd_header`` in ``include/libchdr/chd.h``.

    Only the leading fields are read; the rest exist so the struct has the
    right size and the pointer field lands on its natural alignment.
    """

    _fields_ = [
        ("length", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("compression", ctypes.c_uint32 * 4),
        ("hunkbytes", ctypes.c_uint32),
        ("totalhunks", ctypes.c_uint32),
        ("logicalbytes", ctypes.c_uint64),
        ("metaoffset", ctypes.c_uint64),
        ("mapoffset", ctypes.c_uint64),
        ("md5", ctypes.c_uint8 * _CHD_MD5_BYTES),
        ("parentmd5", ctypes.c_uint8 * _CHD_MD5_BYTES),
        ("sha1", ctypes.c_uint8 * _CHD_SHA1_BYTES),
        ("rawsha1", ctypes.c_uint8 * _CHD_SHA1_BYTES),
        ("parentsha1", ctypes.c_uint8 * _CHD_SHA1_BYTES),
        ("unitbytes", ctypes.c_uint32),
        ("unitcount", ctypes.c_uint64),
        ("hunkcount", ctypes.c_uint32),
        ("mapentrybytes", ctypes.c_uint32),
        ("rawmap", ctypes.c_void_p),
        ("obsolete_cylinders", ctypes.c_uint32),
        ("obsolete_sectors", ctypes.c_uint32),
        ("obsolete_heads", ctypes.c_uint32),
        ("obsolete_hunksize", ctypes.c_uint32),
    ]


def candidate_dirs(project_root: Optional[Path], framework_root: Optional[Path]) -> list[Path]:
    """Directories the shared library may sit in, most specific first.

    Same order as ``psxrecomp_cli._find_recompiler_tool`` so the library is
    found wherever the emitters were.
    """
    dirs: list[Path] = []
    if project_root is not None:
        dirs += [
            project_root / "psxrecomp" / "recompiler" / "build",
            project_root / "psxrecomp" / "recompiler" / "build" / "Release",
            project_root / "build-recompiler",
        ]
    if framework_root is not None:
        dirs += [
            framework_root / "recompiler" / "build",
            framework_root / "recompiler" / "build" / "Release",
        ]
    return dirs


def library_names() -> tuple[str, ...]:
    return _LIB_NAMES.get(sys.platform, _LIB_NAMES_POSIX)


def find_libchdr(
    project_root: Optional[Path] = None,
    framework_root: Optional[Path] = None,
) -> Optional[Path]:
    """Locate the shared libchdr, honouring ``PSXRECOMP_LIBCHDR`` first."""
    env = os.environ.get(LIB_ENV, "").strip()
    if env:
        p = Path(env).expanduser()
        if p.is_file():
            return p.resolve()
    for d in candidate_dirs(project_root, framework_root):
        for name in library_names():
            c = d / name
            if c.is_file():
                return c.resolve()
        if d.is_dir() and sys.platform not in _LIB_NAMES:
            # Versioned .so names vary with libchdr's own version.
            for c in sorted(d.glob("libchdr.so*")):
                if c.is_file():
                    return c.resolve()
    return None


class LibChdr:
    """Loaded libchdr with the prototypes this module uses."""

    def __init__(self, path: Path):
        self.path = Path(path)
        try:
            self._lib = ctypes.CDLL(str(self.path))
        except OSError as exc:
            raise ChdError(f"cannot load libchdr from {self.path}: {exc}") from exc
        lib = self._lib
        try:
            lib.chd_open.argtypes = [
                ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_void_p),
            ]
            lib.chd_open.restype = ctypes.c_int
            lib.chd_close.argtypes = [ctypes.c_void_p]
            lib.chd_close.restype = None
            lib.chd_get_header.argtypes = [ctypes.c_void_p]
            lib.chd_get_header.restype = ctypes.POINTER(_ChdHeader)
            lib.chd_read.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
            lib.chd_read.restype = ctypes.c_int
            lib.chd_get_metadata.argtypes = [
                ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
                ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint8),
            ]
            lib.chd_get_metadata.restype = ctypes.c_int
            lib.chd_error_string.argtypes = [ctypes.c_int]
            lib.chd_error_string.restype = ctypes.c_char_p
        except AttributeError as exc:
            raise ChdError(f"{self.path} does not export the libchdr API: {exc}") from exc

    def error_string(self, err: int) -> str:
        s = self._lib.chd_error_string(err)
        return s.decode("ascii", "replace") if s else f"chd error {err}"

    @property
    def raw(self) -> ctypes.CDLL:
        return self._lib


def _open_arg_candidates(path: Path) -> list[bytes]:
    """Byte strings to try with ``chd_open``, which ends in ``fopen``.

    On Windows that is the ANSI code page. A path the code page cannot spell
    (a title with an accented letter under a Greek locale, say) is retried
    through its 8.3 short name, which is always ASCII.
    """
    cands: list[bytes] = []
    try:
        cands.append(os.fsencode(str(path)))
    except UnicodeError:
        pass
    if sys.platform == "win32":
        try:
            mbcs = str(path).encode("mbcs")
            if mbcs not in cands:
                cands.append(mbcs)
        except UnicodeError:
            pass
        try:
            buf = ctypes.create_unicode_buffer(32768)
            n = ctypes.windll.kernel32.GetShortPathNameW(str(path), buf, len(buf))  # type: ignore[attr-defined]
            if 0 < n < len(buf):
                short = buf.value.encode("mbcs")
                if short not in cands:
                    cands.append(short)
        except (AttributeError, OSError, UnicodeError):
            pass
    return cands


# ---------------------------------------------------------------------------
# Track table
# ---------------------------------------------------------------------------

@dataclass
class ChdTrack:
    number: int
    type: str
    subtype: str
    frames: int
    pregap: int = 0
    pgtype: str = ""
    pgsub: str = ""
    postgap: int = 0
    # Derived layout, filled by build_track_table().
    stored_pregap: int = 0
    virtual_pregap: int = 0
    chd_frame_start: int = 0
    disc_lba: int = 0  # first disc sector, including any virtual pregap

    @property
    def is_audio(self) -> bool:
        return self.type == "AUDIO"

    @property
    def cue_mode(self) -> str:
        try:
            return _CUE_MODE[self.type]
        except KeyError:
            raise ChdError(
                f"track {self.number} is {self.type}; only MODE1_RAW, MODE2_RAW "
                f"and AUDIO tracks have a 2352-byte .bin form"
            ) from None

    @property
    def data_bytes(self) -> int:
        return self.frames * RAW_SECTOR_SIZE


_META_V2 = re.compile(
    r"TRACK:(\d+) TYPE:(\S+) SUBTYPE:(\S+) FRAMES:(\d+) PREGAP:(\d+) "
    r"PGTYPE:(\S+) PGSUB:(\S+) POSTGAP:(\d+)"
)
_META_V1 = re.compile(r"TRACK:(\d+) TYPE:(\S+) SUBTYPE:(\S+) FRAMES:(\d+)")


def parse_track_metadata(text: str, *, v2: bool) -> ChdTrack:
    """Parse one ``CHT2`` (v2) or ``CHTR`` (v1) metadata string."""
    text = text.rstrip("\0").strip()
    if v2:
        m = _META_V2.match(text)
        if not m:
            raise ChdError(f"malformed CHT2 track metadata: {text!r}")
        return ChdTrack(
            number=int(m.group(1)), type=m.group(2), subtype=m.group(3),
            frames=int(m.group(4)), pregap=int(m.group(5)), pgtype=m.group(6),
            pgsub=m.group(7), postgap=int(m.group(8)),
        )
    m = _META_V1.match(text)
    if not m:
        raise ChdError(f"malformed CHTR track metadata: {text!r}")
    return ChdTrack(
        number=int(m.group(1)), type=m.group(2), subtype=m.group(3),
        frames=int(m.group(4)),
    )


def build_track_table(tracks: Sequence[ChdTrack], *, v2: Sequence[bool]) -> list[ChdTrack]:
    """Fill the derived layout fields the way the runtime lays a CHD out.

    ``v2`` says, per track, whether the entry came from CHT2 (only those can
    carry a stored pregap).
    """
    out: list[ChdTrack] = []
    disc_lba = 0
    chd_frame = 0
    for index, (t, is_v2) in enumerate(zip(tracks, v2)):
        if t.number != index + 1:
            raise ChdError(f"track metadata out of order: expected {index + 1}, got {t.number}")
        if t.frames <= 0:
            raise ChdError(f"track {t.number} has no frames")
        pregap_stored = is_v2 and t.pregap > 0 and t.pgtype[:1].upper() == "V"
        t.stored_pregap = min(t.pregap, t.frames) if pregap_stored else 0
        t.virtual_pregap = t.pregap if (not pregap_stored and t.pregap > 0) else 0
        t.disc_lba = disc_lba
        t.chd_frame_start = chd_frame
        disc_lba += t.virtual_pregap + t.frames
        chd_frame += t.frames
        chd_frame = (chd_frame + CD_TRACK_PADDING - 1) & ~(CD_TRACK_PADDING - 1)
        out.append(t)
    if not out:
        raise ChdError("CHD carries no CD track metadata; not a CD image")
    return out


# ---------------------------------------------------------------------------
# Disc access
# ---------------------------------------------------------------------------

class ChdDisc:
    """An open CD CHD. Use as a context manager."""

    def __init__(self, path: Path, lib: LibChdr):
        self.path = Path(path)
        self.lib = lib
        self._file = ctypes.c_void_p()
        self.hunk_bytes = 0
        self.frames_per_hunk = 0
        self.total_hunks = 0
        self.tracks: list[ChdTrack] = []
        self._hunk_buf: Optional[ctypes.Array] = None
        self._cached_hunk = -1
        self._open()

    # -- lifecycle ----------------------------------------------------------

    def _open(self) -> None:
        lib = self.lib.raw
        last = CHDERR_NONE
        for arg in _open_arg_candidates(self.path):
            handle = ctypes.c_void_p()
            err = lib.chd_open(arg, CHD_OPEN_READ, None, ctypes.byref(handle))
            if err == CHDERR_NONE and handle.value:
                self._file = handle
                break
            last = err
        else:
            raise ChdError(f"cannot open {self.path.name}: {self.lib.error_string(last)}")

        hdr_p = lib.chd_get_header(self._file)
        if not hdr_p:
            self.close()
            raise ChdError(f"{self.path.name}: libchdr returned no header")
        hdr = hdr_p.contents
        if hdr.hunkbytes == 0 or hdr.hunkbytes % CD_FRAME_SIZE != 0:
            self.close()
            raise ChdError(
                f"{self.path.name} is not a CD image: hunk size {hdr.hunkbytes} "
                f"is not a multiple of the {CD_FRAME_SIZE}-byte CD frame"
            )
        self.hunk_bytes = int(hdr.hunkbytes)
        self.frames_per_hunk = self.hunk_bytes // CD_FRAME_SIZE
        self.total_hunks = int(hdr.totalhunks)
        self.version = int(hdr.version)
        self._hunk_buf = (ctypes.c_ubyte * self.hunk_bytes)()
        try:
            self.tracks = self._read_track_table()
        except ChdError:
            self.close()
            raise

    def close(self) -> None:
        if self._file and self._file.value:
            self.lib.raw.chd_close(self._file)
        self._file = ctypes.c_void_p()

    def __enter__(self) -> "ChdDisc":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # -- metadata -----------------------------------------------------------

    def _metadata(self, tag: int, index: int) -> Optional[str]:
        lib = self.lib.raw
        buf = ctypes.create_string_buffer(256)
        result_len = ctypes.c_uint32(0)
        err = lib.chd_get_metadata(
            self._file, tag, index, buf, len(buf) - 1,
            ctypes.byref(result_len), None, None,
        )
        if err == CHDERR_METADATA_NOT_FOUND:
            return None
        if err != CHDERR_NONE:
            raise ChdError(f"{self.path.name}: metadata read failed: {self.lib.error_string(err)}")
        return buf.raw[: result_len.value].decode("ascii", "replace")

    def _read_track_table(self) -> list[ChdTrack]:
        tracks: list[ChdTrack] = []
        v2_flags: list[bool] = []
        for index in range(CD_MAX_TRACKS):
            text = self._metadata(CDROM_TRACK_METADATA2_TAG, index)
            v2 = text is not None
            if not v2:
                text = self._metadata(CDROM_TRACK_METADATA_TAG, index)
                if text is None:
                    break
            tracks.append(parse_track_metadata(text, v2=v2))
            v2_flags.append(v2)
        table = build_track_table(tracks, v2=v2_flags)
        last = table[-1]
        needed = last.chd_frame_start + last.frames
        available = self.total_hunks * self.frames_per_hunk
        if needed > available:
            raise ChdError(
                f"{self.path.name}: track table needs {needed} frames but the "
                f"image holds {available}"
            )
        return table

    @property
    def data_frames(self) -> int:
        return sum(t.frames for t in self.tracks)

    # -- reading ------------------------------------------------------------

    def _hunk(self, index: int) -> bytes:
        assert self._hunk_buf is not None
        if index != self._cached_hunk:
            err = self.lib.raw.chd_read(self._file, index, self._hunk_buf)
            if err != CHDERR_NONE:
                self._cached_hunk = -1
                raise ChdError(
                    f"{self.path.name}: hunk {index} failed to decode: "
                    f"{self.lib.error_string(err)}"
                )
            self._cached_hunk = index
        return bytes(self._hunk_buf)

    def iter_track_chunks(self, track: ChdTrack) -> Iterator[bytes]:
        """Yield the track's 2352-byte sectors, hunk by hunk, as .bin bytes.

        Chunks are whole sectors but of varying length (a hunk's worth at
        most); concatenated they are exactly the track's ``.bin``.
        """
        if track.type not in _CUE_MODE:
            track.cue_mode  # raises with the reason
        frame = track.chd_frame_start
        end = frame + track.frames
        swap = track.is_audio
        while frame < end:
            hunk_index = frame // self.frames_per_hunk
            first = frame % self.frames_per_hunk
            count = min(self.frames_per_hunk - first, end - frame)
            hunk = self._hunk(hunk_index)
            start = first * CD_FRAME_SIZE
            parts = [
                hunk[off: off + RAW_SECTOR_SIZE]
                for off in range(start, start + count * CD_FRAME_SIZE, CD_FRAME_SIZE)
            ]
            chunk = b"".join(parts)
            if swap:
                a = array("H")
                a.frombytes(chunk)
                a.byteswap()
                chunk = a.tobytes()
            yield chunk
            frame += count

    def read_track(self, track: ChdTrack) -> bytes:
        return b"".join(self.iter_track_chunks(track))


# ---------------------------------------------------------------------------
# Digests and extraction
# ---------------------------------------------------------------------------

@dataclass
class Digest:
    size: int
    md5: str
    sha1: str


@dataclass
class ChdDigests:
    """Digests of every layout a Redump dump of this disc could have."""

    tracks: list[Digest] = field(default_factory=list)
    disc: Digest = field(default_factory=lambda: Digest(0, "", ""))

    @property
    def first_track(self) -> Digest:
        return self.tracks[0]

    def layout_matching(self, sizes: Sequence[int], md5s: Sequence[str], sha1s: Sequence[str]) -> Optional[str]:
        """``"multi"`` if the first track alone matches a known digest,
        ``"single"`` if the whole disc does, else ``None``."""
        md5s = [m.lower() for m in md5s]
        sha1s = [s.lower() for s in sha1s]

        def hit(d: Digest) -> bool:
            if d.md5 in md5s or d.sha1 in sha1s:
                return True
            return bool(sizes) and not md5s and not sha1s and d.size in sizes

        if hit(self.first_track):
            return "multi"
        if len(self.tracks) > 1 and hit(self.disc):
            return "single"
        return None


ProgressFn = Callable[[int, int], None]


def digests(disc: ChdDisc, progress: Optional[ProgressFn] = None) -> ChdDigests:
    """Hash every track and the whole-disc concatenation in one pass."""
    out = ChdDigests()
    disc_md5, disc_sha1 = hashlib.md5(), hashlib.sha1()
    total = disc.data_frames
    done = 0
    for track in disc.tracks:
        md5, sha1, size = hashlib.md5(), hashlib.sha1(), 0
        for chunk in disc.iter_track_chunks(track):
            md5.update(chunk)
            sha1.update(chunk)
            disc_md5.update(chunk)
            disc_sha1.update(chunk)
            size += len(chunk)
            done += len(chunk) // RAW_SECTOR_SIZE
            if progress:
                progress(done, total)
        out.tracks.append(Digest(size, md5.hexdigest(), sha1.hexdigest()))
    out.disc = Digest(
        sum(d.size for d in out.tracks), disc_md5.hexdigest(), disc_sha1.hexdigest()
    )
    return out


def msf(frames: int) -> str:
    m, rem = divmod(frames, 75 * 60)
    s, f = divmod(rem, 75)
    return f"{m:02d}:{s:02d}:{f:02d}"


def track_bin_name(stem: str, track: ChdTrack, track_count: int) -> str:
    return f"{stem}.bin" if track_count == 1 else f"{stem} (Track {track.number}).bin"


def render_cue(tracks: Sequence[ChdTrack], *, layout: str, stem: str, single_bin: Optional[str] = None) -> str:
    """Cue text for the given layout, in Redump's shape."""
    lines: list[str] = []
    if layout == "single":
        lines.append(f'FILE "{single_bin or stem + ".bin"}" BINARY')
        offset = 0
        for i, t in enumerate(tracks):
            lines.append(f"  TRACK {t.number:02d} {t.cue_mode}")
            if i and t.virtual_pregap:
                lines.append(f"    PREGAP {msf(t.virtual_pregap)}")
            if t.stored_pregap:
                lines.append(f"    INDEX 00 {msf(offset)}")
            lines.append(f"    INDEX 01 {msf(offset + t.stored_pregap)}")
            offset += t.frames
    elif layout == "multi":
        for i, t in enumerate(tracks):
            name = single_bin if (single_bin and len(tracks) == 1) else track_bin_name(stem, t, len(tracks))
            lines.append(f'FILE "{name}" BINARY')
            lines.append(f"  TRACK {t.number:02d} {t.cue_mode}")
            if i and t.virtual_pregap:
                lines.append(f"    PREGAP {msf(t.virtual_pregap)}")
            if t.stored_pregap:
                lines.append("    INDEX 00 00:00:00")
            lines.append(f"    INDEX 01 {msf(t.stored_pregap)}")
    else:
        raise ValueError(f"unknown layout {layout!r}")
    return "\n".join(lines) + "\n"


def extract(
    disc: ChdDisc,
    out_dir: Path,
    cue_name: str,
    *,
    layout: str = "multi",
    bin_name: Optional[str] = None,
    progress: Optional[ProgressFn] = None,
) -> Path:
    """Write ``.bin`` file(s) and a cue into ``out_dir``; return the cue path.

    ``bin_name`` names the single-layout bin (default: cue stem + ``.bin``).
    In the multi layout the bins follow Redump naming off the cue stem.
    """
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = Path(cue_name).stem
    total = disc.data_frames
    done = 0

    def pump(fh, track: ChdTrack) -> None:
        nonlocal done
        for chunk in disc.iter_track_chunks(track):
            fh.write(chunk)
            done += len(chunk) // RAW_SECTOR_SIZE
            if progress:
                progress(done, total)

    if layout == "single":
        target = out_dir / (bin_name or f"{stem}.bin")
        with open(target, "wb") as fh:
            for t in disc.tracks:
                pump(fh, t)
        cue_text = render_cue(disc.tracks, layout="single", stem=stem, single_bin=target.name)
    elif layout == "multi":
        single_track = len(disc.tracks) == 1 and bin_name
        for t in disc.tracks:
            name = bin_name if single_track else track_bin_name(stem, t, len(disc.tracks))
            with open(out_dir / name, "wb") as fh:
                pump(fh, t)
        cue_text = render_cue(
            disc.tracks, layout="multi", stem=stem,
            single_bin=bin_name if single_track else None,
        )
    else:
        raise ValueError(f"unknown layout {layout!r}")

    cue_path = out_dir / cue_name
    with open(cue_path, "w", encoding="ascii", newline="\n") as fh:
        fh.write(cue_text)
    return cue_path


def multi_layout_bins(tracks: Sequence[ChdTrack], cue_name: str) -> list[str]:
    stem = Path(cue_name).stem
    return [track_bin_name(stem, t, len(tracks)) for t in tracks]


def unsupported_message(path: Path) -> str:
    """What to tell a player when no libchdr is available for this CHD."""
    return (
        f"{path.name} is a CHD and the CHD reader (libchdr) is not built for this "
        f"kit, so the build tools cannot read it -- this does not mean your dump is "
        f"bad. Rebuild the emitters, or extract it first, for example: chdman "
        f'extractcd -i "{path.name}" -o "{path.stem}.cue" -ob "{path.stem}.bin", '
        f"then select the .cue."
    )


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------

def _main(argv: Optional[Sequence[str]] = None) -> int:
    import argparse

    ap = argparse.ArgumentParser(description="Inspect, hash or extract a CD .chd through libchdr.")
    ap.add_argument("chd", help="path to the .chd")
    ap.add_argument("--lib", default="", help=f"libchdr shared library (default: ${LIB_ENV} or the recompiler build dir)")
    ap.add_argument("--extract", default="", help="write .bin/.cue into this directory")
    ap.add_argument("--layout", choices=("multi", "single"), default="multi")
    ap.add_argument("--cue-name", default="", help="cue file name for --extract (default: <chd stem>.cue)")
    ap.add_argument("--no-hash", action="store_true", help="skip the digest pass")
    args = ap.parse_args(argv)

    here = Path(__file__).resolve().parent.parent
    lib_path = Path(args.lib) if args.lib else find_libchdr(None, here)
    if not lib_path:
        print(f"libchdr not found; set {LIB_ENV} or build target chdr", file=sys.stderr)
        return 2
    lib = LibChdr(lib_path)
    chd_path = Path(args.chd).expanduser().resolve()
    with ChdDisc(chd_path, lib) as disc:
        print(f"{chd_path.name}: v{disc.version}, {disc.total_hunks} hunks of {disc.hunk_bytes}, {len(disc.tracks)} track(s)")
        for t in disc.tracks:
            extra = f" pregap={t.pregap}({'stored' if t.stored_pregap else 'virtual' if t.virtual_pregap else 'none'})"
            print(f"  track {t.number:02d} {t.type:<10} frames={t.frames}{extra} chd_frame={t.chd_frame_start}")
        if not args.no_hash:
            d = digests(disc)
            for i, td in enumerate(d.tracks, 1):
                print(f"  track {i:02d}: size={td.size} md5={td.md5} sha1={td.sha1}")
            if len(d.tracks) > 1:
                print(f"  disc    : size={d.disc.size} md5={d.disc.md5} sha1={d.disc.sha1}")
        if args.extract:
            cue = extract(disc, Path(args.extract), args.cue_name or f"{chd_path.stem}.cue", layout=args.layout)
            print(f"wrote {cue}")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
