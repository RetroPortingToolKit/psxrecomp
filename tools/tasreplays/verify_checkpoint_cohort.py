"""Check that resumes reproduce an uninterrupted run, and that the check can fail.

With one game command (a debug build, run headless), this records:
  A1           a cold run that captures at every point;
  A2           a second cold run, compared with A1 (the run is deterministic);
  resumeN      for N in 500, 1500 and 2500, a run resumed from A1's capture
               requested at N, compared with A1 at every later point;
  negative     a resume from 2500 with timer 2's counter changed by one after
               the restore, which must differ from A1 at a later point.
Points are 500, 501, 560, 1500, 1501, 1560, 2500, 2501, 2560 and the terminal
frame. OUTPUT/results.json holds every command, exit status, duration and
comparison. Exit status 0 when every comparison passes and the negative run
is caught.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

from compare_checkpoint_runs import compare_runs
from run_checkpoint import run

RESUMES = (500, 1500, 2500)


def source_commit():
    try:
        return subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=Path(__file__).parent,
                                       text=True, encoding='utf-8', errors='replace',
                                       stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def cohort(output, command, terminal, port=4370, timeout=3600.0, resumes=RESUMES, runner=run):
    output = Path(output)
    output.mkdir(parents=True)
    points = sorted({p for k in resumes for p in (k, k + 1, k + 60)} | {terminal})
    report = {'source_commit': source_commit(), 'command': command, 'terminal': terminal,
              'points': points, 'runs': {}, 'comparisons': {}, 'passed': False}

    def save():
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    def record(name, start=0, resume=None, perturb=None):
        save_at = [p for p in points if p > start]
        code, rows = runner(output / name, command, save_at, resume, perturb, port, timeout)
        report['runs'][name] = json.loads((output / name / 'run.json').read_text(encoding='utf-8'))
        save()
        if code:
            raise RuntimeError(f'{name} exited {code}')
        return rows

    try:
        taken = {row['requested_frame']: row for row in record('A1')}
        cases = [('A2', 0, None)] + [(f'resume{k}', k, None) for k in resumes] + \
                [('negative', resumes[-1], 'timer2')]
        for name, requested, perturb in cases:
            start, resume = 0, None
            if requested:
                if requested not in taken:
                    raise RuntimeError(f'A1 has no capture requested at {requested}')
                start, resume = taken[requested]['frame'], output / 'A1' / taken[requested]['state']
            record(name, start, resume, perturb)
            result = compare_runs(output / 'A1', output / name, start)
            result['requested_start'] = requested
            passed = not result['passed'] if perturb else result['passed']
            result['expected'] = 'differ' if perturb else 'identical'
            report['comparisons'][name] = result
            save()
            print('COMPARE', name, 'PASS' if passed else 'FAIL', flush=True)
            if not passed:
                raise RuntimeError(f'{name}: comparison failed')
        report['passed'] = True
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        report['error'] = str(error)
    save()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', type=Path, help='new directory for the runs and results.json')
    parser.add_argument('--terminal', type=int, required=True, help='last frame; must exceed 2560')
    parser.add_argument('--port', type=int, default=4370)
    parser.add_argument('--timeout', type=float, default=3600.0, help='seconds per run')
    parser.add_argument('command', nargs=argparse.REMAINDER, help='-- then the game command')
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('give the game command after "--"')
    if args.terminal <= 2560:
        parser.error('--terminal must exceed 2560')
    report = cohort(args.output, command, args.terminal, args.port, args.timeout)
    print('COHORT', 'PASS' if report['passed'] else 'FAIL', args.output, flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
