#!/usr/bin/env python3
"""Replay-determinism diff for PSX_RB_SNAP_AUDIT logs.

usage: rb_snap_audit_diff.py <dir>   (dir holds snap-audit-p<N>.txt per seat)

1. Same seat, replay vs live: every R save at tick T must equal the seat's
   live L save at T section by section (same snap + same rows => same machine).
2. Across seats: the last save of each tick (after any rollback) must agree.
Prints the first differing section/byte offsets; exit 1 on any difference.
"""
import collections, glob, re, sys

NAMES = {1: 'CPU', 2: 'RAM', 3: 'SPAD', 4: 'IRQ', 5: 'TIMER', 6: 'CLOCK', 7: 'GPU',
         8: 'VRAM', 9: 'SPU', 0xa: 'SPURAM', 0xb: 'CDROM', 0xc: 'DMA', 0xd: 'SIO',
         0xe: 'DIRTY', 0xf: 'MDEC', 0x10: 'ICACHE', 0x11: 'MODMEM', 0x13: 'GCS'}


def parse(path):
    live, replay = {}, collections.defaultdict(list)
    for line in open(path):
        p = line.split()
        if len(p) < 3:
            continue
        secs = {}
        for x in p[2:]:
            tag, rest = x.split(':', 1)
            crc, _, hexd = rest.partition('=')
            secs[int(tag, 16)] = (crc, hexd)
        (live.__setitem__(int(p[1]), secs) if p[0] == 'L'
         else replay[int(p[1])].append(secs))
    return live, replay


def bytes_diff(a, b):
    if not a or not b:
        return []
    x, y = bytes.fromhex(a), bytes.fromhex(b)
    return [(i, x[i], y[i]) for i in range(min(len(x), len(y))) if x[i] != y[i]][:8]


def main(d):
    seats = {int(re.search(r'p(-?\d+)', f)[1]): parse(f)
             for f in glob.glob(f'{d}/snap-audit-p*.txt')}
    bad = 0
    for s, (live, rep) in sorted(seats.items()):
        for t, rows in sorted(rep.items()):
            for r in rows:
                for tag, (crc, h) in r.items():
                    if t in live and tag in live[t] and live[t][tag][0] != crc:
                        bad += 1
                        if bad <= 10:
                            print(f'seat {s} tick {t} replay!=live {NAMES.get(tag, hex(tag))}',
                                  bytes_diff(live[t][tag][1], h))
    if len(seats) > 1:
        common = set.intersection(*(set(v[0]) for v in seats.values()))
        for t in sorted(common):
            ref = min(seats)
            for tag in seats[ref][0][t]:
                vals = {s: v[0][t].get(tag, ('', ''))[0] for s, v in seats.items()}
                if len(set(vals.values())) > 1:
                    bad += 1
                    if bad <= 20:
                        print(f'tick {t} seats differ {NAMES.get(tag, hex(tag))} {vals}')
    print('differences:', bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
