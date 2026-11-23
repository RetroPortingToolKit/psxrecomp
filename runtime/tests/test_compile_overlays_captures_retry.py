#!/usr/bin/env python3
"""load_captures retries a manifest the runtime is replacing. On Windows a read
that meets the runtime's MoveFileEx fails with PermissionError; the live compile
run then compiled nothing and a replay waiting for its unit stopped
(PS1B-674, MDK cold playback at frame 1914)."""
import builtins, importlib.util, json, os, sys, tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('compile_overlays', root / 'tools' / 'compile_overlays.py')
co = importlib.util.module_from_spec(spec)
sys.modules['compile_overlays'] = co
sys.path.insert(0, str(root / 'tools'))
spec.loader.exec_module(co)

fd, path = tempfile.mkstemp(suffix='.json')
with os.fdopen(fd, 'w') as f:
    json.dump([{'phys_addr': 0xF7000}], f)

fails = {'left': 2, 'calls': 0}


def flaky_open(p, *a, **k):
    fails['calls'] += 1
    if p == path and fails['left']:
        fails['left'] -= 1
        raise PermissionError(13, 'Permission denied', p)
    return builtins.open(p, *a, **k)


co.open = flaky_open
try:
    assert co.load_captures(path, delay=0) == [{'phys_addr': 0xF7000}]
    assert fails['calls'] == 3, fails
    fails['left'] = 5
    try:
        co.load_captures(path, attempts=3, delay=0)
    except PermissionError:
        pass
    else:
        raise AssertionError('a manifest that stays locked must still raise')
finally:
    del co.open
    os.remove(path)
src = (root / 'tools' / 'compile_overlays.py').read_text(encoding='utf-8')
assert 'with open(args.captures)' not in src, 'main must read the manifest through load_captures'
print('captures retry: ok')
