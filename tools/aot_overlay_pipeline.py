#!/usr/bin/env python3
"""Original-disc overlay methods, verified inventories, and release staging.

Game profiles contain facts, never executable plugins. This module contains no
game-ID branches. New consumers select the same methods with their own evidence.
"""
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

import compile_overlays as compiler
from aot_overlay_spike import extract_generic as extractor

FRAMEWORK = Path(__file__).resolve().parents[1]


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8', newline='\n')


def digest(path, algorithm='sha256'):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


class Disc:
    def __init__(self, cue):
        self.binary, raw = extractor.parse_cue_datatrack(str(cue))
        self.reader = extractor.eo.DiscReader(self.binary, raw=raw)
        self.files = {name.upper(): (lba, size)
                      for name, lba, size in extractor.eo.enumerate_files(self.reader)}
        self.cache = {}

    def read(self, name):
        name = name.upper()
        require(name in self.files, f'Missing original disc file: {name}')
        if name not in self.cache:
            self.cache[name] = self.reader.read_file_bytes(*self.files[name])
        return self.cache[name]


def verify_evidence(disc, checks):
    """Reusable binary-word, pointer-string, and BCD extent table identifiers."""
    for check in checks:
        data = disc.read(check['file'])
        origin = number(check.get('file_offset', 0))
        base = number(check.get('base', 0))
        method = check['method']
        if method == 'words':
            for address, expected in check['values'].items():
                offset = origin + number(address) - base
                require(0 <= offset <= len(data) - 4, 'Evidence word outside source')
                actual = struct.unpack_from('<I', data, offset)[0]
                require(actual == number(expected), f"Loader evidence changed: {check['file']} {address}")
        elif method == 'pointer_strings':
            for index, expected in enumerate(check['strings']):
                offset = origin + number(check['table']) - base + index * 4
                pointer = struct.unpack_from('<I', data, offset)[0]
                start = origin + pointer - base
                require(0 <= start < len(data), 'String pointer outside source')
                end = data.find(b'\0', start)
                require(end >= start and data[start:end].decode('ascii') == expected,
                        f"Filename table changed: {check['file']} index {index}")
        elif method == 'bcd_extent_table':
            offset = origin + number(check['table']) - base + number(check['index']) * 8
            location = data[offset:offset + 8]
            require(len(location) == 8 and all((v >> 4) < 10 and (v & 15) < 10 for v in location[:3]),
                    'Invalid BCD disc location')
            bcd = lambda v: (v >> 4) * 10 + (v & 15)
            lba = (bcd(location[0]) * 60 + bcd(location[1])) * 75 + bcd(location[2]) - 150
            size = struct.unpack_from('<I', location, 4)[0]
            require((lba, size) == disc.files[check['target'].upper()], 'Loader/ISO extent mismatch')
        else:
            raise ValueError(f'Unknown evidence method: {method}')


def positioned_sources(disc, specifications):
    sources = []
    for spec in specifications:
        method = spec['method']
        for name in spec['files']:
            body = disc.read(name)
            offset = 0
            if method == 'psx_exe':
                require(body[:8] == b'PS-X EXE', f'{name}: missing PS-X EXE header')
                base = struct.unpack_from('<I', body, 0x18)[0]
                offset = 0x800
                body = body[offset:]
            elif method == 'fixed_address_files':
                base = number(spec['load_addr'])
            else:
                raise ValueError(f'Unknown image method: {method}')
            require(0x80000000 <= base < base + len(body) <= 0x80200000, f'{name}: image outside RAM')
            aliases = []
            if spec.get('verify_duplicate_names'):
                leaf = name.rsplit('/', 1)[-1].upper()
                for other in disc.files:
                    if other.rsplit('/', 1)[-1] == leaf:
                        require(disc.read(other) == body, f'Conflicting original duplicate: {other}')
                        aliases.append(other)
            sources.append(dict(name=name.upper(), base=base, body=body, spec=spec,
                                source_offset=offset, aliases=aliases))
    require(len({s['name'] for s in sources}) == len(sources), 'Duplicate configured source')
    return sources


def match_sources(record, sources):
    require(not record.get('executed_pcs'), 'Runtime observations cannot be AOT inputs')
    data = base64.b64decode(record['bytes_b64'], validate=True)
    require(len(data) == record['size'], 'Recipe size mismatch')
    load = number(record['load_addr'])
    matches = []
    for source in sources:
        base, body = source['base'], source['body']
        lo, hi = max(load, base), min(load + len(data), base + len(body))
        # Raw producers must appear in full. PS-X EXEs can be floor-split.
        if source['spec']['method'] != 'psx_exe' and (lo != base or hi != base + len(body)):
            continue
        if lo < hi and data[lo-load:hi-load] == body[lo-base:hi-base]:
            matches.append((source, lo, hi))
    return matches


def make_fixed_record(source, disc):
    body, base, spec = source['body'], source['base'], source['spec']
    require(spec.get('allow_missing'), f"Generic extractor missed required image: {source['name']}")
    direct = set(extractor.direct_jal_roots(body, base))
    seeds = set(extractor.prologues(body, base)) | extractor.frameless_leaf_entries(body, base)
    if spec.get('supplemental_entries', True):
        seeds |= extractor.supplemental_callable_seeds(body, base)
    entries = set()
    if 'entry_word' in spec:
        item = spec['entry_word']
        offset = number(item.get('file_offset', 0)) + number(item['address']) - number(item.get('base', 0))
        entries.add(struct.unpack_from('<I', disc.read(item['file']), offset)[0])
    entries.update(number(x) for x in spec.get('entries', []))
    require(all(base <= entry < base + len(body) and entry % 4 == 0 for entry in entries),
            f"Entry outside image: {source['name']}")
    seeds |= entries
    require(seeds, f"No static entries: {source['name']}")
    page, data = extractor.page_aligned_region(base, body)
    return extractor.rec(page, data, sorted(seeds), dispatch_extra=sorted(entries),
                         producer_ranges=[(base, base + len(body))], static_discovery=direct | entries)


def compose_records(left, right, left_record, right_record, max_gap):
    """Compose known simultaneous producers; the intervening gap is unowned."""
    lend = left['base'] + len(left['body'])
    gap = right['base'] - lend
    require(0 <= gap <= max_gap, 'Overlapping or excessive-gap composition')
    page = left['base'] & ~0xFFF
    data = bytearray(right['base'] + len(right['body']) - page)
    data[left['base']-page:lend-page] = left['body']
    data[right['base']-page:] = right['body']
    bounds = [(left['base'], lend), (right['base'], right['base'] + len(right['body']))]
    seeds = sorted({number(pc) for r in (left_record, right_record) for pc in r['function_entry_pcs']})
    dispatch = sorted({number(pc) for r in (left_record, right_record) for pc in r['dispatch_entry_pcs']})
    return extractor.rec(page, bytes(data), seeds, dispatch_extra=dispatch, producer_ranges=bounds)


def prepare(profile, disc, records, output):
    verify_evidence(disc, profile.get('checks', []))
    sources = positioned_sources(disc, profile['images'])
    bios = extractor.bios_resident_records() if profile.get('bios_resident') else []
    for record in records:
        if record.get('producer') == 'bios_resident_manifest':
            require(any(record == known for known in bios), 'Unverified BIOS resident recipe')
        else:
            require(match_sources(record, sources), 'Generic recipe has no configured original source')
    singles = {}
    for record in records:
        matches = match_sources(record, sources)
        if len(matches) == 1:
            source, lo, hi = matches[0]
            if (lo, hi) == (source['base'], source['base'] + len(source['body'])):
                singles[source['name']] = record
    for source in sources:
        if source['spec']['method'] == 'fixed_address_files' and source['name'] not in singles:
            record = make_fixed_record(source, disc)
            singles[source['name']] = record
            records.append(record)
    by_name = {source['name']: source for source in sources}
    keys = {(number(r['load_addr']), r['bytes_b64']) for r in records}
    for composition in profile.get('compositions', []):
        left_name = composition['left'].upper()
        for right_name in composition['right']:
            right_name = right_name.upper()
            record = compose_records(by_name[left_name], by_name[right_name], singles[left_name],
                                     singles[right_name], number(composition.get('max_gap', 0)))
            key = (number(record['load_addr']), record['bytes_b64'])
            if key not in keys:
                records.append(record)
                keys.add(key)
    require(len(records) == profile['expected_records'],
            f"Expected {profile['expected_records']} recipes, got {len(records)}; review inventory drift")
    jobs, covered = [], set()
    for index, record in enumerate(records):
        matches = match_sources(record, sources)
        names = [source['name'] for source, _, _ in matches]
        bounds = [(lo, hi) for _, lo, hi in matches]
        if record.get('producer') == 'bios_resident_manifest':
            names = ['BIOS resident manifest']
            bounds = [(number(r['start']), number(r['end'])) for r in record['producer_ranges']]
        require(bounds, 'No established producer bounds')
        covered.update(names)
        record['guard_bytes'] = 0
        record['producer_ranges'] = [dict(start=hex(lo), end=hex(hi)) for lo, hi in bounds]
        if profile.get('strict_bounds'):
            record['strict_producer_ranges'] = True
        data = base64.b64decode(record['bytes_b64'], validate=True)
        load = number(record['load_addr'])
        path = output / 'runtime-inputs' / f'{index:03d}.json'
        write_json(path, [record])
        jobs.append(dict(name=f'{index:03d}-' + '+'.join(names), sources=names,
                         known_ranges=bounds, input=str(path.resolve()), load_addr=hex(load),
                         size=len(data), sha256=hashlib.sha256(data).hexdigest(),
                         recipe_sha256=hashlib.sha256(json.dumps(record, sort_keys=True).encode()).hexdigest(),
                         pair_stem=f'{load & 0x1fffffff:08X}_{zlib.crc32(data):08X}'))
    require(set(by_name) <= covered, 'Configured image missing from inventory')
    inventory = dict(schema='psxrecomp original-disc AOT inventory v1', game_id=profile['game_id'],
                     profile_sha256=hashlib.sha256(json.dumps(profile, sort_keys=True).encode()).hexdigest(),
                     original_disc_sha256=digest(disc.binary), required_images=sorted(by_name),
                     recipe_count=len(jobs), full_static_coverage_proven=False, jobs=jobs)
    write_json(output / 'runtime-input-inventory.json', inventory)
    return inventory


def extract(profile_path, game_toml, recompiler, output, cue=None):
    profile = json.loads(profile_path.read_text(encoding='utf-8-sig'))
    require(profile['schema'] == 'psxrecomp AOT methods v1', 'Unsupported AOT profile')
    import tomllib
    config = tomllib.loads(game_toml.read_text(encoding='utf-8-sig'))
    require(config['game']['id'] == profile['game_id'], 'AOT profile/game mismatch')
    cue = cue or game_toml.parent / config['game']['disc']
    disc = Disc(cue)
    for algorithm, expected in profile['disc_hashes'].items():
        require(digest(disc.binary, algorithm) == expected, f'Unsupported disc {algorithm}')
    output.mkdir(parents=True, exist_ok=True)
    # Override only the input disc in a temporary config beside the original,
    # retaining its relative paths and code-generation settings.
    command = [sys.executable, str(FRAMEWORK / 'tools/aot_overlay_spike/extract_generic.py'),
               '--game-toml', str(game_toml), '--recompiler', str(recompiler),
               '--out', str(output / 'generic.json'), '--tmp', str(output / 'extract-tmp')]
    require(cue.resolve() == (game_toml.parent / config['game']['disc']).resolve(),
            'Use a local game config with the desired disc path')
    command += ['--require-bios-resident'] if profile.get('bios_resident') else ['--no-bios-resident']
    with (output / 'extract.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    return prepare(profile, disc, json.loads((output / 'generic.json').read_text(encoding='utf-8-sig')), output)


def audit(game_toml, recompiler, cache, inventory, output):
    subprocess.run([sys.executable, str(FRAMEWORK / 'tools/audit_aot_cache.py'),
                    '--framework-root', str(FRAMEWORK), '--recompiler', str(recompiler),
                    '--game-toml', str(game_toml), '--cache-root', str(cache),
                    '--inventory', str(inventory), '--output', str(output)], check=True)
    return json.loads(output.read_text())


def build(inventory, game_toml, recompiler, work, gcc, workers, project_root=None):
    """Independent recipe builds cannot nominate entries in sibling images."""
    cache = work / 'cache'
    cache.mkdir(parents=True)
    def compile_job(pair):
        index, job = pair
        target = work / 'jobs' / f'{index:03d}'
        target.mkdir(parents=True)
        command = [sys.executable, str(FRAMEWORK / 'tools/compile_overlays.py'),
                   '--captures', job['input'], '--game-toml', str(game_toml),
                   '--project-root', str(project_root or game_toml.parent), '--recompiler', str(recompiler),
                   '--runtime-include', str(FRAMEWORK / 'runtime/include'),
                   '--out-dir', str(target / 'cache'), '--compiler', 'gcc', '--gcc', gcc,
                   '--flavor', '0', '--jobs', '1']
        with (target / 'compile.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        return index, job, target
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(compile_job, item) for item in enumerate(inventory['jobs'])]
        for future in as_completed(futures):
            try:
                index, job, target = future.result()
            except Exception:
                # Stop queued work immediately. Already-running compilers may
                # finish, but no partial result reaches auditing or staging.
                for pending in futures:
                    pending.cancel()
                print(f'AOT compilation failed; recipe logs are under {work / "jobs"}',
                      file=sys.stderr, flush=True)
                raise
            print(f"Compiled {index + 1}/{len(futures)}: {job['name']}", flush=True)
            for library in (target / 'cache').rglob('*' + compiler.overlay_ext()):
                destination = cache / library.relative_to(target / 'cache')
                destination.parent.mkdir(parents=True, exist_ok=True)
                if destination.exists():
                    require(library.with_suffix('.ranges').read_bytes() == destination.with_suffix('.ranges').read_bytes(),
                            f'Conflicting native pair identity: {library.name}')
                    continue
                shutil.copy2(library, destination)
                shutil.copy2(library.with_suffix('.ranges'), destination.with_suffix('.ranges'))
    return cache


def stage(cache, destination, receipt):
    """Publish precisely the audited platform namespace; never copy old caches."""
    source = cache / receipt['game_id'] / 'gcc' / compiler.cache_arch_abi() / receipt['cache_tag']
    target = destination / 'cache' / receipt['game_id'] / 'gcc' / compiler.cache_arch_abi() / receipt['cache_tag']
    target.mkdir(parents=True, exist_ok=True)
    expected = set()
    for pair in receipt['pairs']:
        for name, key in [(pair['dll'], 'dll_sha256'),
                          (Path(pair['dll']).with_suffix('.ranges').name, 'manifest_sha256')]:
            expected.add(name)
            require(digest(source / name) == pair[key], f'Audited artifact changed: {name}')
            shutil.copy2(source / name, target / name)
            require(digest(target / name) == pair[key], f'Staged artifact changed: {name}')
    extras = {p.name for p in target.iterdir() if p.is_file()} - expected
    require(not extras, f'Stage contains unaudited files: {sorted(extras)}')
    write_json(destination / 'AOT_CACHE_AUDIT.json', receipt)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['extract', 'release'])
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--game-toml', type=Path, required=True)
    parser.add_argument('--runtime-config', type=Path,
                        help='Packaged config controlling native cache namespace/code generation')
    parser.add_argument('--runtime-build-dir', type=Path,
                        help='Verify the staged runtime publishes the supported flavor-0 ABI')
    parser.add_argument('--runtime-target', default='psx-runtime')
    parser.add_argument('--recompiler', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--stage', type=Path)
    parser.add_argument('--gcc', default='gcc')
    parser.add_argument('--workers', type=int, default=3)
    args = parser.parse_args()
    require(args.workers > 0, 'Workers must be positive')
    if args.runtime_build_dir:
        from release_stage import _flavor_from_build
        require(_flavor_from_build(str(args.runtime_build_dir), args.runtime_target) == 0,
                'This AOT pipeline currently requires a flavor-0 runtime')
    args.work_dir.mkdir(parents=True, exist_ok=True)
    # Every release extracts again from the supported original disc. A private
    # new work directory excludes runtime caches and incomplete prior attempts.
    work = Path(tempfile.mkdtemp(prefix='disc-aot-', dir=args.work_dir.resolve()))
    config, recompiler = args.game_toml.resolve(), args.recompiler.resolve()
    inventory = extract(args.profile.resolve(), config, recompiler, work)
    print(f"Verified {len(inventory['required_images'])} images / {len(inventory['jobs'])} recipes", flush=True)
    if args.action == 'release':
        require(args.stage is not None, 'release requires --stage')
        runtime_config = args.runtime_config.resolve() if args.runtime_config else config
        import tomllib
        require(tomllib.loads(runtime_config.read_text(encoding='utf-8-sig'))['game']['id'] == inventory['game_id'],
                'Runtime config/game mismatch')
        cache = build(inventory, runtime_config, recompiler, work, args.gcc, args.workers, config.parent)
        receipt = audit(runtime_config, recompiler, cache, work / 'runtime-input-inventory.json', work / 'audit.json')
        receipt['profile_sha256'] = inventory['profile_sha256']
        receipt['original_disc_sha256'] = inventory['original_disc_sha256']
        receipt['required_images'] = inventory['required_images']
        stage(cache, args.stage.resolve(), receipt)
        print(f"Staged {receipt['published_pairs']} audited native pairs", flush=True)
    print(f'Original-disc evidence: {work}', flush=True)


if __name__ == '__main__':
    main()
