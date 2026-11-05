"""Strict, streaming checks for complete independent TAS observations."""
from itertools import islice, zip_longest
from pathlib import Path
import hashlib
import re
from compare_ram_pages import read_pages, page_hash, PAGE_COUNT, PAGE_BYTES, RAM_BYTES


def validate_cpu_capture(directory, expected_frames, first_frame=1):
    header=['frame','pc','cycle','sr','cause','epc']+[f'r{i}' for i in range(32)]
    count=0;previous=0
    with (directory/'cpu-return.tsv').open() as stream:
        if stream.readline().rstrip('\r\n').split('\t')!=header:
            raise ValueError('invalid CPU return columns')
        for count,line in enumerate(stream,1):
            columns=line.rstrip('\r\n').split('\t')
            if len(columns)!=38 or int(columns[0])!=count+first_frame-1:
                raise ValueError('incomplete or discontinuous CPU return capture')
            cycle=int(columns[2])
            if cycle<=previous or any(not re.fullmatch('[0-9A-F]{8}',columns[i]) for i in [1,*range(3,38)]):
                raise ValueError('invalid CPU return state/clock')
            previous=cycle
    if count!=expected_frames-first_frame+1:raise ValueError(f'CPU capture has {count} returns, expected {expected_frames}')
    extended=directory/'cpu-extended.tsv'
    if extended.exists():
        with extended.open() as stream:
            if stream.readline().strip()!='frame\thi\tlo':raise ValueError('invalid CPU HI/LO columns')
            rows=0
            for rows,line in enumerate(stream,1):
                fields=line.rstrip('\r\n').split('\t')
                if (len(fields)!=3 or int(fields[0])!=rows+first_frame-1 or
                    any(not re.fullmatch('[0-9A-F]{8}',v) for v in fields[1:])):raise ValueError('invalid CPU HI/LO row')
            if rows!=count:raise ValueError('CPU HI/LO capture coverage differs')
    return {'valid':True,'frames':count,'extended_hi_lo':extended.exists(),'scope':'native return capture completeness; no independent source CPU equivalence claim'}


def compare_returns(source, native, expected_frames, start=0):
    """Every source return against the native capture; a run resumed at return
    `start` is compared from start+1 and must cover exactly the rest."""
    first = None
    coverage = [0, 0]
    paired = 0
    for expected, actual in zip_longest(islice(read_pages(source), start, None),
                                        read_pages(native, first_frame=start+1)):
        coverage[0] += expected is not None
        coverage[1] += actual is not None
        if expected is None or actual is None:
            if first is None:
                first = {'kind': 'missing_return', 'frame': (expected or actual)[0],
                         'missing_side': 'source' if expected is None else 'native'}
            continue
        paired += 1
        changed = [f'{i*PAGE_BYTES:06X}' for i in range(PAGE_COUNT) if expected[2][i] != actual[2][i]]
        if first is None and (expected[1] != actual[1] or changed):
            first = {'kind': 'state_or_clock', 'frame': expected[0],
                     'source_cycle': expected[1], 'native_cycle': actual[1], 'changed_pages': changed}
    return {'match': first is None and coverage == [expected_frames-start, expected_frames-start],
            'compared_returns': paired, 'captured_returns': coverage, 'first_divergence': first}


def terminal_consistency(pages, raw_path, expected_frames, full_sha=None, first_frame=1):
    terminal = None
    for row in read_pages(pages, first_frame=first_frame):
        terminal = row
    if terminal is None or terminal[0] != expected_frames:
        raise ValueError('RAM index does not reach the declared terminal return')
    raw = raw_path.read_bytes()
    if len(raw) != RAM_BYTES:
        raise ValueError('terminal RAM is not exactly 2 MiB')
    for page in range(PAGE_COUNT):
        if page_hash(raw[page*PAGE_BYTES:(page+1)*PAGE_BYTES]) != terminal[2][page]:
            raise ValueError('terminal RAM disagrees with its page index')
    if full_sha is not None and hashlib.sha256(raw).hexdigest() != full_sha.lower():
        raise ValueError('terminal RAM disagrees with the independent full RAM digest')
    return raw


def captured_frames(endpoint, input_frames, checkpoint_every=1200, tail_every=60):
    if (type(endpoint) is not int or type(input_frames) is not int or
            not 0 <= input_frames <= endpoint or
            any(type(n) is not int or n <= 0 for n in (checkpoint_every, tail_every))):
        raise ValueError('invalid declared capture bounds or intervals')
    frames = {0, endpoint, input_frames}
    frames.update(range(checkpoint_every, endpoint+1, checkpoint_every))
    frames.update(frame for frame in range(input_frames+1, endpoint+1) if frame % tail_every == 0)
    return frames


def compare_stock_observations(stock, observed, endpoint, input_frames, checkpoint_every=1200, tail_every=60):
    """Recompute passivity from captures, including all declared sparse images."""
    a, b = stock/'ram-frames.tsv', observed/'ram-frames.tsv'
    with a.open() as left, b.open() as right:
        count = 0
        for count, (s, n) in enumerate(zip_longest(left, right), 1):
            if s != n:
                raise ValueError(f'stock/observer RAM or lag mismatch at line {count}')
        if count != endpoint + 2:
            raise ValueError('stock/observer return coverage differs')
    frames = captured_frames(endpoint,input_frames,checkpoint_every,tail_every)
    required = {f'frame-{frame:06d}.png' for frame in frames}
    inventories = [{p.name for p in root.glob('frame-*.png')} for root in [stock,observed]]
    if inventories[0] != inventories[1] or not required <= inventories[0]:
        raise ValueError('stock/observer screenshot inventory differs or is incomplete')
    evidence = [a, b]
    for name in sorted(inventories[0]):
        s, n = stock/name, observed/name
        # Identical encoded PNG bytes also establish identical decoded pixels.
        if s.read_bytes() != n.read_bytes():
            raise ValueError(f'stock/observer screenshot differs: {name}')
        evidence += [s, n]
    for name in ['loaded-bios.json', 'effective-sync.json', 'effective-settings.json']:
        s, n = stock/name, observed/name
        if s.read_bytes() != n.read_bytes():
            raise ValueError('stock/observer effective identity differs: '+name)
        evidence += [s, n]
    return evidence
