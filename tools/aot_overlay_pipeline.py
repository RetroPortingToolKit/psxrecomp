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
from packed_sector_table import extract_members as extract_sector_members
from sector_extent_archive import extract_members as extract_extent_members
from aligned_lzss_banks import banks as extract_lzss_banks
from mips_tagged_relocations import parse as parse_tagged_relocations, relocate as relocate_tagged_image

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
        if check['method'] == 'adjacent_files':
            files = [disc.files[name.upper()] for name in check['files']]
            require(all(size % 2048 == 0 and lba + size // 2048 == next_lba
                        for (lba, size), (next_lba, _) in zip(files, files[1:])),
                    'Original disc files are not sector-adjacent')
            continue
        data = disc.read(check['file'])
        origin = number(check.get('file_offset', 0))
        base = number(check.get('base', 0))
        method = check['method']
        if method == 'tagged_relocations':
            image, relocations = parse_tagged_relocations(data)
            require(len(image) == number(check['image_size']), 'Relocated image inventory changed')
            require(len(relocations) == number(check['relocation_count']), 'Relocation inventory changed')
        elif method == 'words':
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


def sector_sources(disc, spec):
    """Verified member inventory, with explicit exclusions and extent checks."""
    first, last = (number(spec['inventory_range'][key]) for key in ('first', 'last'))
    require(0 <= first <= last, 'Invalid sector inventory range')
    configured = {number(item['index']): item for item in spec['members']}
    excluded = {number(item['index']): item for item in spec.get('excluded_members', [])}
    require(len(configured) == len(spec['members']) and
            len(excluded) == len(spec.get('excluded_members', [])), 'Duplicate member inventory')
    require(not configured.keys() & excluded.keys() and
            configured.keys() | excluded.keys() == set(range(first, last + 1)),
            'Sector inventory has missing or conflicting classifications')
    require(all(item.get('reason', '').strip() for item in excluded.values()),
            'Excluded sector member needs a reason')
    members = extract_sector_members(disc.read(spec['table_file']),
                    [(name.upper(), disc.read(name)) for name in spec['payload_files']],
                    range(first, last + 1), sector_size=number(spec.get('sector_size', 2048)),
                    offset_bits=number(spec.get('offset_bits', 20)),
                    table_offset=number(spec.get('table_offset', 0)))
    for name in spec.get('cover_payloads', []):
        spans = sorted((member['source_offset'], len(member['body'])) for member in members
                       if member['source_file'] == name.upper())
        cursor = 0
        for offset, size in spans:
            require(offset == cursor, 'Sector inventory has a gap or overlapping members')
            cursor += size
        require(cursor == len(disc.read(name)), 'Sector inventory does not cover required payload')
    sources = []
    for member in members:
        item = configured.get(member['index'])
        if item is None:
            continue
        base = number(item['load_addr'])
        require(0x80000000 <= base < base + len(member['body']) <= 0x80200000,
                'Sector image outside RAM')
        name = f"{spec['table_file'].upper()}:ENTRY_{member['index']:04X}"
        sources.append(dict(name=name, base=base, body=member['body'],
                            spec={**spec, **item}, source_offset=member['source_offset'],
                            source_file=member['source_file'], aliases=[]))
    return sources


def positioned_sources(disc, specifications):
    sources = []
    for spec in specifications:
        method = spec['method']
        if method == 'aligned_lzss_banks':
            grouped = {}
            containers = spec['containers']
            require(len({item['file'].upper() for item in containers}) == len(containers),
                    'Duplicate compressed container')
            for item in containers:
                file = item['file'].upper()
                inventory, banks = extract_lzss_banks(disc.read(file),
                    alignment=number(spec.get('alignment', 2048)),
                    version=number(spec.get('version', 1)),
                    bank_tag=number(spec.get('bank_tag', 0x4B)),
                    terminal_tag=number(spec.get('terminal_tag', 0x31)),
                    data_tags=tuple(number(x) for x in spec.get('data_tags', [0x30])))
                require(len(inventory) == item['member_count'] and
                        [bank['bank'] for bank in banks] == item['bank_indices'],
                        f'Compressed bank inventory changed: {file}')
                for bank in banks:
                    placement = item['placements'][str(bank['bank'])]
                    base = number(placement['load_addr'])
                    body = bank['body']
                    require(hashlib.sha256(body).hexdigest() == placement['decoded_sha256'],
                            f'Decoded bank changed: {file}')
                    require(0x80000000 <= base < base + len(body) <= 0x80200000,
                            'Decoded bank outside RAM')
                    name = f"{file}:BANK_{bank['bank']:04X}"
                    key = (base, body)
                    if key in grouped:
                        grouped[key]['aliases'].append(name)
                        require(grouped[key]['spec']['entries'] == placement.get('entries', []),
                                'Duplicate bank has conflicting declared entries')
                        continue
                    source = dict(name=name, base=base, body=body,
                        spec={**spec, **placement, 'entries': placement.get('entries', []),
                              'allow_missing': True},
                        source_file=file, source_offset=bank['source_offset'], aliases=[])
                    grouped[key] = source
                    sources.append(source)
            continue
        if method == 'packed_sector_members':
            sources.extend(sector_sources(disc, spec))
            continue
        if method == 'sector_extent_members':
            members = extract_extent_members(disc.read(spec['file']),
                sector_size=number(spec.get('sector_size', 2048)),
                table_offset=number(spec.get('table_offset', 0)), count=number(spec['count']))
            configured = {number(item['index']): item for item in spec['members']}
            excluded = {number(item['index']): item for item in spec.get('excluded_members', [])}
            require(len(configured) == len(spec['members']) and
                    len(excluded) == len(spec.get('excluded_members', [])), 'Duplicate member inventory')
            require(not configured.keys() & excluded.keys() and
                    configured.keys() | excluded.keys() == set(range(len(members))),
                    'Archive inventory has missing or conflicting classifications')
            require(all(item.get('reason', '').strip() for item in excluded.values()),
                    'Excluded archive member needs a reason')
            for member in members:
                item = configured.get(member['index'])
                if item is None:
                    continue
                base = number(item['load_addr'])
                require(0x80000000 <= base < base + len(member['body']) <= 0x80200000,
                        'Archive image outside RAM')
                name = f"{spec['file'].upper()}:ENTRY_{member['index']:04X}"
                sources.append(dict(name=name, base=base, body=member['body'],
                    spec={**spec, **item}, source_offset=member['source_offset'],
                    source_file=spec['file'].upper(), aliases=[]))
            continue
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
            elif method == 'tagged_relocated_files':
                base = number(spec['load_addr'])
                require(not spec.get('verify_duplicate_names'),
                        'Relocated sources need individual source identities')
                body, _ = relocate_tagged_image(body, base)
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


def declared_entries(source, disc):
    body, base, spec = source['body'], source['base'], source['spec']
    entries = set()
    if 'entry_word' in spec:
        item = spec['entry_word']
        offset = number(item.get('file_offset', 0)) + number(item['address']) - number(item.get('base', 0))
        entries.add(struct.unpack_from('<I', disc.read(item['file']), offset)[0])
    entries.update(number(x) for x in spec.get('entries', []))
    require(all(base <= entry < base + len(body) and entry % 4 == 0 for entry in entries),
            f"Entry outside image: {source['name']}")
    return entries


def make_fixed_record(source, disc):
    body, base, spec = source['body'], source['base'], source['spec']
    require(spec.get('allow_missing'), f"Generic extractor missed required image: {source['name']}")
    direct = set(extractor.direct_jal_roots(body, base))
    seeds = set(extractor.prologues(body, base)) | extractor.frameless_leaf_entries(body, base)
    if spec.get('supplemental_entries', True):
        seeds |= extractor.supplemental_callable_seeds(body, base)
    entries = declared_entries(source, disc)
    seeds = {entry for entry in seeds if extractor.optional_entry_delay_valid(body, base, entry)}
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


def eligible_ranges(source, lo, hi):
    """Explicit, byte-verified fallback intervals cannot be native producers."""
    excluded = []
    for item in source['spec'].get('excluded_ranges', []):
        start, end = number(item['start']), number(item['end'])
        base, body = source['base'], source['body']
        require(base <= start < end <= base + len(body) and start % 4 == end % 4 == 0,
                'Invalid excluded native interval')
        require(item.get('reason', '').strip(), 'Excluded native interval needs a reason')
        require(hashlib.sha256(body[start-base:end-base]).hexdigest() == item['sha256'],
                'Excluded native interval bytes changed')
        excluded.append((start, end))
    excluded.sort()
    require(all(a[1] <= b[0] for a, b in zip(excluded, excluded[1:])),
            'Overlapping excluded native intervals')
    ranges = [(lo, hi)]
    for start, end in excluded:
        ranges = [(a, b) for left, right in ranges
                  for a, b in [(left, min(right, start)), (max(left, end), right)] if a < b]
    return ranges


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
        if source['spec']['method'] in ('fixed_address_files', 'packed_sector_members', 'sector_extent_members', 'aligned_lzss_banks', 'tagged_relocated_files') and source['name'] not in singles:
            record = make_fixed_record(source, disc)
            singles[source['name']] = record
            records.append(record)
    # Loader-established exports remain required even when heuristic discovery
    # found the image without finding that particular callable entry.
    for source in sources:
        entries = declared_entries(source, disc)
        if entries:
            require(source['name'] in singles, 'Declared entries need a complete source recipe')
            record = singles[source['name']]
            for key in ('function_entry_pcs', 'dispatch_entry_pcs', 'static_dispatch_entry_pcs', 'seeds'):
                record[key] = [hex(pc) for pc in sorted(entries | {number(pc) for pc in record[key]})]
            record['static_discovery_entry_pcs'] = [hex(pc) for pc in sorted(entries |
                {number(pc) for pc in record.get('static_discovery_entry_pcs', [])})]
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
        bounds = [span for source, lo, hi in matches for span in eligible_ranges(source, lo, hi)]
        if any(source['spec'].get('excluded_ranges') for source, _, _ in matches):
            require(profile.get('strict_bounds'), 'Excluded intervals require strict producer bounds')
            eligible = lambda pc: any(lo <= number(pc) < hi for lo, hi in bounds)
            for key in ('function_entry_pcs', 'dispatch_entry_pcs', 'static_dispatch_entry_pcs',
                        'static_discovery_entry_pcs', 'seeds'):
                if key in record:
                    record[key] = [pc for pc in record[key] if eligible(pc)]
            record['static_alias_ranges'] = [alias for alias in record.get('static_alias_ranges', [])
                if any(lo <= number(alias['start']) <= number(alias['entry']) < number(alias['end']) <= hi
                       for lo, hi in bounds)]
            require(all(eligible(entry) for source, _, _ in matches
                        for entry in declared_entries(source, disc)), 'Excluded required loader entry')
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
                         required_entries=sorted({entry for source, _, _ in matches
                                                  for entry in declared_entries(source, disc)}),
                         known_ranges=bounds, input=str(path.resolve()), load_addr=hex(load),
                         size=len(data), sha256=hashlib.sha256(data).hexdigest(),
                         recipe_sha256=hashlib.sha256(json.dumps(record, sort_keys=True).encode()).hexdigest(),
                         pair_stem=f'{load & 0x1fffffff:08X}_{zlib.crc32(data):08X}'))
    require(set(by_name) <= covered, 'Configured image missing from inventory')
    inventory = dict(schema='psxrecomp original-disc AOT inventory v1', game_id=profile['game_id'],
                     profile_sha256=hashlib.sha256(json.dumps(profile, sort_keys=True).encode()).hexdigest(),
                     original_disc_sha256=digest(disc.binary), required_images=sorted(by_name),
                     recipe_count=len(jobs), full_static_coverage_proven=False, jobs=jobs)
    inventory['source_images'] = [dict(name=s['name'], aliases=s['aliases'],
        method=s['spec']['method'], load_addr=hex(s['base']), size=len(s['body']),
        sha256=hashlib.sha256(s['body']).hexdigest(),
        excluded_ranges=s['spec'].get('excluded_ranges', [])) for s in sources]
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
                else:
                    shutil.copy2(library, destination)
                    shutil.copy2(library.with_suffix('.ranges'), destination.with_suffix('.ranges'))
                marker = library.with_suffix('.resident')
                if marker.exists():
                    target_marker = destination.with_suffix('.resident')
                    require(not target_marker.exists() or marker.read_bytes() == target_marker.read_bytes(),
                            f'Conflicting resident metadata: {library.name}')
                    shutil.copy2(marker, target_marker)
    return cache


def stage(cache, destination, receipt):
    """Publish precisely the audited platform namespace; never copy old caches."""
    source = cache / receipt['game_id'] / 'gcc' / compiler.cache_arch_abi() / receipt['cache_tag']
    target = destination / 'cache' / receipt['game_id'] / 'gcc' / compiler.cache_arch_abi() / receipt['cache_tag']
    target.mkdir(parents=True, exist_ok=True)
    expected = set()
    for pair in receipt['pairs']:
        artifacts = [(pair['dll'], 'dll_sha256'),
                     (Path(pair['dll']).with_suffix('.ranges').name, 'manifest_sha256')]
        if 'resident_sha256' in pair:
            artifacts.append((Path(pair['dll']).with_suffix('.resident').name, 'resident_sha256'))
        for name, key in artifacts:
            expected.add(name)
            require(digest(source / name) == pair[key], f'Audited artifact changed: {name}')
            shutil.copy2(source / name, target / name)
            require(digest(target / name) == pair[key], f'Staged artifact changed: {name}')
    extras = {p.name for p in target.iterdir() if p.is_file()} - expected
    require(not extras, f'Stage contains unaudited files: {sorted(extras)}')
    write_json(destination / 'AOT_CACHE_AUDIT.json', receipt)


def require_runtime_cache(config):
    require(config.get('runtime', {}).get('overlay_cache') is True,
            'AOT release requires runtime.overlay_cache = true in the packaged config')


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
    if args.action == 'release':
        import tomllib
        require_runtime_cache(tomllib.loads((args.runtime_config or args.game_toml)
                                           .read_text(encoding='utf-8-sig')))
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
