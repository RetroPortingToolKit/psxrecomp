#!/usr/bin/env python3
"""Build and verify Spikestuff's Tekken 3 TAS from an owned USA disc and BIOS."""
from __future__ import annotations

import argparse
import datetime
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import urllib.request

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PROJECT = ROOT / 'build/tekken3'
TOOLS = ROOT / 'build/tasreplays-tools'
NATIVE = ROOT / 'build/tasreplays-native'
PUBLICATION = 'https://tasvideos.org/4164M'
MOVIE_SHA = '13ebc56bb877ed3200ca3dcd54dac25ad20cba091a73351425ec8bbabc82e5d6'
BIOS_SHA = '71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3'
EXE_SHA = 'fbda8b68e5799dbef4af39a161783bc670c15b0aa0e87dce65e210717da19b8c'
WORDS_SHA = '6f46b6e44f15b73d28209123d8d3d2f8d3972ada0c524d13e0d296dbd4dac606'
TAPE_SHA = 'd85f0dec13b00e50b10a52ce0fda3cdaa9797f058ad16dd57caa8e926fad7a6a'
TRACKS = [
    (632532768, '6b660e62748d02779e9e08362a5ed202540af7fad134de2ec0a2184ad78bb496'),
    (27701856, '37cc0be9c76738e7fc1842e532126c1a4fdc9486ea51720a3b4182b69ace56d6'),
    (28042896, 'c7c6db2736351933f04d9843c3358a641f6548340da10a3f19a2e48562683ad2'),
]
PROFILE = [
    '--critical-section-model', 'exception',
    '--field-model', 'octoshock-2.2.2-ntsc-raster',
    '--dma-model', 'octoshock-2.2.2-otc',
    '--pad-ack-model', 'octoshock-2.2.2-digital',
    '--cd-firmware-model', 'octoshock-2.2.2',
    '--cd-cold-status-model', 'octoshock-2.2.2',
    '--cd-toc-seek-model', 'octoshock-2.2.2',
    '--cd-explicit-seek-model', 'octoshock-2.2.2',
    '--cd-read-start-model', 'octoshock-2.2.2-pipeline',
    '--cd-dma-model', 'octoshock-2.2.2',
    '--gpu-status-model', 'octoshock-2.2.2-raster',
    '--gpu-dma-model', 'octoshock-2.2.2-bounded-quad',
    '--timer1-model', 'octoshock-2.2.2',
    '--timer2-model', 'octoshock-2.2.2',
    '--precise-slice', 'on', '--cpu-return-probe', '--ram-page-probe',
]


def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def require_hash(path: Path, expected: str) -> None:
    if not path.is_file() or digest(path) != expected:
        raise ValueError(f'Wrong or missing file: {path}\nExpected SHA-256: {expected}')


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf8', newline='\n')


def command(argv, log: Path, *, env=None) -> None:
    print(f'Running {Path(str(argv[0])).name}; log: {log}', flush=True)
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w', encoding='utf8') as stream:
        result = subprocess.run([str(v) for v in argv], cwd=ROOT, env=env,
                                stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        print('\n'.join(log.read_text(errors='replace').splitlines()[-35:]), file=sys.stderr)
        raise RuntimeError(f'Command failed ({result.returncode}); see {log}')


def verify_cue(cue: Path) -> list[Path]:
    """Accept the qualified three-track topology, independent of file names."""
    files, layout = [], []
    for line in cue.read_text(encoding='utf-8-sig').splitlines():
        line = line.strip()
        if not line or line.upper().startswith('REM '):
            continue
        match = re.fullmatch(r'FILE\s+"([^"\r\n]+)"\s+BINARY', line, re.I)
        if match:
            files.append((cue.parent / match[1]).resolve(strict=True))
            layout.append('FILE')
        else:
            layout.append(' '.join(line.upper().split()))
    expected = ['FILE', 'TRACK 01 MODE2/2352', 'INDEX 01 00:00:00',
                'FILE', 'TRACK 02 AUDIO', 'INDEX 00 00:00:00', 'INDEX 01 00:02:00',
                'FILE', 'TRACK 03 AUDIO', 'INDEX 00 00:00:00', 'INDEX 01 00:02:00']
    if layout != expected or len(files) != 3:
        raise ValueError('Expected the USA three-track BIN/CUE with the original audio pregaps; ISO/CHD and merged-track dumps are not qualified.')
    for index, (path, (size, sha)) in enumerate(zip(files, TRACKS), 1):
        print(f'Checking disc track {index}...', flush=True)
        if path.stat().st_size != size:
            raise ValueError(f'Track {index} has the wrong size: {path}')
        require_hash(path, sha)
    return files


def extract_executable(track: Path) -> bytes:
    """Read the standard ISO9660 directory tree in raw Mode2 sectors."""
    sys.path.insert(0, str(ROOT / 'tools'))
    from prepare_disc import parse_root_entries
    with track.open('rb') as stream:
        def user(lba):
            stream.seek(lba * 2352)
            sector = stream.read(2352)
            if len(sector) != 2352 or sector[:12] != b'\0' + b'\xff' * 10 + b'\0':
                raise ValueError(f'Invalid raw data sector {lba}')
            return sector[24:2072]

        def span(extent, size):
            return b''.join(user(extent + i) for i in range((size + 2047) // 2048))[:size]

        pvd = user(16)
        if pvd[1:6] != b'CD001':
            raise ValueError('Missing ISO9660 primary volume')
        extent, size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
        for part in ('TEKKEN3', 'SLUS_004.02'):
            extent, size = parse_root_entries(span(extent, size))[part]
        result = span(extent, size)
    if hashlib.sha256(result).hexdigest() != EXE_SHA:
        raise ValueError('Extracted executable does not match the qualified USA revision')
    return result


def prepare_movie(source: Path | None) -> Path:
    import bk2_intake
    from bk2_to_psxrti import convert
    archive = PROJECT / 'tasvideos-4164.zip'
    if source is None and not archive.exists():
        print(f'Downloading Spikestuff\'s tool-assisted speedrun from {PUBLICATION}', flush=True)
        request = urllib.request.Request(PUBLICATION + '?handler=Download',
                                         headers={'User-Agent': 'psxrecomp-tasreplays/1.0'})
        with urllib.request.urlopen(request, timeout=60) as response:
            data = response.read(bk2_intake.MAX_BYTES + 1)
        _, receipt = bk2_intake.inspect(data)
        if receipt['movie_sha256'] != MOVIE_SHA:
            raise ValueError('The downloaded TAS changed; no unverified movie will be used')
        archive.write_bytes(data)
    selected = source or archive
    if selected.stat().st_size > bk2_intake.MAX_BYTES:
        raise ValueError('Movie exceeds the supported size')
    payload, receipt = convert(selected.read_bytes())
    if (receipt['movie_sha256'], receipt['frame_count'], receipt['pad_words_le_sha256']) != (MOVIE_SHA, 7974, WORDS_SHA):
        raise ValueError('Expected the unchanged 7,974-input Spikestuff movie4164')
    route = PROJECT / 'tekken3.psxrti'
    route.write_bytes(payload)
    write_json(PROJECT / 'movie.json', receipt)
    return route


def setup(args) -> None:
    if os.name != 'nt':
        raise ValueError('The reproducible build is currently qualified for Windows x64 with MinGW GCC.')
    if sys.version_info < (3, 11):
        raise ValueError('Python 3.11 or newer is required')
    for executable in ('git', 'gcc', 'g++', 'cmake', 'ninja'):
        if not shutil.which(executable):
            raise ValueError(f'{executable} is missing from PATH. See tools/tasreplays/README.md.')
    machine = subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip()
    if machine != 'x86_64-w64-mingw32':
        raise ValueError(f'Expected x86_64-w64-mingw32 GCC; found {machine}')
    macros = subprocess.check_output(['gcc', '-dM', '-E', '-include', '_mingw.h', '-'],
                                     input='', text=True)
    if not re.search(r'^#define _UCRT\b', macros, re.M):
        raise ValueError('Use a UCRT MinGW toolchain (WinLibs UCRT or MSYS2 UCRT64), not MSVCRT.')
    if subprocess.check_output(['git','-C',str(ROOT),'status','--porcelain'],text=True).strip():
        raise ValueError('Commit all candidate source before setup')
    head=subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip()
    bash=Path(shutil.which('git')).resolve().parents[1]/'bin/bash.exe'
    if not bash.is_file(): raise ValueError('Git for Windows bash is required for BIOS fingerprint verification')
    bios = args.bios.resolve(strict=True)
    require_hash(bios, BIOS_SHA)
    tracks = verify_cue(args.disc.resolve(strict=True))
    PROJECT.mkdir(parents=True, exist_ok=False)
    cue=args.disc.resolve(strict=True)
    cache=(args.cache or PROJECT/'input-cache').resolve()
    def store_private(data,sha,name):
        folder=cache/sha;folder.mkdir(parents=True,exist_ok=True);target=folder/name
        if not target.exists():
            with target.open('xb') as stream:stream.write(data)
        require_hash(target,sha);return target
    boot=store_private(extract_executable(tracks[0]),EXE_SHA,'SLUS_004.02')
    staged_bios=store_private(bios.read_bytes(),BIOS_SHA,'SCPH1001.BIN')
    q=lambda path:json.dumps(path.as_posix())
    bios_profile=PROJECT/'bios.toml'
    profile=(ROOT/'bios/SCPH1001.toml').read_text()
    for key,path in [('rom',staged_bios),('seeds',ROOT/'recompiler/seeds/phase2_ghidra_seeds.json'),('out_dir',ROOT/'generated')]:
        profile=re.sub(r'^'+key+r'\s*=.*$',lambda _:key+' = '+q(path),profile,flags=re.M)
    bios_profile.write_text(profile,encoding='utf8')
    route = prepare_movie(args.movie)
    tape = PROJECT / 'octoshock222-cold-random.psxrng'
    if not tape.exists():
        command([sys.executable, HERE / 'external/source_random_tape.py', tape], PROJECT / 'random-tape.log')
    require_hash(tape, TAPE_SHA)
    game = PROJECT / 'game.toml'
    def q(path):
        return json.dumps(path.as_posix())
    game.write_text(f'''[game]
name = "Tekken 3 TAS test"
id = "SLUS-00402"
exe = {q(boot)}
load_address = "0x80010000"
entry_pc = "0x80079C70"
text_size = "0x121000"
stack_base = "0x801FFFF0"
[recompiler]
seeds = {q(HERE / 'tekken3-seeds.txt')}
bios_config = {q(bios_profile)}
strict = true
out_dir = {q(PROJECT / 'generated')}
[runtime]
window_title = "Tekken 3 - Spikestuff TAS"
bios_hle = false
[video]
renderer = "software"
''', encoding='utf8', newline='\n')
    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=gcc', '-DCMAKE_CXX_COMPILER=g++']
    command(['cmake', '-S', ROOT / 'recompiler', '-B', TOOLS, *common,
             '-DPSXRECOMP_ENABLE_CHD=ON', '-DBUILD_TESTING=ON',
             '-DPython3_EXECUTABLE=' + sys.executable], PROJECT / 'configure-tools.log')
    command(['cmake', '--build', TOOLS, '--parallel', args.jobs], PROJECT / 'build-tools.log')
    command(['ctest', '--test-dir', TOOLS, '--output-on-failure', '-j', args.jobs], PROJECT / 'test-tools.log')
    command([TOOLS / 'psxrecomp-bios.exe', '--config', bios_profile,
             '--rom', staged_bios, '--out-dir', ROOT / 'generated'], PROJECT / 'generate-bios.log')
    fingerprint=subprocess.check_output([str(bash),(ROOT/'tools/bios_emitter_fingerprint.sh').as_posix(),bios_profile.as_posix()],cwd=ROOT,text=True).strip()
    if not re.fullmatch('[0-9a-f]{64}',fingerprint):raise ValueError('invalid BIOS emitter fingerprint')
    (ROOT/'generated/SCPH1001.emitter.sha').write_text(fingerprint+'\n')
    command([TOOLS / 'psxrecomp-game.exe', '--config', game], PROJECT / 'generate-game.log')
    # Check generated text against the winning build, independent of CRLF/LF.
    expected = json.loads((HERE / 'tekken3-codegen.json').read_text())
    for name, sha in expected.items():
        path = PROJECT/name.removeprefix('build/tekken3/') if name.startswith('build/tekken3/') else ROOT/name
        if hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest() != sha:
            raise ValueError(f'Generated source differs from the qualified build: {name}')
    command(['cmake', '-S', HERE, '-B', NATIVE, *common,
             '-DPSX_RECOMP_UI=OFF', '-DPSX_NETPLAY=OFF', '-DPSX_REWIND=OFF',
             '-DPSX_SETUP_WIZARD=OFF', '-DPSX_DEBUG_TOOLS=ON', '-DPSX_ENABLE_VULKAN=OFF',
             '-DTAS_PROJECT_DIR='+str(PROJECT),'-DPSXRECOMP_BIOS_PROFILE='+str(bios_profile),
             '-D_psxrt_bash='+str(bash),
             '-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE',
             '-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE'], PROJECT / 'configure-native.log')
    command(['cmake', '--build', NATIVE, '--parallel', args.jobs], PROJECT / 'build-native.log')
    if subprocess.check_output(['git','-C',str(ROOT),'status','--porcelain'],text=True).strip() or subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip()!=head:
        raise ValueError('Source changed during setup')
    build_info = {'schema': 'psx-tas-setup-v1','source_head':head,'tracks':[str(p) for p in tracks],
                  'source_tree':subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD^{tree}'],text=True).strip(),
                  'original_bios':str(bios),'bios_profile':str(bios_profile),'bios_profile_sha256':digest(bios_profile),
                  'bios_emitter_fingerprint':fingerprint,'qualified_codegen_sha256':expected,
                  'disc': str(cue), 'bios': str(staged_bios),
                  'replay_speed_control': 1,
                  'game': str(game), 'route': str(route), 'tape': str(tape),
                  'executable': str(NATIVE / 'Tekken3-TAS.exe'),
                  'executable_sha256': digest(NATIVE / 'Tekken3-TAS.exe'),
                  'compiler': subprocess.check_output(['gcc', '--version'], text=True).splitlines()[0]}
    write_json(PROJECT / 'setup.json', build_info)
    print('Build ready. Run: python tools/tasreplays/tekken3.py run', flush=True)


def compare_replay(run: Path) -> dict:
    expected = dict(line.split() for line in gzip.decompress((HERE / 'tekken3-reference.tsv.gz').read_bytes()).decode().splitlines())
    count = 0
    with (run / 'ram-pages.tsv').open() as stream:
        for line in stream:
            values = line.split()
            if not values or not values[0].isdigit():
                continue
            frame = int(values[0])
            if frame != count + 1 or len(values) != 514:
                raise ValueError(f'Invalid RAM checkpoint sequence at return {frame}')
            actual = hashlib.sha256(' '.join(values).encode()).hexdigest()
            if expected.get(str(frame)) != actual:
                raise ValueError(f'Playback diverged at return {frame}; retained RAM hashes and clock: {run / "ram-pages.tsv"}')
            count += 1
    if count != 8399:
        raise ValueError(f'Incomplete replay: {count} of 8399 return checkpoints')
    complete = json.loads((run / 'complete.json').read_text())
    if (complete['frame'], complete['input_frames'], complete['neutral_tail_ticks'], complete['applied_words_sha256']) != (8400, 7974, 426, WORDS_SHA):
        raise ValueError('Original controller input or ending boundary did not match')
    return {'status': 'pass', 'original_inputs': 7974, 'compared_returns': count,
            'reference': 'integrated202 native victory; all7974 original-input returns also matched the independent Octoshock reference',
            'expected_victory_time': '8.80', 'end_frame': 8400,
            'limitation': 'Replay ends at the observed victory. Later CDDA Play seek remains unqualified.'}


def run(args) -> None:
    setup_path = PROJECT / 'setup.json'
    if not setup_path.exists():
        raise ValueError('Run the setup command with your disc and SCPH1001 BIOS first.')
    info = json.loads(setup_path.read_text())
    if args.speed != '1' and info.get('replay_speed_control') != 1:
        raise ValueError('Rerun setup to build a player with replay speed control before using --speed.')
    require_hash(Path(info['executable']), info['executable_sha256'])
    require_hash(Path(info['bios']), BIOS_SHA)
    require_hash(Path(info['tape']), TAPE_SHA)
    verify_cue(Path(info['disc']))
    run_dir = (args.output or ROOT / 'build/tasreplays-runs' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')).resolve()
    if run_dir.exists():
        raise ValueError(f'Choose a new run directory: {run_dir}')
    argv = [sys.executable, HERE / 'run_native.py', run_dir,
            '--exe', info['executable'], '--game', info['game'], '--disc', info['disc'],
            '--bios', info['bios'], '--route', info['route'], '--cd-source-clock-tape', info['tape'],
            '--storage-budget-mib','1536','--neutral-tail', '426', '--timeout', str(args.timeout), '--checkpoint-every', '300',
            '--renderer', 'software', '--speed', args.speed, *PROFILE]
    if not args.headless:
        argv.append('--show')
    # The runtime writes settings beside its executable; the runner creates an
    # isolated copy for every run, with digital P1, absent P2, and no cards.
    command(argv, run_dir.parent / (run_dir.name + '-launch.log'))
    result = compare_replay(run_dir)
    write_json(run_dir / 'verification.json', result)
    print(f'PASS: all 7,974 original inputs and 8,399 RAM/clock checkpoints match the 8.80 victory.\nEvidence: {run_dir}')


def main():
    global PROJECT,TOOLS,NATIVE
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    prepare = sub.add_parser('setup', help='verify owned assets, download the TAS, generate and build')
    prepare.add_argument('--disc', type=Path, required=True, help='USA three-track .cue')
    prepare.add_argument('--bios', type=Path, required=True, help='SCPH1001.BIN')
    prepare.add_argument('--project',type=Path,help='fresh private generated/build directory')
    prepare.add_argument('--cache',type=Path,help='verified private boot/firmware cache')
    prepare.add_argument('--tools-dir',type=Path,help='reusable tools build; configured and tested for this source')
    prepare.add_argument('--movie', type=Path, help='optional original BK2/download ZIP; otherwise download movie4164')
    prepare.add_argument('--jobs', type=int, default=min(12, os.cpu_count() or 1))
    play = sub.add_parser('run', help='play through the victory and compare every RAM/clock checkpoint')
    play.add_argument('--project',type=Path,help='directory containing setup.json')
    play.add_argument('--headless', action='store_true')
    play.add_argument('--speed', choices=('1', '2', '4', '8', '16', '32', '64', 'max'), default='1',
                      help='visible replay speed cap; actual speed depends on the host')
    play.add_argument('--timeout', type=int, default=1800, help='host seconds; increase for a slower machine')
    play.add_argument('--output', type=Path, help='new run directory; defaults to a unique build subdirectory')
    args = parser.parse_args()
    try:
        if args.project:
            PROJECT=args.project.resolve()
            if PROJECT==ROOT or PROJECT.is_relative_to(ROOT):raise ValueError('Explicit project must be outside source')
            NATIVE=PROJECT/'native'
        if args.action=='setup':TOOLS=args.tools_dir.resolve() if args.tools_dir else PROJECT/'tools'
        if args.action == 'setup':
            if not 1 <= args.jobs <= 64:
                raise ValueError('--jobs must be in 1..64')
            setup(args)
        else:
            if args.timeout < 1:
                raise ValueError('--timeout must be positive')
            if args.headless and args.speed != '1':
                raise ValueError('--speed requires visible playback; --headless is already uncapped')
            run(args)
    except (ValueError, RuntimeError, OSError, KeyError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
