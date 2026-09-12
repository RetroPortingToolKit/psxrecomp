#!/usr/bin/env python3
"""Verify declarative original-disc inventories without producing code recipes.

Reports contain metadata only. MIPS instruction-shape counts are observations,
not an executable-code classifier or proof that compressed code cannot exist.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

from aot_overlay_pipeline import Disc, digest, number, require, verify_evidence, write_json


def typed_members(data, alignment=2048):
    """Count/total-size header, then {type, byte-size}, aligned payloads."""
    require(alignment >= 8 and alignment & (alignment - 1) == 0,
            'Alignment must be a power of two')
    require(len(data) >= alignment and len(data) % alignment == 0,
            'Archive is not sector aligned')
    count, total = struct.unpack_from('<II', data)
    require(total == len(data), 'Archive total size mismatch')
    require(0 < count <= (alignment - 8) // 8, 'Invalid descriptor count')
    # The count bounds the descriptor table; remaining header bytes are opaque.
    cursor, members = alignment, []
    for index in range(count):
        kind, size = struct.unpack_from('<II', data, 8 + index * 8)
        require(size > 0 and cursor + size <= len(data), 'Member outside archive')
        members.append(dict(index=index, kind=kind, offset=cursor,
                            body=data[cursor:cursor + size]))
        cursor = (cursor + size + alignment - 1) & ~(alignment - 1)
    require(cursor == len(data), 'Archive payload coverage mismatch')
    return members


def mips_shapes(data):
    words = (struct.unpack_from('<I', data, i)[0] for i in range(0, len(data) - 3, 4))
    returns = frames = 0
    for word in words:
        returns += word == 0x03e00008
        frames += (word & 0xffff8000) == 0x27bd8000
    return dict(aligned_jr_ra=returns, negative_sp_adjust=frames)


def inspect(profile, disc):
    require(profile['schema'] == 'psxrecomp disc inventory v1', 'Unknown inventory schema')
    for algorithm, expected in profile['disc_hashes'].items():
        require(digest(disc.binary, algorithm) == expected, f'Unsupported disc {algorithm}')
    verify_evidence(disc, profile.get('checks', []))
    for table in profile.get('file_tables', []):
        data = disc.read(table['file'])
        origin = number(table.get('file_offset', 0))
        offset = origin + number(table['address']) - number(table.get('base', 0))
        stride = number(table['stride'])
        require(stride >= 4, 'File table stride must be at least four bytes')
        fields = ['lba_offset', 'size_offset']
        if 'first_word_offset' in table:
            fields.append('first_word_offset')
        for field in fields:
            field_offset = number(table[field])
            require(0 <= field_offset <= stride - 4 and field_offset % 4 == 0,
                    f'Invalid file table field offset: {field}')
        for index, name in enumerate(table['files']):
            at = offset + index * stride
            require(0 <= at <= len(data) - stride, 'File table outside source')
            lba = struct.unpack_from('<I', data, at + number(table['lba_offset']))[0]
            size = struct.unpack_from('<I', data, at + number(table['size_offset']))[0]
            require((lba, size) == disc.files[name.upper()], f'ISO/table mismatch: {name}')
            if 'first_word_offset' in table:
                word = struct.unpack_from('<I', data, at + number(table['first_word_offset']))[0]
                require(word == struct.unpack_from('<I', disc.read(name))[0],
                        f'File table/header mismatch: {name}')
    groups = Counter()
    files, seen = [], set()
    for spec in profile['files']:
        name = spec['file'].upper()
        require(name not in seen, f'Duplicate file classification: {name}')
        seen.add(name)
        require(spec.get('role', '').strip(), f'Missing classification rationale: {name}')
        data = disc.read(name)
        info = dict(file=name, role=spec['role'], lba=disc.files[name][0], size=len(data),
                    sha256=hashlib.sha256(data).hexdigest())
        if spec['method'] == 'typed_aligned_archive':
            members = typed_members(data, number(spec.get('alignment', 2048)))
            require(len(members) == spec['member_count'], f'Member count drift: {name}')
            info['members'] = []
            for member in members:
                body = member.pop('body')
                groups[member['kind'] >> number(profile.get('group_shift', 16))] += 1
                info['members'].append(dict(**member, size=len(body),
                    sha256=hashlib.sha256(body).hexdigest(), mips_shapes=mips_shapes(body)))
        elif spec['method'] == 'raw_file':
            if spec.get('inspect_mips_shapes'):
                info['mips_shapes'] = mips_shapes(data)
        else:
            raise ValueError(f"Unknown inventory method: {spec['method']}")
        files.append(info)
    require(seen == set(disc.files), 'Inventory does not classify the complete ISO file list')
    require(dict(groups) == {number(k): v for k, v in profile['expected_groups'].items()},
            'Member group count drift')
    return dict(schema='psxrecomp disc inspection v1', game_id=profile['game_id'],
                disc_hashes=profile['disc_hashes'], files=files, groups=dict(groups),
                limitation='Structural verification and instruction shapes do not prove code absence.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--cue', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    profile = json.loads(args.profile.read_text(encoding='utf-8'))
    report = inspect(profile, Disc(args.cue))
    write_json(args.output, report)
    print(f"Verified {len(report['files'])} files, {sum(report['groups'].values())} typed members")


if __name__ == '__main__':
    main()
