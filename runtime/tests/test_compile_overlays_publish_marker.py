#!/usr/bin/env python3
"""publish_shard_pair announces each committed shard as `PSX_SHARD_PUBLISHED <dll>` -- but only to a
runtime that asked (PSX_OVERLAY_LIVE_AUTOCOMPILE=1). The runtime's publication pipeline
(autocompile.c) consumes the line; without it every shard compiled during play is first mapped by
the emulation thread (300 ms LoadLibrary at the first dispatch miss, measured)."""
import contextlib, importlib.util, io, os, sys, tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('compile_overlays', root / 'tools' / 'compile_overlays.py')
co = importlib.util.module_from_spec(spec)
sys.modules['compile_overlays'] = co
sys.path.insert(0, str(root / 'tools'))
spec.loader.exec_module(co)


def run(live):
    if live:
        os.environ['PSX_OVERLAY_LIVE_AUTOCOMPILE'] = '1'
    else:
        os.environ.pop('PSX_OVERLAY_LIVE_AUTOCOMPILE', None)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        co.announce_published_shard(str(Path(tempfile.gettempdir()) / '00012000_DEADBEEF.dll'))
    return buf.getvalue()


quiet = run(False)
assert quiet == '', f'offline compile must keep stdout quiet, got {quiet!r}'
loud = run(True)
line = loud.strip()
assert line.startswith('PSX_SHARD_PUBLISHED '), loud
assert line.endswith('00012000_DEADBEEF.dll'), loud
assert os.path.isabs(line[len('PSX_SHARD_PUBLISHED '):]), 'path must be absolute (runtime resolves it)'
assert loud.count('\n') == 1
# The announcement sits on the success path of the single commit point.
src = (root / 'tools' / 'compile_overlays.py').read_text(encoding='utf-8')
body = src[src.index('def publish_shard_pair('):src.index('def _dll_abi_matches(')]
assert 'announce_published_shard(dll)' in body and body.index('announce_published_shard(dll)') > body.index('os.replace(staged_dll, dll)')
print('publish marker: ok')
