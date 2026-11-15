"""Run a debug build once with checkpoint capture and, optionally, a resume.

The command after "--" starts the game (for example a headless build with its
disc). This driver sets the checkpoint environment variables (see
docs/TAS_CHECKPOINTS.md), waits until a capture at or after the last
requested frame has its manifest, then asks the debug server to quit. It
writes OUT/captures.json, one entry per manifest, and OUT/run.json with the
command, exit status and duration. Its exit status is 0 when the run reached
the last capture, otherwise 1, or the game's own status when it exited first
(2 is a refused resume).
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

STATE_NAME = 'tas-state-{frame}.pst'
PERTURB_FIELDS = ('ram', 'irq_timing', 'sched', 'timer2', 'memctrl')


def captures(out, strict=True):
    """Manifests in OUT, sorted by the frame they were taken at. While the game
    runs (strict=False) a manifest still being written is skipped."""
    rows = []
    for path in sorted(Path(out).glob('tas-state-*.pst.json')):
        try:
            m = json.loads(path.read_text(encoding='utf-8'))
        except ValueError:
            if strict:
                raise
            continue
        rows.append({'requested_frame': m['requested_frame'], 'frame': m['frame'],
                     'cycle': m['cycle'], 'ram_digest': m['ram_digest'],
                     'state': path.name[:-len('.json')]})
    return sorted(rows, key=lambda row: row['frame'])


def request_quit(port, timeout=5.0):
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=timeout) as sock:
            sock.sendall(b'{"cmd":"quit"}\n')
            sock.recv(256)
        return True
    except OSError:
        return False


def run(out, command, save_at, resume=None, perturb=None, port=4370, timeout=3600.0,
        poll=0.25):
    out = Path(out)
    out.mkdir(parents=True)
    env = dict(os.environ)
    for name in ('PSX_TAS_SAVE_STATE_AT', 'PSX_TAS_SAVE_STATE_PATH', 'PSX_TAS_RESUME_STATE',
                 'PSX_TAS_RESUME_MANIFEST', 'PSX_TAS_PERTURB_RESTORE'):
        env.pop(name, None)
    env['PSX_TAS_SAVE_STATE_AT'] = ','.join(map(str, save_at))
    env['PSX_TAS_SAVE_STATE_PATH'] = str((out / STATE_NAME).resolve())
    if resume:
        env['PSX_TAS_RESUME_STATE'] = str(Path(resume).resolve())
    if perturb:
        env['PSX_TAS_PERTURB_RESTORE'] = perturb
    last = max(save_at)
    record = {'command': command, 'save_at': list(save_at), 'resume': str(resume) if resume else None,
              'perturb': perturb, 'reached_last_capture': False}
    begin = time.monotonic()
    with (out / 'run.log').open('wb') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            while process.poll() is None:
                if any(row['frame'] >= last for row in captures(out, strict=False)):
                    record['reached_last_capture'] = True
                    if not request_quit(port):
                        process.terminate()
                    try:
                        process.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        process.kill()
                    break
                if time.monotonic() - begin > timeout:
                    process.kill()
                    break
                time.sleep(poll)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
    record['exit_code'] = process.returncode
    record['seconds'] = round(time.monotonic() - begin, 3)
    rows = captures(out)
    (out / 'captures.json').write_text(json.dumps(rows, indent=2) + '\n', encoding='utf-8')
    (out / 'run.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    if record['reached_last_capture']:
        return 0, rows
    return process.returncode or 1, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('out', type=Path, help='new directory for states, manifests and logs')
    parser.add_argument('--save-at', type=int, nargs='+', required=True, metavar='FRAME')
    parser.add_argument('--resume-from', type=Path, help='a state written by an earlier capture')
    parser.add_argument('--perturb-restore', choices=PERTURB_FIELDS)
    parser.add_argument('--port', type=int, default=4370, help='the build\'s debug-server port')
    parser.add_argument('--timeout', type=float, default=3600.0, help='seconds before the run is stopped')
    parser.add_argument('command', nargs=argparse.REMAINDER, help='-- then the game command')
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('give the game command after "--"')
    if any(frame <= 0 for frame in args.save_at):
        parser.error('capture frames start at 1')
    if args.perturb_restore and not args.resume_from:
        parser.error('--perturb-restore needs --resume-from')
    code, rows = run(args.out, command, sorted(set(args.save_at)), args.resume_from,
                     args.perturb_restore, args.port, args.timeout)
    print(f'{len(rows)} capture(s), exit {code}: {args.out}')
    return code


if __name__ == '__main__':
    sys.exit(main())
