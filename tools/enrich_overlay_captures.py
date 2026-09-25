#!/usr/bin/env python3
"""Add conservative static walk-root evidence to runtime overlay captures.

Runtime capture knows that a dirty-RAM PC was dispatched, but that is not the
same thing as knowing it is a function boundary: CPS continuations and jump
table cases are dispatched too.  This tool therefore leaves the runtime-owned
``function_entry_pcs`` untouched and derives the optional, tool-owned
``static_discovery_entry_pcs`` from captured bytes.  compile_overlays.py
re-validates every supplied root before compiling it.
"""

import argparse
import base64
import json
import struct
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import compile_overlays as CO  # noqa: E402


def word_at(data, base, addr):
    offset = addr - base
    if offset < 0 or offset + 4 > len(data):
        return None
    return struct.unpack_from('<I', data, offset)[0]


def is_candidate(data, base, addr):
    """Use only the compiler's existing root proofs.

    A classic stack-frame prologue is already accepted by overlay discovery.
    Frameless entries additionally need its bounded CFG proof, which rejects
    tables and sequential data that happen to decode as MIPS.
    """
    if addr & 3 or not base <= addr < base + len(data):
        return False
    word = word_at(data, base, addr)
    previous = word_at(data, base, addr - 4)
    if CO._is_addiu_sp_neg(word) and not CO._is_control_flow(previous):
        return True
    return CO.plausible_callable_target(data, base, len(data), addr,
                                        base + len(data))


def derived_roots(data, base):
    """Find generic, independently evidenced roots in one captured image."""
    hi = base + len(data)
    roots = set()
    pointers = []
    for addr in range(base, hi - 3, 4):
        word = word_at(data, base, addr)
        if CO._is_addiu_sp_neg(word) and not CO._is_control_flow(
                word_at(data, base, addr - 4)):
            roots.add(addr)
        if word is not None and (word >> 26) == 0x03:  # jal
            target = 0x80000000 | ((word & 0x03ffffff) << 2)
            if is_candidate(data, base, target):
                roots.add(target)
        pointers.append(word if word is not None and not (word & 3) and
                        base <= word < hi else None)

    # A dense run is a table only when it has three or more in-image pointers.
    # Each member must still pass the same root proof; no bare pointer becomes
    # authority, and a pointer to the table itself fails the CFG proof.
    start = 0
    while start < len(pointers):
        if pointers[start] is None:
            start += 1
            continue
        end = start + 1
        while end < len(pointers) and pointers[end] is not None:
            end += 1
        if end - start >= 3:
            for target in pointers[start:end]:
                if is_candidate(data, base, target):
                    roots.add(target)
        start = end
    return sorted(roots)


def enrich_record(record):
    """Return a copy of *record* with discovered roots unioned into it."""
    out = dict(record)
    try:
        base = CO._parse_addr(record['load_addr'])
        data = base64.b64decode(record['bytes_b64'], validate=True)
        size = int(record.get('size', len(data)))
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError('capture lacks valid load_addr/bytes_b64/size') from exc
    if size != len(data) or base & 3 or len(data) & 3:
        raise ValueError('capture bytes must be a word-aligned image matching size')
    prior = CO._parse_addr_list(record.get('static_discovery_entry_pcs', []))
    roots = sorted(prior | set(derived_roots(data, base)))
    out['static_discovery_entry_pcs'] = [f'0x{addr:08X}' for addr in roots]
    return out, len(roots) - len(prior)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--captures', required=True, help='runtime capture JSON')
    parser.add_argument('--out', required=True, help='enriched JSON output path')
    args = parser.parse_args(argv)
    with open(args.captures, encoding='utf-8') as src:
        records = json.load(src)
    if not isinstance(records, list):
        raise SystemExit('captures must contain a JSON array')
    enriched, added = [], 0
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            raise SystemExit(f'capture {index} is not an object')
        try:
            item, count = enrich_record(record)
        except ValueError as exc:
            raise SystemExit(f'capture {index}: {exc}') from exc
        enriched.append(item)
        added += count
    with open(args.out, 'w', encoding='utf-8', newline='\n') as dst:
        json.dump(enriched, dst, indent=2)
        dst.write('\n')
    print(f'enriched {len(enriched)} capture(s); added {added} static discovery root(s)')


if __name__ == '__main__':
    main()
