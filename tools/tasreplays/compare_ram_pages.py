"""Locate RAM differences at matching completed frontend returns.

Hashes are an index, not proof of equality. Paired raw snapshots are compared
byte for byte and checked against their page hashes. No pages are ignored.
This reports evidence; it does not classify differences or authorize a full TAS.
"""
import argparse
import csv
import hashlib
from itertools import zip_longest
import json
from pathlib import Path

MAGIC = '# psx-ram-pages-v1 page_bytes=4096 ram_bytes=2097152 hash=fnv1a64'
RAM_BYTES = 2097152
PAGE_BYTES = 4096
PAGE_COUNT = RAM_BYTES // PAGE_BYTES


def page_hash(data):
    h = 14695981039346656037
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xffffffffffffffff
    return f'{h:016X}'


def read_pages(path, first_frame=1):
    with path.open(encoding='utf-8-sig', newline='') as stream:
        if stream.readline().rstrip('\r\n') != MAGIC:
            raise ValueError(f'unsupported RAM index: {path}')
        rows = csv.reader(stream, delimiter='\t')
        header = ['frame', 'cycle'] + [f'{p*PAGE_BYTES:06X}' for p in range(PAGE_COUNT)]
        if next(rows, None) != header:
            raise ValueError(f'invalid page columns: {path}')
        previous = 0
        for expected_frame, row in enumerate(rows, first_frame):
            if len(row) != len(header) or int(row[0]) != expected_frame:
                raise ValueError(f'incomplete or discontinuous RAM index: {path}, frame {expected_frame}')
            cycle = int(row[1])
            if cycle <= previous or any(len(h) != 16 or any(c not in '0123456789ABCDEF' for c in h) for h in row[2:]):
                raise ValueError(f'invalid cycle/hash: {path}, frame {expected_frame}')
            previous = cycle
            yield expected_frame, cycle, row[2:]


def validate_capture(directory, expected_frames, snapshot_frames, first_frame=1):
    """Fail if an enabled observer was missing, truncated, or missed a snapshot."""
    count = sum(1 for _ in read_pages(directory / 'ram-pages.tsv', first_frame))
    if count != expected_frames-first_frame+1:
        raise ValueError(f'RAM capture has {count} frames, expected {expected_frames}')
    for frame in snapshot_frames:
        path = directory / f'ram-frame-{frame:06d}.bin'
        if path.stat().st_size != RAM_BYTES:
            raise ValueError(f'incomplete RAM snapshot: {path}')
    return dict(valid=True, frames=count, snapshot_frames=snapshot_frames)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('native', type=Path)
    p.add_argument('output', type=Path, help='new JSON report; raw differences use an adjacent JSONL file')
    p.add_argument('--through-frame', type=int, help='explicit common prefix when terminal observer coverage differs')
    a = p.parse_args()
    if a.through_frame is not None and a.through_frame < 1: raise ValueError('invalid prefix bound')
    raw_name = lambda frame: f'ram-frame-{frame:06d}.bin'
    inventories = [{int(f.stem.rsplit('-', 1)[1]) for f in d.glob('ram-frame-*.bin')} for d in [a.source, a.native]]
    selected = inventories[0] & inventories[1]
    retained = {}
    page_summary = [dict(address=f'{i*PAGE_BYTES:06X}', first=None, last=None, count=0) for i in range(PAGE_COUNT)]
    frames = []
    cycles = []
    count = 0
    coverage = [0, 0]
    for s, n in zip_longest(read_pages(a.source/'ram-pages.tsv'), read_pages(a.native/'ram-pages.tsv')):
        coverage[0] += s is not None; coverage[1] += n is not None
        if a.through_frame is not None and max(coverage) > a.through_frame: continue
        if s is None or n is None or s[0] != n[0]:
            raise ValueError('source/native frame coverage differs')
        frame = s[0]; count += 1
        if frame in selected: retained[frame] = (s, n)
        if s[1] != n[1]: cycles.append(dict(frame=frame, source=s[1], native=n[1], delta=n[1]-s[1]))
        changed = [i for i, (x, y) in enumerate(zip(s[2], n[2])) if x != y]
        if changed:
            event = dict(frame=frame, pages=len(changed), source_cycle=s[1], native_cycle=n[1])
            frames.append(event)
            for i in changed:
                row = page_summary[i]
                if row['first'] is None: row['first'] = event
                row['last'] = frame; row['count'] += 1
    if not count: raise ValueError('empty RAM capture')
    if a.through_frame is not None and count != a.through_frame: raise ValueError('requested prefix is incomplete')
    if set(retained) != selected: raise ValueError('raw snapshot outside index coverage')
    raw_report = a.output.with_suffix('.raw-differences.jsonl')
    snapshots = []
    with raw_report.open('x', encoding='utf-8') as differences:
        for frame in sorted(selected):
            data = [(d/raw_name(frame)).read_bytes() for d in [a.source, a.native]]
            if any(len(b) != RAM_BYTES for b in data): raise ValueError('invalid raw RAM size')
            for side in range(2):
                hashes = retained[frame][side][2]
                if any(page_hash(data[side][i*PAGE_BYTES:(i+1)*PAGE_BYTES]) != hashes[i] for i in range(PAGE_COUNT)):
                    raise ValueError(f'raw snapshot/hash mismatch at frame {frame}, side {side}')
            offsets = [i for i, (x, y) in enumerate(zip(*data)) if x != y]
            counts = [0] * PAGE_COUNT
            for i in offsets: counts[i // PAGE_BYTES] += 1
            # Coalesce adjacent differing bytes, bounded to 256 bytes per record.
            ranges = []
            for offset in offsets:
                if ranges and offset == ranges[-1][1] and offset-ranges[-1][0] < 256:
                    ranges[-1][1] += 1
                else: ranges.append([offset, offset+1])
            for start, end in ranges:
                differences.write(json.dumps(dict(frame=frame, start=f'{start:06X}', end_exclusive=f'{end:06X}',
                    source=data[0][start:end].hex(), native=data[1][start:end].hex()))+'\n')
            snapshots.append(dict(frame=frame, bytes_differ=len(offsets), first_byte=f'{offsets[0]:06X}' if offsets else None,
                changed_pages=[dict(address=f'{i*PAGE_BYTES:06X}', bytes_differ=c) for i,c in enumerate(counts) if c],
                sha256=[hashlib.sha256(b).hexdigest() for b in data]))
    report = dict(scope=__doc__, compared_frames=count, captured_frames=coverage, explicit_through_frame=a.through_frame,
        first_hash_difference=frames[0] if frames else None,
        first_cycle_difference=cycles[0] if cycles else None, cycle_differences=cycles,
        differing_frames=frames, pages=page_summary, raw_snapshots=snapshots,
        unpaired_snapshots=[sorted(inventories[0]-selected), sorted(inventories[1]-selected)],
        raw_differences=str(raw_report), ignored_pages=[],
        evidence=[dict(path=str(d/'ram-pages.tsv'),sha256=hashlib.sha256((d/'ram-pages.tsv').read_bytes()).hexdigest()) for d in [a.source,a.native]])
    with a.output.open('x', encoding='utf-8') as f: json.dump(report,f,indent=2);f.write('\n')
    print(json.dumps(dict(compared_frames=count,first_hash_difference=report['first_hash_difference'],
        first_cycle_difference=report['first_cycle_difference'],raw_snapshots=snapshots,report=str(a.output)),indent=2))


if __name__ == '__main__': main()
