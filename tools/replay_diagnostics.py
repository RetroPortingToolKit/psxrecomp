"""Replay an offline PSX playtest diagnostic recording in a private hidden process.

Use the recording's original build, disc and game config. Starting cards and mod
choices come from the recording. Segments created by save-state loads also
restore their complete baseline.pst. The runtime verifies the build/mod identity
and, on every VBlank, the accepted pad state and a guest-RAM digest; it stops at
the first divergence ("first divergence at sequence N"). Network recordings and
rewind segments are not supported for playback. A torn final row (an interrupted
recording) is ignored. Personal cards/settings are never overwritten.

The retail OpenBIOS bundled in the release package is used unless --bios is
given. On Linux, use the native executable and payload extracted from the
original AppImage. Direct AppImage execution is rejected because AppRun seeds
user data. Replay uses a hidden window (the headless frontend ignores
controller sources) and runs at the game's normal speed unless the host allows
more.

Developer builds additionally accept --input-only for exploring inputs across
fixes. That mode still checks accepted pad bytes and presence, but deliberately
does not verify build/mod identity or RAM. V1 routes require an explicit
--legacy-connected P1/P2 assumption because their format did not record it.
An input-only completion is never an exact replay, and player builds reject it.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib

FORMATS = (b'# psxrecomp-input-v1', b'# psxrecomp-input-v2')
ENV_PREFIXES = ('PSX_', 'RNET_', 'LNG_', 'PSX_DIAGNOSTIC_', 'RECOMP_')
LOG_PREFIX = 'psxrecomp.diagnostics: '


def native_executable(path):
    executable = path.resolve(strict=True)
    with executable.open('rb') as source:
        header = source.read(11)
    if (executable.suffix.lower() == '.appimage' or
            (header[:4] == b'\x7fELF' and header[8:11] in (b'AI\x01', b'AI\x02'))):
        raise ValueError(
            'Direct AppImage replay is unsupported: AppRun would seed user data. '
            'Extract the original image with --appimage-extract, then use '
            '--exe squashfs-root/usr/bin/GameRecomp '
            '--game squashfs-root/usr/share/gamerecomp/game.toml')
    return executable


def copy_native_libraries(executable, output):
    # Dereference each soname link into the private directory. A link back to
    # the extracted mount or original installation would defeat isolation.
    roots = [executable.parent]
    if executable.parent.name == 'bin':
        roots.append(executable.parent.parent / 'lib')
    destination = output / 'lib'
    for root in roots:
        for library in root.glob('*.so*'):
            if not library.is_file():
                continue
            destination.mkdir(exist_ok=True)
            target = destination / library.name
            if not target.exists():
                shutil.copy2(library, target, follow_symlinks=True)


def replay_environment(output, trace, source_env):
    env = {key: value for key, value in source_env.items()
           if not key.startswith(ENV_PREFIXES)
           and key not in ('APPIMAGE', 'APPDIR', 'RECOMP_APPIMAGE_PATH', 'LD_LIBRARY_PATH')}
    # Keep audio emulation active, with no unattended speaker output.
    env['SDL_AUDIODRIVER'] = 'dummy'
    env['SDL_AUDIO_DRIVER'] = 'dummy'
    env['PSX_DIAGNOSTIC_REPLAY'] = str(trace)
    # Use copied libraries before normal system libraries, without inheriting
    # dependencies from another AppImage mount or a developer installation.
    env['LD_LIBRARY_PATH'] = os.pathsep.join((str(output / 'lib'), str(output)))
    if (output / 'diagnostic-baseline.pst').is_file():
        env['PSX_LOAD_SLOT'] = '0'
    return env


def recording_rows(recording):
    """Validate the header; return the byte length of the complete prefix and
    the number of complete rows."""
    with recording.open('rb') as source:
        if source.readline().rstrip(b'\r\n') not in FORMATS:
            raise ValueError('Expected a psxrecomp-input-v1 or psxrecomp-input-v2 diagnostic recording')
        for prefix in (b'# execution ', b'# plan ', b'# build '):
            if not source.readline().startswith(prefix):
                raise ValueError('Incomplete diagnostic identity header')
        end, count = source.tell(), 0
        while True:
            line = source.readline()
            if not line.endswith(b'\n'):
                break  # torn or absent final row
            if not line.startswith(b'N '):
                raise ValueError('Unexpected record in diagnostic recording')
            end, count = source.tell(), count + 1
    return end, count


def prepare(args):
    recording = args.recording.resolve()
    if recording.is_dir():
        recording /= 'inputs.csv'
    metadata = recording.parent / 'session.json'
    session = json.loads(metadata.read_text(encoding='utf-8')) if metadata.exists() else {}
    if session.get('netplay'):
        raise ValueError('Netplay recording: useful for inspection, but offline replay is not supported')
    if session.get('baseline_unavailable'):
        raise ValueError('This segment started from a rewind restore, whose machine image was not '
                         'captured: it is useful for inspection, but cannot be replayed')
    baseline = None
    if 'baseline' in session:
        if session['baseline'] != 'baseline.pst':
            raise ValueError('Unsupported diagnostic baseline filename')
        baseline = recording.parent / 'baseline.pst'
        if not baseline.is_file() or not 0 < baseline.stat().st_size <= 64 * 1024 * 1024:
            raise ValueError('Recording has no complete restored-state baseline')
    complete, count = recording_rows(recording)
    if count < 2:
        raise ValueError('Recording has no complete input interval')
    executable = native_executable(args.exe)
    game = (args.game or executable.parent / 'game.toml').resolve(strict=True)
    config = game.read_text(encoding='utf-8')
    game_values = tomllib.loads(config)['game']
    section = re.search(r'^\[game\]\s*$.*?(?=^\[|\Z)', config, re.M | re.S)
    if not section:
        raise ValueError('Game configuration has no [game] table')
    text = section.group()
    for key in ('exe', 'disc'):
        path = args.disc.resolve() if key == 'disc' else Path(game_values[key])
        if not path.is_absolute():
            path = game.parent / path
        value = json.dumps(path.resolve().as_posix(), ensure_ascii=False)
        text = re.sub(r'^(' + key + r'\s*=\s*).+$', lambda m: m[1] + value, text, count=1, flags=re.M)
    config = config[:section.start()] + text + config[section.end():]
    cards = []
    for slot in (1, 2):
        card, absent = recording.parent / f'card{slot}.mcd', recording.parent / f'card{slot}.absent'
        if card.exists() == absent.exists():
            raise ValueError(f'Expected one starting card or absence marker for slot {slot}')
        if card.exists() and card.stat().st_size != 131072:
            raise ValueError(f'Invalid starting card size in slot {slot}')
        cards.append(card if card.exists() else None)
    state = recording.parent / 'mods-state.toml'
    if not state.is_file():
        raise ValueError('Recording is missing its starting mod choices')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    target = output / executable.name
    shutil.copy2(executable, target)
    # Windows release dependencies live beside the executable. An isolated
    # replay must work without relying on the developer's compiler PATH.
    for dependency in executable.parent.glob('*.dll'):
        shutil.copy2(dependency, output / dependency.name)
    copy_native_libraries(executable, output)
    for name in ('assets', 'mods', 'bios'):
        # Extracted AppImages place the ELF under usr/bin and the title
        # resources beside the explicitly supplied payload game.toml.
        for root in (executable.parent, game.parent):
            source = root / name
            if source.is_dir():
                shutil.copytree(source, output / name)
                break
    (output / 'mods').mkdir(exist_ok=True)
    shutil.copy2(state, output / 'mods/state.toml')
    (output / 'game.toml').write_text(config, encoding='utf-8')
    if (game.parent / 'game_options.toml').is_file():
        shutil.copy2(game.parent / 'game_options.toml', output / 'game_options.toml')
    settings_path = args.settings or (recording.parent / 'settings.toml'
        if (recording.parent / 'settings.toml').exists() else executable.parent / 'settings.toml')
    settings = settings_path.read_text(encoding='utf-8') if settings_path.exists() else ''
    settings = re.sub(r'^\[memcard\]\s*$.*?(?=^\[|\Z)', '', settings, flags=re.M | re.S)
    settings += '\n[memcard]\n' + ''.join(f'enable{i+1} = {str(card is not None).lower()}\n' for i, card in enumerate(cards))
    (output / 'settings.toml').write_text(settings, encoding='utf-8')
    (output / 'saves').mkdir()
    for i, card in enumerate(cards):
        if card:
            shutil.copy2(card, output / f'saves/card{i+1}.mcd')
    if baseline is not None:
        entry = int(game_values['entry_pc'], 0)
        if not 0 < entry <= 0xffffffff:
            raise ValueError('Invalid game entry PC')
        # The runtime chooses the BIOS token by checksum, rather than filename.
        # Both private locations let the active BIOS select its own slot.
        for token in ('openbios', 'scph1001'):
            directory = output / 'saves' / token
            directory.mkdir()
            shutil.copy2(baseline, directory / f'state_{entry:08X}_slot00.pst')
        shutil.copy2(baseline, output / 'diagnostic-baseline.pst')
    trace = output / 'replay-inputs.csv'
    with recording.open('rb') as source, trace.open('xb') as dest:
        remaining = complete
        while remaining:
            chunk = source.read(min(remaining, 1024 * 1024))
            if not chunk:
                raise ValueError('Recording changed while preparing replay')
            dest.write(chunk)
            remaining -= len(chunk)
    return output, target, trace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording', type=Path, help='Session directory or inputs.csv')
    parser.add_argument('--exe', type=Path, required=True, help='Original native PSX playtest build')
    parser.add_argument('--bios', type=Path, help='BIOS image (default: bundled OpenBIOS)')
    parser.add_argument('--disc', type=Path, required=True)
    parser.add_argument('--game', type=Path)
    parser.add_argument('--settings', type=Path, help='Settings used when recording')
    parser.add_argument('--output', type=Path, required=True, help='New isolated directory')
    parser.add_argument('--timeout', type=float, default=900)
    parser.add_argument('--debug-port', type=int, help='Developer build TCP inspection port')
    parser.add_argument('--input-only', action='store_true', help='Developer build only: apply inputs across fixes without claiming build/mod/RAM equivalence')
    parser.add_argument('--legacy-connected', choices=('00','10','01','11'), help='Explicit P1/P2 presence assumption for an input-only v1 route')
    args = parser.parse_args()
    disc = args.disc.resolve(strict=True)
    bios = args.bios.resolve(strict=True) if args.bios else None
    output, target, trace = prepare(args)
    env = replay_environment(output, trace, os.environ)
    if args.input_only:
        env['PSX_DIAGNOSTIC_INPUT_ONLY'] = '1'
        if args.legacy_connected:
            env['PSX_DIAGNOSTIC_LEGACY_CONNECTED'] = args.legacy_connected
    baseline = output / 'diagnostic-baseline.pst'
    if baseline.is_file():
        env['PSX_DIAGNOSTIC_BASELINE'] = str(baseline)
    argv = [str(target), '--hidden-window', '--no-launcher', '--game', str(output / 'game.toml'),
            '--disc', str(disc), '--memcard-dir', str(output / 'saves')]
    if bios:
        argv += ['--bios', str(bios)]
    if args.debug_port:
        argv += ['--debug-port', str(args.debug_port)]
    log_path = output / 'replay.log'
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    result = 1
    with log_path.open('wb') as log:
        process = subprocess.Popen(argv, cwd=output, env=env, stdout=log, stderr=log, creationflags=flags)
        deadline = time.monotonic() + args.timeout
        try:
            while time.monotonic() < deadline:
                text = log_path.read_text(encoding='utf-8', errors='replace')
                complete = 'input-only route complete (' if args.input_only else 'input replay complete ('
                if LOG_PREFIX + complete in text:
                    if not baseline.is_file() or LOG_PREFIX + 'replay baseline restored' in text:
                        result = 0
                    break
                if LOG_PREFIX + 'replay stopped at sequence ' in text or process.poll() is not None:
                    break
                time.sleep(.1)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=15)
    success = 'Input-only route completed; guest state NOT verified. ' if args.input_only else 'Replay verified. '
    print((success if result == 0 else 'Replay failed or timed out. ') + f'Log: {log_path}')
    raise SystemExit(result)


if __name__ == '__main__':
    main()
