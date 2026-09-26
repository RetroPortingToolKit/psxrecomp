#!/usr/bin/env python3
"""Inspect (and optionally materialize) static root enrichment for captures.

compile_overlays.py derives these roots itself (enrichment is on by default
and part of the overlay cache key), so this tool is no longer a pipeline
step. It shows what the compiler's derivation finds in each captured image,
using exactly the same code (compile_overlays.derive_static_roots), and can
still write the roots into ``static_discovery_entry_pcs`` for offline A/B.

Every derived root needs an image-local boundary proof (a non-delay-slot
stack-frame prologue, or a preceding ``jr $ra`` plus the bounded CFG probe).
A jal or pointer elsewhere in the image is not proof on its own: overlay
regions are swapped, so shared code can call an address that is a function
start only in a sibling image. compile_overlays.py additionally applies the
no-split guard to every root, derived or not.
"""

import argparse
import base64
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import compile_overlays as CO  # noqa: E402


def derived_roots(data, base, sources=None):
    """The compiler's own enrichment roots for one captured image."""
    return sorted(CO.derive_static_roots(data, base, len(data),
                                         sources=sources))


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
    parser.add_argument('--out', help='write an enriched JSON copy here')
    parser.add_argument('--list', action='store_true',
                        help='print every derived root and its evidence sources')
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
        if args.list:
            base = CO._parse_addr(record['load_addr'])
            data = base64.b64decode(record['bytes_b64'])
            sources = {}
            roots = derived_roots(data, base, sources)
            print(f'capture {index} load=0x{base:08X}: {len(roots)} derived root(s)')
            for addr in roots:
                print(f'  0x{addr:08X}  {"+".join(sorted(sources[addr]))}')
        enriched.append(item)
        added += count
    if args.out:
        with open(args.out, 'w', encoding='utf-8', newline='\n') as dst:
            json.dump(enriched, dst, indent=2)
            dst.write('\n')
    print(f'{len(enriched)} capture(s); {added} static discovery root(s) not '
          f'already declared')


if __name__ == '__main__':
    main()
