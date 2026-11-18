"""Compare a resumed run with an uninterrupted one at every later capture.

Both directories are run_checkpoint.py outputs. For each capture the candidate
requested after the frame it resumed from, the baseline must hold a capture
requested at the same frame, taken at the same frame, with the same cycle and
RAM digest, and a state image identical section for section. A capture
missing from either side fails. Exit status 0 when every comparison passes.
"""
import argparse
import json
from pathlib import Path

from compare_boot_states import compare


def load_captures(run):
    return json.loads((Path(run) / 'captures.json').read_text(encoding='utf-8'))


def compare_runs(baseline, candidate, start):
    """start: the frame the candidate resumed from (0 for a cold run)."""
    baseline, candidate = Path(baseline), Path(candidate)
    left = {row['requested_frame']: row for row in load_captures(baseline)}
    right = {row['requested_frame']: row for row in load_captures(candidate)}
    result = {'start': start, 'captures': {}}
    for requested in sorted(k for k in left.keys() | right.keys() if k > start):
        a, b = left.get(requested), right.get(requested)
        if a is None or b is None:
            result['captures'][str(requested)] = {
                'identical': False, 'missing': 'baseline' if a is None else 'candidate'}
            continue
        fields = {key: [a[key], b[key]] for key in ('frame', 'cycle', 'ram_digest') if a[key] != b[key]}
        state = compare(baseline / a['state'], candidate / b['state'])
        result['captures'][str(requested)] = {
            'identical': state['identical'] and not fields, 'fields': fields,
            'differences': state['differences']}
    if not result['captures']:
        raise ValueError('no capture after the resume frame to compare')
    result['passed'] = all(entry['identical'] for entry in result['captures'].values())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path, help='the uninterrupted run')
    parser.add_argument('candidate', type=Path, help='the run to check')
    parser.add_argument('--start', type=int, default=0, help='frame the candidate resumed from')
    parser.add_argument('--output', type=Path, help='write the JSON report here (must not exist)')
    args = parser.parse_args()
    try:
        result = compare_runs(args.baseline, args.candidate, args.start)
    except (OSError, ValueError, KeyError) as error:
        result = {'passed': False, 'error': str(error)}
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        with args.output.open('x', encoding='utf-8') as stream:
            stream.write(text)
    else:
        print(text, end='')
    print('PASS' if result['passed'] else 'FAIL')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
