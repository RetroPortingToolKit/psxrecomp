import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_title_release as release
import aot_overlay_pipeline as pipeline


class RegionalCacheTest(unittest.TestCase):
    def test_exact_game_guards_and_codegen_tag_are_required(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = dict(game_id='SCES-01331', all_pairs_valid=True,
                           all_guards_match_known_input_bytes=True, cache_tag='tag', flavor=2)
            ctx = SimpleNamespace(aot_attempt=root, game_id='SCES-01331',
                config=SimpleNamespace(player_game_toml=root/'game.toml', runtime_target='psx-runtime'),
                recompiler=root/'emitter', aot_cache=None, stage=root/'stage', runtime_build=root)
            (root/'psxrecomp_overlay_flavor-psx-runtime.txt').write_text('2\n')
            def check():
                (root/'audit.json').write_text(json.dumps(receipt))
                release.stage_audited_cache(ctx)
            with mock.patch.object(pipeline.compiler, 'cache_tag', return_value='tag') as cache_tag, \
                 mock.patch.object(pipeline, 'stage') as stage:
                check()
                self.assertEqual(cache_tag.call_args.args[-1], 2)
                stage.assert_called_once_with(root/'cache', root/'stage', receipt)
                for field,value in [('game_id','SCES-01330'),('all_pairs_valid',False),
                                    ('all_guards_match_known_input_bytes',False),('cache_tag','other'),('flavor',0)]:
                    previous=receipt[field];receipt[field]=value
                    with self.assertRaises(release.PackagingError):check()
                    receipt[field]=previous
                self.assertEqual(stage.call_count,1)

    def test_audited_cache_requires_the_explicit_existing_build_command(self):
        args=release.parse_args(['package-existing','--channel','public','--build-dir','build',
                                '--recompiler','emitter','--aot-attempt','audit'])
        self.assertEqual(args.command,'package-existing')


if __name__=='__main__':unittest.main()
