"""Method contracts use invented bytes; no game assets or historical captures."""
import base64
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import aot_overlay_pipeline as pipeline
import audit_aot_cache as auditor


class FakeDisc:
    def __init__(self, files):
        self.data = files
        self.files = {name: (10, len(body)) for name, body in files.items()}

    def read(self, name):
        return self.data[name.upper()]


class AotMethodsTest(unittest.TestCase):
    def test_tagged_relocated_original_file_and_inventory_check(self):
        original = struct.pack('<8I', 16, 8, 0x03e00008, 0, 4, 0xFFFFFFFF, 0, 0)[:24]
        disc = FakeDisc({'MODULE.DLL': original})
        spec = dict(method='tagged_relocated_files', files=['MODULE.DLL'],
                    load_addr='0x80100000', allow_missing=True, entries=['0x80100008'])
        source, = pipeline.positioned_sources(disc, [spec])
        self.assertEqual(source['body'], struct.pack('<4I', 16, 0x80100008, 0x03e00008, 0))
        self.assertEqual(disc.read('MODULE.DLL'), original)
        check = dict(method='tagged_relocations', file='MODULE.DLL', image_size=16, relocation_count=1)
        pipeline.verify_evidence(disc, [check])
        with self.assertRaisesRegex(ValueError, 'inventory changed'):
            pipeline.verify_evidence(disc, [{**check, 'relocation_count': 2}])
        with tempfile.TemporaryDirectory() as directory:
            disc.binary = Path(directory) / 'source.bin'
            disc.binary.write_bytes(original)
            inventory = pipeline.prepare(dict(game_id='TEST', images=[spec], checks=[check],
                expected_records=1, strict_bounds=True), disc, [], Path(directory))
        self.assertEqual(inventory['required_images'], ['MODULE.DLL'])
        self.assertEqual(inventory['jobs'][0]['required_entries'], [0x80100008])

    def test_resident_preload_metadata_survives_build_audit_and_stage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record = dict(producer='bios_resident_manifest', bios_sha256='a' * 64,
                          producer_name='Synthetic exact-BIOS helper')
            filename = '00001000_12345678' + pipeline.compiler.overlay_ext()
            def compile_fixture(command, **kwargs):
                cache = Path(command[command.index('--out-dir') + 1])
                leaf = cache / 'TEST/gcc' / pipeline.compiler.cache_arch_abi() / 'cg'
                leaf.mkdir(parents=True)
                dll = leaf / filename
                dll.write_bytes(b'Synthetic library')
                dll.with_suffix('.ranges').write_bytes(b'Synthetic ranges')
                pipeline.compiler.update_bios_resident_marker(str(dll), record)
            with mock.patch.object(pipeline.subprocess, 'run', side_effect=compile_fixture):
                cache = pipeline.build(dict(jobs=[dict(input='input.json', name='resident')]),
                    root / 'game.toml', root / 'emitter', root, 'gcc', 1)
            library = next(cache.rglob('*' + pipeline.compiler.overlay_ext()))
            pair = dict(dll=filename, dll_sha256=pipeline.digest(library),
                        manifest_sha256=pipeline.digest(library.with_suffix('.ranges')),
                        **auditor.resident_metadata(library, record))
            receipt = dict(game_id='TEST', cache_tag='cg', pairs=[pair])
            pipeline.stage(cache, root / 'stage', receipt)
            staged = next((root / 'stage').rglob('*.resident'))
            self.assertEqual(staged.read_bytes(), library.with_suffix('.resident').read_bytes())
            library.with_suffix('.resident').write_bytes(b'changed after audit')
            with self.assertRaisesRegex(ValueError, 'Audited artifact changed'):
                pipeline.stage(cache, root / 'stage', receipt)

    def test_resident_audit_rejects_missing_unproven_and_changed_markers(self):
        with tempfile.TemporaryDirectory() as directory:
            dll = Path(directory) / ('helper' + pipeline.compiler.overlay_ext())
            record = dict(producer='bios_resident_manifest', bios_sha256='a' * 64)
            with self.assertRaisesRegex(AssertionError, 'marker/recipe mismatch'):
                auditor.resident_metadata(dll, record)
            pipeline.compiler.update_bios_resident_marker(str(dll), record)
            self.assertIn('resident_sha256', auditor.resident_metadata(dll, record))
            with self.assertRaisesRegex(AssertionError, 'marker/recipe mismatch'):
                auditor.resident_metadata(dll, {})
            with self.assertRaisesRegex(AssertionError, 'BIOS provenance'):
                auditor.resident_metadata(dll, {**record, 'bios_sha256': 'b' * 64})
            marker = dll.with_suffix('.resident')
            document = json.loads(marker.read_bytes())
            marker.write_text(json.dumps({**document, 'schema': 'unknown'}), encoding='utf-8')
            with self.assertRaisesRegex(AssertionError, 'Invalid resident marker'):
                auditor.resident_metadata(dll, record)

    def test_release_refuses_config_that_would_ignore_bundled_native_modules(self):
        for config in ({}, {'runtime': {}}, {'runtime': {'overlay_cache': False}}):
            with self.subTest(config=config), self.assertRaisesRegex(ValueError, 'overlay_cache = true'):
                pipeline.require_runtime_cache(config)
        pipeline.require_runtime_cache({'runtime': {'overlay_cache': True}})

    def test_verified_fallback_intervals_split_native_ownership(self):
        body = bytes(range(32))
        item = dict(start=0x1008, end=0x1010, reason='Unsupported original instruction',
                    sha256=hashlib.sha256(body[8:16]).hexdigest())
        source = dict(base=0x1000, body=body, spec=dict(excluded_ranges=[item]))
        self.assertEqual(pipeline.eligible_ranges(source, 0x1000, 0x1020),
                         [(0x1000, 0x1008), (0x1010, 0x1020)])
        self.assertEqual(pipeline.eligible_ranges(source, 0x1014, 0x1020),
                         [(0x1014, 0x1020)])
        self.assertEqual(pipeline.eligible_ranges(source, 0x1008, 0x100c), [])
        for change, error in [({'start': 0xffc}, 'Invalid'),
                              ({'end': 0x100f}, 'Invalid'),
                              ({'reason': ''}, 'reason'),
                              ({'sha256': '0' * 64}, 'bytes changed')]:
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, error):
                pipeline.eligible_ranges({**source, 'spec': dict(excluded_ranges=[{**item, **change}])},
                                         0x1000, 0x1020)
        with self.assertRaisesRegex(ValueError, 'Overlapping'):
            pipeline.eligible_ranges({**source, 'spec': dict(excluded_ranges=[item, item])},
                                     0x1000, 0x1020)

    def test_fallback_cannot_hide_required_loader_entry(self):
        base, body = 0x80100000, struct.pack('<4I', 0x03e00008, 0, 0x03e00008, 0)
        spec = dict(method='fixed_address_files', files=['CODE'], load_addr=hex(base),
                    entries=[hex(base)], excluded_ranges=[dict(start=base, end=base+8,
                    reason='Synthetic fallback', sha256=hashlib.sha256(body[:8]).hexdigest())])
        record = pipeline.extractor.rec(base, body, [base, base+8])
        with tempfile.TemporaryDirectory() as directory:
            for strict, error in [(False, 'strict producer'), (True, 'required loader entry')]:
                with self.subTest(strict=strict), self.assertRaisesRegex(ValueError, error):
                    pipeline.prepare(dict(images=[spec], expected_records=1, strict_bounds=strict),
                                     FakeDisc({'CODE': body}), [dict(record)], Path(directory))

    def test_optional_normal_roots_reject_control_transfer_in_delay_slot(self):
        base = 0x80100000
        body = struct.pack('<8I', 0, 0, 0x0C004000, 0x0C004001,
                           0x0C004000, 0, 0x03E00008, 0)
        roots = [base + 8, base + 16, base + 24]
        self.assertEqual(pipeline.extractor.filter_full_discovery_seeds(
            body, base, roots, base + 16), [base + 16, base + 24])
        # A loader-established entry is never silently discarded.
        self.assertIn(base + 8, pipeline.extractor.filter_full_discovery_seeds(
            body, base, roots, base + 8))
        source = dict(name='SYNTHETIC', base=base, body=body, spec=dict(allow_missing=True))
        with mock.patch.object(pipeline.extractor, 'prologues', return_value=roots), \
             mock.patch.object(pipeline.extractor, 'frameless_leaf_entries', return_value=set()), \
             mock.patch.object(pipeline.extractor, 'supplemental_callable_seeds', return_value=set()):
            record = pipeline.make_fixed_record(source, FakeDisc({}))
        self.assertNotIn('0x80100008', record['function_entry_pcs'])

    @staticmethod
    def extent_archive():
        data = bytearray(7 * 2048)
        for i, (sector, size) in enumerate([(2, 2052), (4, 2048), (5, 13), (6, 2048)]):
            struct.pack_into('<II', data, i * 8, sector, size)
            data[sector * 2048:sector * 2048 + size] = bytes([65 + i]) * size
        return data

    def test_explicit_sector_offsets_win_over_one_sector_header_heuristic(self):
        data = self.extent_archive()
        members = pipeline.extractor.split_indexed_archive(data)
        self.assertEqual([offset for _, offset, _ in members], [4096, 8192, 10240, 12288])
        self.assertEqual(members[0][2], b'A' * 2052)
        self.assertEqual(members[-1][2], b'D' * 2048)
        self.assertEqual(members[2][2], b'C' * 13)

    def test_extent_archive_rejects_gaps_overlaps_truncation_and_count_drift(self):
        data = self.extent_archive()
        for offset, value in [(8, 2), (8, 5), (28, 4096), (32, 7)]:
            changed = bytearray(data)
            struct.pack_into('<I', changed, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                pipeline.extract_extent_members(changed, count=4)
        for changed in [data[:-1], data + bytes(2048)]:
            with self.assertRaises(ValueError):
                pipeline.extract_extent_members(changed, count=4)
        with self.assertRaisesRegex(ValueError, 'count'):
            pipeline.extract_extent_members(data, count=3)

    def test_extent_inventory_accounts_for_every_member(self):
        disc = FakeDisc({'ARCHIVE': self.extent_archive()})
        spec = dict(method='sector_extent_members', file='ARCHIVE', count=4,
                    members=[dict(index=i, load_addr='0x80100000') for i in range(3)],
                    excluded_members=[dict(index=3, reason='Data table')])
        sources = pipeline.positioned_sources(disc, [spec])
        self.assertEqual(sources[0]['source_offset'], 4096)
        self.assertEqual(sources[0]['body'], b'A' * 2052)
        with self.assertRaisesRegex(ValueError, 'classifications'):
            pipeline.positioned_sources(disc, [{**spec, 'excluded_members': []}])

    def test_sector_inventory_covers_payload_and_accounts_for_exclusions(self):
        disc = FakeDisc({'INDEX': struct.pack('<III', 0x100000, 0x100001, 0x100002),
                         'DATA': b'A' * 2048, 'CODE': b'B' * 2048 + b'C' * 2048})
        spec = dict(method='packed_sector_members', table_file='INDEX',
                    payload_files=['DATA', 'CODE'], inventory_range=dict(first=1, last=2),
                    cover_payloads=['CODE'], members=[dict(index=1, load_addr='0x80100000')],
                    excluded_members=[dict(index=2, reason='Load destination unresolved')])
        sources = pipeline.positioned_sources(disc, [spec])
        self.assertEqual([(s['name'], s['body'], s['source_offset']) for s in sources],
                         [('INDEX:ENTRY_0001', b'B' * 2048, 0)])
        with self.assertRaisesRegex(ValueError, 'classifications'):
            pipeline.positioned_sources(disc, [{**spec, 'excluded_members': []}])
        disc.data['INDEX'] = struct.pack('<III', 0x100000, 0x100001, 0x100001)
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            pipeline.positioned_sources(disc, [spec])

    def test_sector_bounds_and_descriptor_layout_fail_closed(self):
        for descriptor, indices, options, error in [
                (0x200000, [0], {}, 'crosses'), (0x100002, [0], {}, 'bounds'),
                (0, [0], {}, 'Empty'), (0x100000, [1], {}, 'outside table'),
                (0x100000, [0, 0], {}, 'Duplicate'),
                (0x100000, [0], dict(offset_bits=32), 'layout'),
                (0x100000, [0], dict(sector_size=3), 'power of two')]:
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                pipeline.extract_sector_members(struct.pack('<I', descriptor),
                    [('A', bytes(2048)), ('B', bytes(2048))], indices, **options)

    def test_adjacent_disc_extents_are_verified(self):
        disc = FakeDisc({'INDEX': bytes(2048), 'BODY': bytes(4096)})
        disc.files = {'INDEX': (10, 2048), 'BODY': (11, 4096)}
        check = dict(method='adjacent_files', files=['INDEX', 'BODY'])
        pipeline.verify_evidence(disc, [check])
        disc.files['BODY'] = (12, 4096)
        with self.assertRaisesRegex(ValueError, 'sector-adjacent'):
            pipeline.verify_evidence(disc, [check])

    def test_empty_primary_needs_every_current_byte_root_and_preserves_other_errors(self):
        compiler = pipeline.compiler
        pending = [('image', 0x100000, 0x80100000, 16, b'bytes', {0x80100000, 0x80100008})]
        for coverage, failed in [({0x100000}, 1), ({0x100000, 0x100008}, 0)]:
            stats = compiler.ShardStats()
            with mock.patch.object(compiler, 'load_region_current_variant_coverage', return_value=(coverage, [])):
                compiler.reconcile_empty_primary_scans(pending, 'cache', 123, stats)
            self.assertEqual(stats.total_fail(), failed)
        stats = compiler.ShardStats()
        stats.add_fail('earlier', 'compile_error')
        with mock.patch.object(compiler, 'load_region_current_variant_coverage', return_value=({0x100000, 0x100008}, [])):
            compiler.reconcile_empty_primary_scans(pending, 'cache', 123, stats)
        self.assertEqual(stats.total_fail(), 1)

    def test_invalid_branch_candidate_is_rejected_without_hiding_real_failures(self):
        compiler = pipeline.compiler
        entry = 0x80100000
        job = dict(static_demands={entry}, executed=set(), forced=set())
        reason = 'delay-slot-identity: func 0x80100000 at 0x80100010: reserved/unsupported branch encoding'
        self.assertTrue(compiler.fragment_batch_failure_is_partitionable(reason))
        self.assertTrue(compiler.optional_static_fragment_rejection(entry, job, reason))
        for evidence in ('executed', 'forced'):
            self.assertFalse(compiler.optional_static_fragment_rejection(entry, {**job, evidence: {entry}}, reason))
        for failure in ('compile-error (toolchain unavailable)',
                        'delay-slot-identity: func 0x80100000: missing guarded delay word'):
            self.assertFalse(compiler.optional_static_fragment_rejection(entry, job, failure))
            self.assertFalse(compiler.fragment_batch_failure_is_partitionable(failure))
        successes, failures = [], []
        def compile_roots(roots):
            return (None, reason) if entry in roots else (['valid'], 'built')
        compiler.compile_batched_fragment_roots(
            {entry, entry + 32}, compile_roots,
            lambda roots, result, status: successes.extend(roots),
            lambda root, status: failures.append(root),
            should_bisect=compiler.fragment_batch_failure_is_partitionable)
        self.assertEqual(successes, [entry + 32])
        self.assertEqual(failures, [entry])

    def test_packaged_config_uses_source_project_and_propagates_compile_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            failure = pipeline.subprocess.CalledProcessError(2, 'compiler')
            with mock.patch.object(pipeline.subprocess, 'run', side_effect=failure) as run:
                with self.assertRaises(pipeline.subprocess.CalledProcessError):
                    pipeline.build(dict(jobs=[dict(input='input.json', name='image')]),
                                   root / 'stage/game.toml', root / 'emitter', root,
                                   'gcc', 1, root / 'source')
            command = run.call_args.args[0]
            self.assertEqual(command[command.index('--project-root') + 1], str(root / 'source'))
            self.assertEqual(command[command.index('--game-toml') + 1], str(root / 'stage/game.toml'))
            self.assertFalse((root / 'stage/AOT_CACHE_AUDIT.json').exists())

    def test_unexpected_inventory_count_blocks_preparation(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'Expected 1 recipes, got 0'):
                pipeline.prepare(dict(images=[], expected_records=1), FakeDisc({}), [], Path(directory))

    def test_word_evidence_rejects_changed_loader(self):
        disc = FakeDisc({'LOADER': struct.pack('<II', 0x3C048010, 0x24841000)})
        check = dict(method='words', file='LOADER', base='0x80010000',
                     values={'0x80010000': '0x3C048010', '0x80010004': '0x24841000'})
        pipeline.verify_evidence(disc, [check])
        disc.data['LOADER'] = struct.pack('<II', 0x3C048010, 0x24842000)
        with self.assertRaisesRegex(ValueError, 'Loader evidence changed'):
            pipeline.verify_evidence(disc, [check])

    def test_filename_table_requires_exact_targets(self):
        disc = FakeDisc({'NAMES': struct.pack('<I', 0x80100004) + b'ALPHA\0'})
        check = dict(method='pointer_strings', file='NAMES', base='0x80100000',
                     table='0x80100000', strings=['ALPHA'])
        pipeline.verify_evidence(disc, [check])
        check['strings'] = ['BETA']
        with self.assertRaisesRegex(ValueError, 'Filename table changed'):
            pipeline.verify_evidence(disc, [check])

    def test_declared_duplicate_must_match(self):
        disc = FakeDisc({'ONE/OVERLAY': b'12345678', 'TWO/OVERLAY': b'87654321'})
        spec = dict(method='fixed_address_files', files=['ONE/OVERLAY'],
                    load_addr='0x80100000', verify_duplicate_names=True)
        with self.assertRaisesRegex(ValueError, 'Conflicting original duplicate'):
            pipeline.positioned_sources(disc, [spec])

    def test_gap_never_becomes_producer_evidence(self):
        left = dict(base=0x80100100, body=b'A' * 16)
        right = dict(base=0x80100180, body=b'B' * 16)
        a = pipeline.extractor.rec(left['base'], left['body'], [left['base']])
        b = pipeline.extractor.rec(right['base'], right['body'], [right['base']])
        record = pipeline.compose_records(left, right, a, b, 112)
        self.assertEqual(record['producer_ranges'], [
            dict(start='0x80100100', end='0x80100110'),
            dict(start='0x80100180', end='0x80100190')])
        raw = base64.b64decode(record['bytes_b64'])
        self.assertEqual(raw[0x110:0x180], bytes(112))
        with self.assertRaisesRegex(ValueError, 'excessive-gap'):
            pipeline.compose_records(left, right, a, b, 111)

    def test_runtime_observations_rejected(self):
        record = pipeline.extractor.rec(0x80100000, b'1234', [])
        record['executed_pcs'] = ['0x80100000']
        with self.assertRaisesRegex(ValueError, 'Runtime observations'):
            pipeline.match_sources(record, [])

    def test_partial_raw_extent_is_not_a_match(self):
        source = dict(base=0x80100000, body=b'12345678',
                      spec=dict(method='fixed_address_files'))
        record = pipeline.extractor.rec(0x80100000, b'1234', [])
        self.assertEqual(pipeline.match_sources(record, [source]), [])

    def test_stage_refuses_missing_or_changed_audited_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = dict(game_id='TEST', cache_tag='cg', pairs=[dict(
                dll='unit' + pipeline.compiler.overlay_ext(), dll_sha256='bad', manifest_sha256='bad')])
            source = root / 'cache/TEST/gcc' / pipeline.compiler.cache_arch_abi() / 'cg'
            source.mkdir(parents=True)
            (source / receipt['pairs'][0]['dll']).write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'Audited artifact changed'):
                pipeline.stage(root / 'cache', root / 'stage', receipt)


if __name__ == '__main__':
    unittest.main()
