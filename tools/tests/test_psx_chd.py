"""Source-owned CHD fixtures: a synthetic uncompressed v5 CHD wrapped around a
cooked ISO 9660 image. No retail sectors, executables or audio.

The pure-Python parts (metadata parsing, track layout, cue rendering, layout
selection) always run. Everything that decodes a CHD needs the shared libchdr
that ``recompiler/`` builds (CMake target ``chdr``); those tests skip with a
reason when ``PSXRECOMP_LIBCHDR`` is unset and no build directory has it.
"""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "tools"), str(ROOT / "tools" / "new_project_layout")]
import prepare_disc as prepare  # noqa: E402
import psx_chd  # noqa: E402
import psxrecomp_cli as cli  # noqa: E402
from sdk_progress import ProgressReporter  # noqa: E402

BOOT = "SLES_000.00"
FRAME = psx_chd.CD_FRAME_SIZE
RAW = psx_chd.RAW_SECTOR_SIZE
HUNK_FRAMES = 8


def cooked_disc() -> bytes:
    """A 24-sector ISO 9660 image with SYSTEM.CNF and a PS-X EXE stub."""
    sectors = [bytearray(2048) for _ in range(24)]
    sectors[16][1:6] = b"CD001"
    struct.pack_into("<I", sectors[16], 158, 20)
    struct.pack_into("<I", sectors[16], 166, 2048)
    cursor = 0
    exe = bytearray(b"PS-X EXE" + bytes(2040))
    struct.pack_into("<I", exe, 0x10, 0x80010000)  # PC0
    struct.pack_into("<I", exe, 0x18, 0x80010000)  # load address
    struct.pack_into("<I", exe, 0x1C, 0x800)       # text size
    for name, lba, data in [("SYSTEM.CNF", 21, b"BOOT = cdrom:\\" + BOOT.encode() + b";1"),
                            (BOOT, 22, bytes(exe))]:
        encoded = (name + ";1").encode()
        record = bytearray(33 + len(encoded) + (len(encoded) % 2 == 0))
        record[0] = len(record)
        struct.pack_into("<I", record, 2, lba)
        struct.pack_into("<I", record, 10, len(data))
        record[32] = len(encoded)
        record[33:33 + len(encoded)] = encoded
        sectors[20][cursor:cursor + len(record)] = record
        cursor += len(record)
        sectors[lba][:len(data)] = data
    return b"".join(sectors)


def audio_track(frames: int, seed: int) -> bytes:
    """Deterministic little-endian PCM, the byte order a Redump .bin holds."""
    out = bytearray()
    for i in range(frames * RAW // 2):
        v = (i * 7919 + seed) & 0xFFFF
        out += struct.pack("<H", v)
    return bytes(out)


def write_uncompressed_chd(path: Path, tracks) -> None:
    """Write a v5 CHD with codec NONE. ``tracks`` is a list of
    ``(type, data_bytes, pregap, pgtype)``; ``data_bytes`` is the track's
    .bin content, including any stored pregap frames."""
    hunk_bytes = FRAME * HUNK_FRAMES
    stream = bytearray()
    meta_texts = []
    for number, (ttype, data, pregap, pgtype) in enumerate(tracks, 1):
        assert len(data) % RAW == 0
        frames = len(data) // RAW
        for f in range(frames):
            sector = data[f * RAW:(f + 1) * RAW]
            if ttype == "AUDIO":
                sector = b"".join(sector[i + 1:i + 2] + sector[i:i + 1] for i in range(0, RAW, 2))
            stream += sector + bytes(psx_chd.SUBCODE_SIZE)
        total = len(stream) // FRAME
        padded = (total + psx_chd.CD_TRACK_PADDING - 1) & ~(psx_chd.CD_TRACK_PADDING - 1)
        stream += bytes((padded - total) * FRAME)
        meta_texts.append(
            f"TRACK:{number} TYPE:{ttype} SUBTYPE:NONE FRAMES:{frames} PREGAP:{pregap} "
            f"PGTYPE:{pgtype} PGSUB:NONE POSTGAP:0"
        )
    logical = len(stream)
    total_hunks = (logical + hunk_bytes - 1) // hunk_bytes
    stream += bytes(total_hunks * hunk_bytes - logical)

    header_size = 124
    map_offset = header_size
    map_size = total_hunks * 4
    meta_offset = map_offset + map_size
    meta_blob = bytearray()
    cursor = meta_offset
    for i, text in enumerate(meta_texts):
        payload = text.encode("ascii") + b"\0"
        entry = 16 + len(payload)
        nxt = cursor + entry if i + 1 < len(meta_texts) else 0
        meta_blob += struct.pack(">II Q", psx_chd.CDROM_TRACK_METADATA2_TAG,
                                 (1 << 24) | len(payload), nxt) + payload
        cursor += entry
    data_offset = ((meta_offset + len(meta_blob) + hunk_bytes - 1) // hunk_bytes) * hunk_bytes
    first_index = data_offset // hunk_bytes
    raw_map = b"".join(struct.pack(">I", first_index + i) for i in range(total_hunks))
    header = (b"MComprHD" + struct.pack(">II", header_size, 5) + struct.pack(">IIII", 0, 0, 0, 0)
              + struct.pack(">QQQ", logical, map_offset, meta_offset)
              + struct.pack(">II", hunk_bytes, FRAME) + bytes(60))
    assert len(header) == header_size
    blob = bytearray(header) + raw_map + meta_blob
    blob += bytes(data_offset - len(blob))
    blob += stream
    path.write_bytes(bytes(blob))


def find_lib():
    return psx_chd.find_libchdr(None, ROOT)


class PureTests(unittest.TestCase):
    def test_parse_v2_and_v1_metadata(self):
        t = psx_chd.parse_track_metadata(
            "TRACK:2 TYPE:AUDIO SUBTYPE:NONE FRAMES:6502 PREGAP:150 PGTYPE:VAUDIO PGSUB:NONE POSTGAP:0\0",
            v2=True)
        self.assertEqual((t.number, t.type, t.frames, t.pregap, t.pgtype), (2, "AUDIO", 6502, 150, "VAUDIO"))
        t1 = psx_chd.parse_track_metadata("TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:100", v2=False)
        self.assertEqual((t1.number, t1.frames, t1.pregap), (1, 100, 0))
        with self.assertRaises(psx_chd.ChdError):
            psx_chd.parse_track_metadata("TRACK:1 TYPE:AUDIO", v2=True)

    def test_layout_mirrors_runtime(self):
        tracks = [
            psx_chd.ChdTrack(1, "MODE2_RAW", "NONE", 10),
            psx_chd.ChdTrack(2, "AUDIO", "NONE", 7, pregap=2, pgtype="VAUDIO"),
            psx_chd.ChdTrack(3, "AUDIO", "NONE", 5, pregap=3, pgtype="AUDIO"),
        ]
        table = psx_chd.build_track_table(tracks, v2=[True, True, True])
        self.assertEqual([t.chd_frame_start for t in table], [0, 12, 20])
        self.assertEqual([t.stored_pregap for t in table], [0, 2, 0])
        self.assertEqual([t.virtual_pregap for t in table], [0, 0, 3])
        self.assertEqual([t.disc_lba for t in table], [0, 10, 17])
        with self.assertRaises(psx_chd.ChdError):
            psx_chd.build_track_table([psx_chd.ChdTrack(2, "AUDIO", "NONE", 1)], v2=[True])

    def test_render_cue_both_layouts(self):
        tracks = psx_chd.build_track_table([
            psx_chd.ChdTrack(1, "MODE2_RAW", "NONE", 100),
            psx_chd.ChdTrack(2, "AUDIO", "NONE", 300, pregap=150, pgtype="VAUDIO"),
        ], v2=[True, True])
        multi = psx_chd.render_cue(tracks, layout="multi", stem="Game")
        self.assertEqual(multi.splitlines(), [
            'FILE "Game (Track 1).bin" BINARY', "  TRACK 01 MODE2/2352", "    INDEX 01 00:00:00",
            'FILE "Game (Track 2).bin" BINARY', "  TRACK 02 AUDIO", "    INDEX 00 00:00:00", "    INDEX 01 00:02:00",
        ])
        single = psx_chd.render_cue(tracks, layout="single", stem="Game")
        self.assertEqual(single.splitlines(), [
            'FILE "Game.bin" BINARY', "  TRACK 01 MODE2/2352", "    INDEX 01 00:00:00",
            "  TRACK 02 AUDIO", "    INDEX 00 00:01:25", "    INDEX 01 00:03:25",
        ])
        with self.assertRaises(psx_chd.ChdError):
            psx_chd.render_cue([psx_chd.ChdTrack(1, "MODE1", "NONE", 1)], layout="multi", stem="x")

    def test_layout_matching(self):
        d = psx_chd.ChdDigests(tracks=[psx_chd.Digest(10, "a", "b"), psx_chd.Digest(5, "c", "d")],
                               disc=psx_chd.Digest(15, "e", "f"))
        self.assertEqual(d.layout_matching([], ["A"], []), "multi")
        self.assertEqual(d.layout_matching([], [], ["F"]), "single")
        self.assertEqual(d.layout_matching([15], [], []), "single")
        self.assertIsNone(d.layout_matching([], ["zz"], ["yy"]))


@unittest.skipUnless(find_lib(), f"libchdr not built; set {psx_chd.LIB_ENV} or build target chdr")
class DecodeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.lib = psx_chd.LibChdr(find_lib())
        self.data = prepare.iso_to_bin(cooked_disc())
        self.audio = audio_track(150 + 33, seed=3)  # 150 stored pregap frames + 33 audio frames
        self.chd = self.root / "Fixture (Europe).chd"
        write_uncompressed_chd(self.chd, [
            ("MODE2_RAW", self.data, 0, "MODE2_RAW"),
            ("AUDIO", self.audio, 150, "VAUDIO"),
        ])
        self.single = self.root / "Single (USA).chd"
        write_uncompressed_chd(self.single, [("MODE2_RAW", self.data, 0, "MODE2_RAW")])

    def sha1(self, b: bytes) -> str:
        return hashlib.sha1(b).hexdigest()

    def test_tracks_read_back_as_redump_bytes(self):
        with psx_chd.ChdDisc(self.chd, self.lib) as disc:
            self.assertEqual([t.type for t in disc.tracks], ["MODE2_RAW", "AUDIO"])
            self.assertEqual(disc.tracks[1].stored_pregap, 150)
            self.assertEqual(disc.read_track(disc.tracks[0]), self.data)
            self.assertEqual(disc.read_track(disc.tracks[1]), self.audio)
            d = psx_chd.digests(disc)
        self.assertEqual(d.first_track.sha1, self.sha1(self.data))
        self.assertEqual(d.tracks[1].sha1, self.sha1(self.audio))
        self.assertEqual(d.disc.sha1, self.sha1(self.data + self.audio))
        self.assertEqual(d.disc.size, len(self.data) + len(self.audio))

    def test_extract_multi_and_single(self):
        with psx_chd.ChdDisc(self.chd, self.lib) as disc:
            cue = psx_chd.extract(disc, self.root / "multi", "Fixture (Europe).cue", layout="multi")
            single = psx_chd.extract(disc, self.root / "single", "Fixture (Europe).cue", layout="single")
        self.assertEqual((self.root / "multi" / "Fixture (Europe) (Track 1).bin").read_bytes(), self.data)
        self.assertEqual((self.root / "multi" / "Fixture (Europe) (Track 2).bin").read_bytes(), self.audio)
        self.assertIn('FILE "Fixture (Europe) (Track 2).bin" BINARY', cue.read_text())
        self.assertEqual((self.root / "single" / "Fixture (Europe).bin").read_bytes(), self.data + self.audio)
        self.assertIn("INDEX 00 00:00:24", single.read_text())  # 24 data sectors, then the stored pregap

    def test_verify_disc_path_hashes_tracks_not_the_container(self):
        prep_multi = {"known_sha1": [self.sha1(self.data)]}
        identity = cli.verify_disc_path(self.chd, prep_multi, skip_hash=False, progress=ProgressReporter())
        self.assertTrue(identity["verified"])
        self.assertEqual(identity["chd"], {"layout": "multi", "tracks": 2})
        self.assertEqual(identity["size"], len(self.data))

        prep_single = {"known_md5": [hashlib.md5(self.data + self.audio).hexdigest()]}
        identity = cli.verify_disc_path(self.chd, prep_single, skip_hash=False, progress=ProgressReporter())
        self.assertEqual(identity["chd"]["layout"], "single")
        self.assertEqual(identity["size"], len(self.data) + len(self.audio))

        with self.assertRaises(cli.DiscVerifyError) as ctx:
            cli.verify_disc_path(self.chd, {"known_sha1": ["0" * 40]}, skip_hash=False, progress=ProgressReporter())
        self.assertIn("CHD track digests", str(ctx.exception))
        self.assertNotIn("wrong dump", str(ctx.exception))

    def test_verify_without_reader_names_the_reader_not_the_dump(self):
        with patch.dict("os.environ", {psx_chd.LIB_ENV: ""}), patch.object(psx_chd, "find_libchdr", return_value=None):
            with self.assertRaises(cli.DiscVerifyError) as ctx:
                cli.verify_disc_path(self.chd, {"known_sha1": ["x"]}, skip_hash=False, progress=ProgressReporter())
        self.assertIn("libchdr", str(ctx.exception))
        self.assertIn("does not mean your dump is bad", str(ctx.exception))

    def run_prepare(self, source: Path, config_text: str, out: Path):
        config = self.root / "game.toml"
        config.write_text(config_text, encoding="utf-8")
        output = io.StringIO()
        args = ["prepare_disc.py", str(source), "--config", str(config), "--out-dir", str(out)]
        with patch.object(sys, "argv", args), contextlib.redirect_stdout(output), \
                contextlib.redirect_stderr(output), patch.object(prepare, "_configure_stdio"):
            code = prepare.main()
        return code, output.getvalue()

    def test_prepare_disc_stages_a_multitrack_chd_once(self):
        out = self.root / "disc"
        code, log = self.run_prepare(self.chd, (
            f'[game]\nid = "SLES-00000"\n[prepare_disc]\nboot_exe = "{BOOT}"\n'
            f'bin_name = "Fixture (Europe) (Track 1).bin"\ncue_name = "Fixture (Europe).cue"\n'
            f'known_sha1 = ["{self.sha1(self.data)}"]\n'
        ), out)
        self.assertEqual(code, 0, log)
        self.assertIn("layout: multi", log)
        self.assertIn("preserving multi-track Redump set (2 files)", log)
        self.assertEqual((out / "Fixture (Europe) (Track 1).bin").read_bytes(), self.data)
        self.assertEqual((out / "Fixture (Europe) (Track 2).bin").read_bytes(), self.audio)
        self.assertTrue((out / BOOT).is_file())
        receipt = json.loads((out / "Fixture (Europe).disc-receipt.json").read_text())
        # macOS reports the temp dir through /private/var while resolve() may
        # not; compare resolved paths.
        self.assertEqual(Path(receipt["source_image"]).resolve(), self.chd.resolve())
        self.assertIn('FILE "Fixture (Europe) (Track 2).bin" BINARY', (out / "Fixture (Europe).cue").read_text())

    def test_prepare_disc_single_track_chd_lands_on_bin_name(self):
        out = self.root / "disc"
        code, log = self.run_prepare(self.single, (
            f'[game]\nid = "SLUS-00000"\n[prepare_disc]\nboot_exe = "{BOOT}"\n'
            f'bin_name = "Single (USA).bin"\ncue_name = "Single (USA).cue"\n'
            f'known_md5 = ["{hashlib.md5(self.data).hexdigest()}"]\n'
        ), out)
        self.assertEqual(code, 0, log)
        self.assertIn("source already at", log)
        self.assertEqual((out / "Single (USA).bin").read_bytes(), self.data)
        self.assertEqual(sorted(p.name for p in out.glob("*.bin")), ["Single (USA).bin"])

    def test_prepare_disc_refuses_unknown_digests_without_override(self):
        code, log = self.run_prepare(self.single, (
            f'[game]\nid = "SLUS-00000"\n[prepare_disc]\nboot_exe = "{BOOT}"\n'
            f'known_sha1 = ["{"0" * 40}"]\n'
        ), self.root / "disc")
        self.assertEqual(code, 1)
        self.assertIn("CHD track digests are not in prepare_disc.known_*", log)

    def test_probe_disc_accepts_a_chd(self):
        import probe_disc
        output = io.StringIO()
        args = ["probe_disc.py", str(self.chd)]
        with patch.object(sys, "argv", args), contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            code = probe_disc.main()
        self.assertEqual(code, 0)
        payload = json.loads(output.getvalue())
        self.assertEqual(payload["boot_exe"], BOOT)
        self.assertEqual(payload["track_count"], 2)
        self.assertEqual(payload["data_track_size"], len(self.data))
        self.assertEqual(payload["bin_name"], "Fixture (Europe) (Track 1).bin")


if __name__ == "__main__":
    unittest.main()
