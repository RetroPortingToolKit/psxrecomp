"""Audit native cache pairs against independently established original inputs.

This verifies byte/ABI provenance, not the semantics of generated native code.
Input recipes themselves still require original-disc and loader verification.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import sys


def resident_metadata(dll, record):
    """The compiler's preload opt-in must agree with the verified recipe."""
    marker = dll.with_suffix('.resident')
    resident = record.get('producer') == 'bios_resident_manifest'
    assert marker.exists() == resident, f'Resident preload marker/recipe mismatch: {dll.name}'
    if not resident:
        return {}
    body = marker.read_bytes()
    document = json.loads(body)
    assert document.get('schema') == 'psxrecomp bios resident shard v1', f'Invalid resident marker: {dll.name}'
    bios_hash = record.get('bios_sha256', '')
    assert len(bios_hash) == 64 and all(c in '0123456789abcdef' for c in bios_hash), 'Invalid resident BIOS hash'
    assert document.get('bios_sha256') == bios_hash, f'Resident BIOS provenance mismatch: {dll.name}'
    assert document.get('producer_name') == str(record.get('producer_name', 'BIOS resident code')), \
        f'Resident producer mismatch: {dll.name}'
    return dict(resident_sha256=hashlib.sha256(body).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--framework-root', type=Path,
                        default=Path(__file__).resolve().parents[1])
    parser.add_argument('--recompiler', type=Path, required=True)
    parser.add_argument('--game-toml', type=Path, required=True)
    parser.add_argument('--cache-root', type=Path, required=True)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument('--records', type=Path)
    inputs.add_argument('--inventory', type=Path)
    parser.add_argument('--flavor', type=int, default=0)
    parser.add_argument('--expected-pairs', type=int)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    framework = args.framework_root.resolve()
    sys.path.insert(0, str(framework / 'tools'))
    import compile_overlays as compiler
    try:
        import tomllib
    except ImportError:
        import tomli as tomllib
    config = tomllib.loads(args.game_toml.read_text(encoding='utf-8-sig'))
    game_id = config['game']['id']
    include = framework / 'runtime/include'
    tag = compiler.cache_tag(str(include), str(args.recompiler),
                             str(args.game_toml), args.flavor)
    cache = args.cache_root / game_id / 'gcc' / compiler.cache_arch_abi() / tag
    abi = compiler.overlay_abi_tag(str(include), args.flavor)
    source = args.inventory or args.records
    source_bytes = source.read_bytes()
    document = json.loads(source_bytes)
    jobs = document['jobs'] if args.inventory else [
        dict(name=f'record-{index:03d}', record=record)
        for index, record in enumerate(document)]
    recipes = []
    records_by_name = {}
    for job in jobs:
        if args.inventory:
            record = json.loads(Path(job['input']).read_text(encoding='utf-8-sig'))[0]
        else:
            record = job['record']
        assert not record.get('executed_pcs'), 'Runtime observations are not AOT inputs'
        data = base64.b64decode(record['bytes_b64'], validate=True)
        assert len(data) == record['size']
        if 'sha256' in job:
            assert hashlib.sha256(data).hexdigest() == job['sha256']
        load = int(record['load_addr'], 0)
        known = job.get('known_ranges')
        if known is None:
            known = [(int(r['start'], 0), int(r['end'], 0))
                     for r in record.get('producer_ranges', [])]
        assert known, f"No established producer bounds: {job['name']}"
        bounds = [(lo & 0x1fffffff, hi & 0x1fffffff) for lo, hi in known]
        assert all(lo < hi for lo, hi in bounds)
        recipes.append((job['name'], load, data, bounds))
        records_by_name[job['name']] = record

    pairs = []
    native_ids = []
    dlls = sorted(cache.glob('*' + compiler.overlay_ext()))
    assert dlls, f'No native pairs in expected cache namespace: {cache}'
    for dll in dlls:
        ids = compiler.load_shard_func_ids(str(dll), abi)
        assert ids, f'Invalid ABI/exports/pair/manifest: {dll}'
        physical = int(dll.stem.split('_')[0], 16)
        native_ids.append((physical, ids))
        matched = None
        for name, load, data, bounds in recipes:
            if (load & 0x1fffffff) != physical:
                continue
            known = all(any(lo <= (start & 0x1fffffff) and
                            (start & 0x1fffffff) + length <= hi
                            for lo, hi in bounds)
                        for _, _, ranges in ids for start, length in ranges)
            if known and len(compiler.current_variant_func_ids(ids, data, load, len(data))) == len(ids):
                matched = name
                break
        assert matched, f'No original-input guard proof for {dll.name}'
        pairs.append(dict(dll=dll.name, functions=len(ids), matched_recipe=matched,
                          dll_sha256=hashlib.sha256(dll.read_bytes()).hexdigest(),
                          manifest_sha256=hashlib.sha256(dll.with_suffix('.ranges').read_bytes()).hexdigest(),
                          **resident_metadata(dll, records_by_name[matched])))
    if args.expected_pairs is not None:
        assert len(pairs) == args.expected_pairs, (len(pairs), args.expected_pairs)
    image_coverage = []
    required = {job['name']: {entry & 0x1fffffff for entry in job.get('required_entries', [])}
                for job in jobs}
    for name, load, data, bounds in recipes:
        compatible = set()
        for physical, ids in native_ids:
            if physical != (load & 0x1fffffff):
                continue
            bounded = [item for item in ids if all(
                any(lo <= (start & 0x1fffffff) and (start & 0x1fffffff) + length <= hi
                    for lo, hi in bounds) for start, length in item[2])]
            compatible.update((entry, crc, tuple(tuple(r) for r in ranges))
                              for entry, crc, ranges in compiler.current_variant_func_ids(
                                  bounded, data, load, len(data)))
        assert compatible, f'No guarded native entries for input image: {name}'
        served = {item[0] & 0x1fffffff for item in compatible}
        assert required[name] <= served, f'Missing loader-established entries for {name}: {sorted(required[name] - served)}'
        image_coverage.append(dict(name=name,
                                   required_entries_verified=len(required[name]),
                                   native_entries=len({item[0] for item in compatible}),
                                   guarded_variants=len(compatible)))
    receipt = dict(game_id=game_id, cache_tag=tag, flavor=args.flavor,
                   recipe_input_sha256=hashlib.sha256(source_bytes).hexdigest(),
                   recipe_count=len(recipes), published_pairs=len(pairs),
                   manifest_rows=sum(pair['functions'] for pair in pairs),
                   all_pairs_valid=True, all_guards_match_known_input_bytes=True,
                   full_static_coverage_proven=False,
                   image_coverage=image_coverage, pairs=pairs)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2), encoding='utf-8', newline='\n')
    print(json.dumps({k: v for k, v in receipt.items() if k not in ('pairs', 'image_coverage')}))


if __name__ == '__main__':
    main()
