"""Method contracts use invented bytes; no game assets or historical captures."""
import base64
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import aot_overlay_pipeline as pipeline


class FakeDisc:
    def __init__(self, files):
        self.data = files
        self.files = {name: (10, len(body)) for name, body in files.items()}

    def read(self, name):
        return self.data[name.upper()]


class AotMethodsTest(unittest.TestCase):
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
