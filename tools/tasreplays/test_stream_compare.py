"""Streaming comparison against a simulated runtime; no retail assets."""
from pathlib import Path
import json
import tempfile
import threading
import time
from compare_ram_pages import MAGIC, PAGE_BYTES, PAGE_COUNT, page_hash, read_pages
from observation_evidence import captured_frames, compare_returns
from stream_compare import Watcher, follow_lines, parse_rows, line_hash, write_stop_request, page_reference, stock_reference, stock_ram_reference, CPU_FIELDS

ZERO = page_hash(bytes(PAGE_BYTES))
HEADER = '\t'.join(['frame', 'cycle'] + [f'{i*PAGE_BYTES:06X}' for i in range(PAGE_COUNT)])

assert captured_frames(5, 2, 2, 1) == {0, 2, 3, 4, 5}
assert captured_frames(0, 0) == {0}
for bounds in ((5, 6, 2, 1), (5, -1, 2, 1), (5, 2, 0, 1), (5, 2, 2, 0)):
    try:
        captured_frames(*bounds)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid sparse capture bounds accepted')


def row(frame, cycle_bump=0, page=None):
    pages = [ZERO] * PAGE_COUNT
    if page is not None: pages[page] = '0000000000000001'
    return '\t'.join([str(frame), str(frame*564480+cycle_bump)] + pages)


def capture_text(frames, cycle_bump_at=None, page_at=None, magic=MAGIC, first=1):
    lines = [magic, HEADER] + [row(f, 1 if cycle_bump_at == f else 0, page_at[1] if page_at and page_at[0] == f else None)
                               for f in range(first, frames+1)]
    return '\n'.join(lines) + '\n'


def simulate(run, text, chunk=4096, delay=0.02, create_delay=0.0, truncate_tail=False, write_exit=True):
    """Append text in arbitrary byte chunks like a live runtime, then publish exit.json."""
    data = text.encode()
    if truncate_tail: data = data[:-1]
    def body():
        time.sleep(create_delay)
        with (run/'ram-pages.tsv').open('wb') as stream:
            for offset in range(0, len(data), chunk):
                stream.write(data[offset:offset+chunk]); stream.flush(); time.sleep(delay)
        if write_exit:
            (run/'exit.json').write_text(json.dumps({'exit_code': 0, 'stop_reason': None}))
    thread = threading.Thread(target=body, daemon=True); thread.start()
    return thread


def watch(root, reference_text, native_text, endpoint, kind='pages', stop=False, start=0, **simulate_args):
    run = root/f'run-{time.monotonic_ns()}'; run.mkdir()
    reference = root/f'reference-{time.monotonic_ns()}.tsv'; reference.write_text(reference_text)
    if kind == 'pages': table = page_reference(reference, endpoint, start)
    else: table = {frame: line_hash(values) for frame, _, _, values in parse_rows(reference_text.splitlines())}
    watcher = Watcher(run, table, endpoint, kind=kind, stop_on_divergence=stop, poll=0.02, progress_every=3,
                      first_frame=start+1)
    watcher.start()
    thread = simulate(run, native_text, **simulate_args)
    thread.join(30)
    result = watcher.finish(30)
    return run, reference, result


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    same = capture_text(5)
    # Matching stream, delivered in 7-byte fragments that split every line.
    run, reference, result = watch(root, same, same, 5, chunk=7, delay=0.0, create_delay=0.1)
    assert result['compared_returns'] == 5 and result['first_divergence'] is None and result['complete_through_endpoint']
    assert not result['stopped_on_divergence'] and not (run/'stop-request.json').exists()
    assert result['observation_error'] is None and result['watcher_error'] is None
    assert [r[:2] for r in parse_rows(same.splitlines())] == [(f, f*564480) for f in range(1, 6)]
    # Clock-only divergence: same criterion as compare_returns, empty changed_pages, no stop unless enabled.
    run, reference, result = watch(root, same, capture_text(5, cycle_bump_at=3), 5)
    assert result['first_divergence'] == {'frame': 3, 'source_cycle': 3*564480, 'native_cycle': 3*564480+1, 'changed_pages': []}
    assert result['compared_returns'] == 3 and not result['stopped_on_divergence'] and not (run/'stop-request.json').exists()
    verdict = compare_returns(reference, run/'ram-pages.tsv', 5)['first_divergence']
    assert (verdict['frame'], verdict['changed_pages']) == (3, [])
    # Page divergence with the stop enabled writes the stop request the runner honours.
    run, reference, result = watch(root, same, capture_text(5, page_at=(4, 511)), 5, stop=True)
    assert result['first_divergence'] == {'frame': 4, 'source_cycle': 4*564480, 'native_cycle': 4*564480, 'changed_pages': ['1FF000']}
    assert result['stopped_on_divergence'] and result['stop_on_divergence']
    request = json.loads((run/'stop-request.json').read_text())
    assert request['reason'] == 'divergence' and request['frame'] == 4 and request['first_divergence'] == result['first_divergence']
    verdict = compare_returns(reference, run/'ram-pages.tsv', 5)['first_divergence']
    assert (verdict['frame'], verdict['changed_pages']) == (4, ['1FF000'])
    # Line-hash reference (Tekken): match, then a clock difference reported without cycles.
    run, reference, result = watch(root, same, same, 5, kind='line_hash')
    assert result['compared_returns'] == 5 and result['first_divergence'] is None
    run, reference, result = watch(root, same, capture_text(5, cycle_bump_at=2), 5, kind='line_hash', stop=True)
    assert result['first_divergence'] == {'frame': 2, 'kind': 'line_hash'} and result['stopped_on_divergence']
    assert json.loads((run/'stop-request.json').read_text())['kind'] == 'line_hash'
    # A trailing fragment without its newline is never counted; exit.json ends the tail.
    run, reference, result = watch(root, same, same, 5, chunk=100, truncate_tail=True)
    assert result['compared_returns'] == 4 and result['first_divergence'] is None and not result['complete_through_endpoint']
    # The comparison is bounded to the endpoint; a shorter reference is reported, not matched.
    run, reference, result = watch(root, capture_text(3), same, 3)
    assert result['compared_returns'] == 3 and result['complete_through_endpoint'] and result['native_returns'] == 3
    run, reference, result = watch(root, capture_text(3), same, 5)
    assert result['reference_exhausted'] and result['compared_returns'] == 3 and result['first_divergence'] is None
    # Invalid capture header is an observation error, not a match.
    run, reference, result = watch(root, same, capture_text(5, magic='# other'), 5)
    assert result['observation_error'] and result['compared_returns'] == 0
    # exit.json without any capture: nothing compared, no error, no hang.
    run = root/'no-capture'; run.mkdir(); (run/'exit.json').write_text('{}')
    watcher = Watcher(run, page_reference(reference, 5), 5, poll=0.02); watcher.start()
    result = watcher.finish(10)
    assert result['compared_returns'] == 0 and result['observation_error'] is None and not watcher.is_alive()
    # finish() cancels a watcher whose runner never produced anything.
    run = root/'never'; run.mkdir()
    watcher = Watcher(run, page_reference(reference, 5), 5, poll=0.02); watcher.start()
    started = time.monotonic(); result = watcher.finish(10)
    assert not watcher.is_alive() and time.monotonic()-started < 5 and result['compared_returns'] == 0
    # follow_lines drains complete lines written before cancellation and strips CR and BOM.
    run = root/'drain'; run.mkdir()
    (run/'ram-pages.tsv').write_bytes(b'\xef\xbb\xbfa\r\nb\r\nc')
    cancelled = threading.Event(); cancelled.set()
    assert list(follow_lines(run/'ram-pages.tsv', run, cancelled, poll=0.01)) == ['a', 'b']
    # Stop requests are published atomically and readable as JSON.
    target = write_stop_request(run, {'reason': 'divergence', 'frame': 7})
    assert target.name == 'stop-request.json' and json.loads(target.read_text())['frame'] == 7
    assert not target.with_suffix('.json.tmp').exists()
    # A run resumed from a checkpoint at return 2 captures 3..5: compared against source 3..5 only.
    resumed = capture_text(5, first=3)
    run, reference, result = watch(root, same, resumed, 5, start=2)
    assert result['compared_returns'] == 3 and result['first_divergence'] is None and result['complete_through_endpoint']
    assert result['first_frame'] == 3 and compare_returns(reference, run/'ram-pages.tsv', 5, start=2)['match']
    run, reference, result = watch(root, same, capture_text(5, page_at=(4, 7), first=3), 5, start=2)
    assert result['first_divergence']['frame'] == 4 and result['first_divergence']['changed_pages'] == ['007000']
    verdict = compare_returns(reference, run/'ram-pages.tsv', 5, start=2)
    assert not verdict['match'] and verdict['first_divergence']['frame'] == 4 and verdict['captured_returns'] == [3, 3]
    # A capture that does not begin right after the checkpoint is an observation error, never a match.
    run, reference, result = watch(root, same, same, 5, start=2)
    assert result['observation_error'] and result['compared_returns'] == 0
    try: compare_returns(reference, run/'ram-pages.tsv', 5, start=2); raise AssertionError('misaligned resume admitted')
    except ValueError: pass
    # Streamed rows equal read_pages rows for the same file.
    assert [r[:3] for r in parse_rows(same.splitlines())] == list(read_pages(reference))
    for bad in [Watcher, ]:
        try: bad(run, {}, 0, kind='line_hash'); raise AssertionError('zero endpoint admitted')
        except ValueError: pass
        try: bad(run, {}, 1, kind='other'); raise AssertionError('unknown kind admitted')
        except ValueError: pass
        try: bad(run, {}, 5, first_frame=6); raise AssertionError('resume past the endpoint admitted')
        except ValueError: pass
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    pages = root/'original-pages.tsv'
    cpu = root/'original-cpu.tsv'
    cpu_header = ['frame', *CPU_FIELDS, 'r0']
    pages.write_text(''.join('\t'.join([str(f), *([ZERO]*PAGE_COUNT)])+'\n' for f in range(4)))
    # Retain a nonzero serialized source GPR0; the architectural register remains zero.
    cpu.write_text('\t'.join(cpu_header)+'\n'+''.join('\t'.join([str(f), *(['00000000']*len(CPU_FIELDS)), 'DEADBEEF'])+'\n' for f in range(4)))
    original_pages=pages.read_text()
    for malformed in (original_pages.replace(ZERO,'G'+ZERO[1:],1),
                      original_pages.replace(ZERO,ZERO[:-1],1),
                      original_pages.replace('\n','\t'+ZERO+'\n',1),
                      original_pages[:-1]):
        pages.write_text(malformed)
        try:list(stock_reference(pages,cpu,3))
        except ValueError:pass
        else:raise AssertionError('Malformed full-source page row admitted')
    pages.write_text(original_pages.replace(ZERO,'abcdef0123456789'))
    assert list(stock_reference(pages,cpu,3))[0][2]==['ABCDEF0123456789']*PAGE_COUNT
    pages.write_text(original_pages)
    for name, change_cpu, change_page, truncate in [('same', False, False, False),
            ('cpu', True, False, False), ('page', False, True, False), ('partial', False, False, True)]:
        run = root/name; run.mkdir()
        header = ['frame', 'pc', 'sr', 'cause', 'epc', *[f'r{i}' for i in range(32)]]
        lines = ['\t'.join(header)]
        for frame in range(1, 4):
            values = dict.fromkeys(header, '00000000'); values['frame'] = str(frame)
            if change_cpu and frame == 2: values['pc'] = '80001234'
            lines.append('\t'.join(values[k] for k in header))
        (run/'cpu-return.tsv').write_text('\n'.join(lines)+'\n')
        (run/'cpu-extended.tsv').write_text('frame\thi\tlo\n'+''.join(f'{f}\t00000000\t00000000\n' for f in range(1,4)))
        watcher = Watcher(run, stock_reference(pages, cpu, 3), 3, kind='stock_cpu_pages',
                          stop_on_divergence=True, poll=0.01)
        watcher.start()
        thread = simulate(run, capture_text(3, page_at=(2,7) if change_page else None),
                          chunk=7, delay=0, truncate_tail=truncate)
        thread.join(10); result = watcher.finish(10)
        assert result['watcher_error'] is None and result['observation_error'] is None, result
        if change_cpu or change_page:
            assert result['first_divergence']['frame'] == 2 and result['stopped_on_divergence'], result
            assert json.loads((run/'stop-request.json').read_text())['frame'] == 2
            if change_cpu: assert result['first_divergence']['cpu'][0]['field'] == 'pc'
            else: assert result['first_divergence']['changed_pages'] == ['007000']
        else:
            assert result['first_divergence'] is None and result['complete_through_endpoint'] == (not truncate), result
            assert result['compared_returns'] == (2 if truncate else 3)
    # Original-source comparison of a checkpoint suffix keeps absolute return
    # numbers. It must not pretend the restored process produced rows1..2.
    run=root/'stock-resumed';run.mkdir()
    for name in ('cpu-return.tsv','cpu-extended.tsv'):
        lines=(root/'same'/name).read_text().splitlines()
        (run/name).write_text(lines[0]+'\n'+lines[3]+'\n')
    (run/'ram-pages.tsv').write_text(capture_text(3,first=3))
    watcher=Watcher(run,stock_reference(pages,cpu,3,first_frame=3),3,
                    kind='stock_cpu_pages',first_frame=3,poll=0.01)
    watcher.start();result=watcher.finish(None)
    assert result['compared_returns']==1 and result['complete_through_endpoint'],result
    assert result['first_divergence'] is None and result['first_frame']==3,result
    # A missing required CPU sidecar stops observation and never becomes a match.
    (root/'same/cpu-extended.tsv').unlink()
    watcher = Watcher(root/'same', stock_reference(pages, cpu, 3), 3, kind='stock_cpu_pages', stop_on_divergence=True)
    watcher.start(); result = watcher.finish(10)
    assert result['observation_error'] and result['stop_request']['reason'] == 'observation_error'
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory); pages=root/'pages.tsv'
    pages.write_text('\n'.join('\t'.join([str(f)]+[ZERO]*PAGE_COUNT) for f in range(4))+'\n')
    for changed in (False,True):
        run=root/str(changed);run.mkdir()
        (run/'ram-pages.tsv').write_text(capture_text(3,cycle_bump_at=2,page_at=(3,511) if changed else None))
        (run/'exit.json').write_text('{}')
        watcher=Watcher(run,stock_ram_reference(pages,3),3,kind='stock_ram_pages',poll=0.01)
        watcher.start(); result=watcher.finish(10)
        assert not result['observation_error'] and not result['watcher_error'],result
        if changed:
            assert result['first_divergence']['frame']==3 and result['first_divergence']['changed_pages']==['1FF000']
            assert result['first_divergence']['cpu'] is None and result['first_divergence']['source_cycle'] is None
        else:
            assert result['first_divergence'] is None and result['complete_through_endpoint']
    pages.write_text('0\tbroken\n')
    try:list(stock_ram_reference(pages,3))
    except ValueError:pass
    else:raise AssertionError('Malformed original RAM accepted')
print('Streaming comparison: CPU/RAM controls and explicit original RAM-only controls pass')

# Exact retained segments: no fabricated frame0, gaps, overlap, edited source or CPU fields.
import hashlib,gzip
with tempfile.TemporaryDirectory() as directory:
    base=Path(directory);first=base/'prefix.tsv';second=base/'tail.tsv.gz';manifest=base/'pages-segments.json'
    first.write_text(''.join(str(f)+'\t'+'\t'.join([ZERO]*PAGE_COUNT)+'\n' for f in range(3)),encoding='utf-8')
    second.write_bytes(gzip.compress(capture_text(4,first=2).encode()))
    parts=[dict(path=str(first),sha256=hashlib.sha256(first.read_bytes()).hexdigest(),first=0,last=2,layout='frame-pages'),dict(path=str(second),sha256=hashlib.sha256(second.read_bytes()).hexdigest(),first=3,last=4,layout='frame-cycle-pages')]
    def publish():manifest.write_text(json.dumps(dict(schema='tas-original-ram-segments-v1',segments=parts)),encoding='utf-8')
    publish();result=list(stock_ram_reference(manifest,4));assert [x[0] for x in result]==[1,2,3,4] and all(x[1]is None and x[3]is None for x in result)
    second.write_bytes(gzip.compress(capture_text(4,first=2).split('\n',1)[1].encode()))
    parts[1]['sha256']=hashlib.sha256(second.read_bytes()).hexdigest();publish()
    assert [x[0] for x in stock_ram_reference(manifest,4)]==[1,2,3,4]
    for boundary in (2,4):
        parts[1]['first']=boundary;publish()
        try:list(stock_ram_reference(manifest,4))
        except ValueError:pass
        else:raise AssertionError('Original segment gap/overlap accepted')
    parts[1]['first']=3;parts[0]['sha256']='0'*64;publish()
    try:list(stock_ram_reference(manifest,4))
    except ValueError:pass
    else:raise AssertionError('Changed original segment accepted')
    parts[:]=[dict(path=str(second),sha256=hashlib.sha256(second.read_bytes()).hexdigest(),first=2,last=4,layout='frame-cycle-pages')];publish()
    assert [x[0] for x in stock_ram_reference(manifest,4,first_frame=2)]==[2,3,4]
print('Original segmented RAM reference controls pass;CPU/clock not invented')
