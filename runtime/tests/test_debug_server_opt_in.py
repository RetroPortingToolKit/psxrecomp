"""Exercise production listener opt-in and Archipelago writes in an isolated player.

No disc, user settings or native saves are used. Each case boots bundled
OpenBIOS headlessly with dummy audio, then terminates its own test process.
"""
import argparse
import json
import tempfile
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--runtime', type=Path, required=True)
parser.add_argument('--bios', type=Path, required=True)
parser.add_argument('--manifest', type=Path, required=True)
parser.add_argument('--scratch', type=Path, required=True)
args = parser.parse_args()
EXE, BIOS, MANIFEST = args.runtime.resolve(), args.bios.resolve(), args.manifest.resolve()

def request(port, command):
    with socket.create_connection(('127.0.0.1', port), timeout=2) as sock:
        sock.sendall(json.dumps({'id': 47, **command}).encode() + b'\n')
        response = sock.makefile('rb').readline()
    return json.loads(response)

def listening(port):
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=0.1):
            return True
    except OSError:
        return False

def run(name, env_choice, mod_choice, expected):
    folder = ROOT / 'protocol' / name
    folder.mkdir(parents=True, exist_ok=True)
    exe = folder / EXE.name
    shutil.copy2(EXE, exe)
    (folder / 'bios').mkdir(exist_ok=True)
    shutil.copy2(BIOS, folder / 'bios/openbios.bin')
    package = folder / 'mods/bundled/psx.integration.debug-server/1.0.0'
    package.mkdir(parents=True, exist_ok=True)
    shutil.copy2(MANIFEST, package / 'manifest.toml')
    if mod_choice is not None:
        (folder / 'mods/state.toml').write_text(
            'format_version = 2\n[[package]]\nid = "psx.integration.debug-server"\nversion = "1.0.0"\n'
            '[[feature]]\npackage_id = "psx.integration.debug-server"\nid = "debug-server"\n'
            f'enabled = {str(mod_choice).lower()}\n')
    env = dict(os.environ, SDL_AUDIODRIVER='dummy', PSX_VOLUME='0', TEMP=str(folder), TMP=str(folder))
    env.pop('PSX_DEBUG_SERVER', None)
    if env_choice is not None:
        env['PSX_DEBUG_SERVER'] = env_choice
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    with (folder / 'stdout.log').open('wb') as out, (folder / 'stderr.log').open('wb') as err:
        proc = subprocess.Popen([str(exe), '--headless', '--bios', str(BIOS), '--debug-port', str(port),
                                 '--memcard-dir', str(folder / 'saves')], cwd=folder, env=env,
                                stdout=out, stderr=err, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        try:
            deadline = time.monotonic() + (12 if expected else 3)
            connected = False
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise AssertionError(f'{name} exited {proc.returncode}: {(folder / "stderr.log").read_text(errors="replace")[-4000:]}')
                if listening(port):
                    connected = True
                    break
                time.sleep(0.1)
            assert connected == expected, (name, connected, expected)
            if connected:
                assert request(port, {'cmd': 'ping'})['ok']
                # BIOS-only fixture: unused RAM well above BIOS scratch.
                addr = '0x801e1000'
                data = bytes(range(128))
                start = time.monotonic()
                assert request(port, {'cmd': 'write_bulk', 'addr': addr, 'hex': data.hex()}) == {'id':47, 'ok':True}
                elapsed = time.monotonic() - start
                result = request(port, {'cmd': 'read_ram', 'addr': addr, 'len': len(data)})
                assert bytes.fromhex(result['hex']) == data, result
                for bad in ['001122gg', '001', '00' * 1025]:
                    assert not request(port, {'cmd': 'write_bulk', 'addr': addr, 'hex': bad})['ok']
                    assert bytes.fromhex(request(port, {'cmd': 'read_ram', 'addr': addr, 'len': len(data)})['hex']) == data
                assert not request(port, {'cmd': 'write_bulk', 'addr': '0x801fffff', 'hex': '0011'})['ok']
                print(f'PASS {name}: listener enabled, 128-byte round-trip, atomic rejection; write {elapsed*1000:.1f} ms', flush=True)
            else:
                assert 'executing from PC=' in (folder / 'stdout.log').read_text(errors='replace'), name
                print(f'PASS {name}: running with no listener (explicit debug port supplied)', flush=True)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)

with tempfile.TemporaryDirectory(prefix='debug-server-opt-in-', dir=args.scratch) as scratch:
    ROOT = Path(scratch)
    run('env-on', '1', None, True)
    run('default-off', None, None, False)
    run('mod-on', None, True, True)
    run('mod-off', None, False, False)
    run('env-off-overrides-mod', '0', True, False)
    run('env-on-overrides-mod', '1', False, True)
