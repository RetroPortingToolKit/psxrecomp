"""Follow a running native ram-pages.tsv and compare it against a reference.

The watcher is additive evidence: it can request a stop at the first
divergence and records what it saw, but it never decides a pass. The post-run
comparison in each title module remains the verdict authority.
"""
import hashlib
from itertools import islice
import json
import os
import re
from pathlib import Path
import threading
import time
from compare_ram_pages import MAGIC, PAGE_BYTES, PAGE_COUNT, read_pages

HEADER = ['frame', 'cycle'] + [f'{p*PAGE_BYTES:06X}' for p in range(PAGE_COUNT)]
STOP_REQUEST = 'stop-request.json'
CPU_FIELDS = ['pc', 'sr', 'cause', 'epc', 'hi', 'lo'] + [f'r{i}' for i in range(1, 32)]
STOCK_HASH_ROW = re.compile(r'[0-9a-fA-F]{16}(?:\t[0-9a-fA-F]{16}){' + str(PAGE_COUNT-1) + r'}')


def cpu_rows(lines, required, first_frame=1):
    """Validate complete CPU records, retaining original raw fields (including GPR0)."""
    lines = iter(lines)
    header = next(lines, '').split('\t')
    if len(set(header)) != len(header) or not {'frame', *required} <= set(header):
        raise ValueError('missing or duplicate CPU columns')
    for frame, line in enumerate(lines, first_frame):
        values = line.split('\t')
        if len(values) != len(header):
            raise ValueError(f'incomplete CPU row at {frame}')
        row = dict(zip(header, values))
        if int(row['frame']) != frame:
            raise ValueError(f'discontinuous CPU row at {frame}')
        for key in required:
            if len(row[key]) != 8 or any(c not in '0123456789abcdefABCDEF' for c in row[key]):
                raise ValueError(f'invalid CPU field {key} at {frame}')
        yield row


def stock_reference(pages, cpu, endpoint, first_frame=1):
    """Original stock observations include frame zero and expose no absolute CPU clock."""
    if type(first_frame) is not int or not 1 <= first_frame <= endpoint:
        raise ValueError('original comparison interval is outside its endpoint')
    with Path(pages).open(encoding='utf-8-sig') as page_stream, Path(cpu).open(encoding='utf-8-sig') as cpu_stream:
        registers = cpu_rows(cpu_stream, CPU_FIELDS, first_frame=0)
        for frame in range(endpoint + 1):
            line = page_stream.readline()
            frame_text, separator, hash_text = line.rstrip('\r\n').partition('\t')
            state = next(registers, None)
            if (not line.endswith('\n') or not separator or
                    int(frame_text) != frame or state is None):
                raise ValueError(f'missing original observation at {frame}')
            # Validate every character/count in C rather than billions of
            # Python generator iterations on a full movie. No validation cache.
            if STOCK_HASH_ROW.fullmatch(hash_text) is None:
                raise ValueError(f'invalid original page hash at {frame}')
            hashes = hash_text.upper().split('\t')
            if frame >= first_frame:
                yield frame, None, hashes, state


def write_stop_request(run, payload):
    """Atomically publish <run>/stop-request.json for run_native's wait loop."""
    target = Path(run) / STOP_REQUEST
    temporary = target.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(payload, indent=2) + '\n', encoding='utf-8')
    os.replace(temporary, target)
    return target


def page_reference(path, endpoint, start=0):
    """Source page rows after `start` (a resumed checkpoint) through the compared endpoint."""
    return islice(read_pages(Path(path)), start, endpoint)



def stock_ram_segments(manifest, endpoint, first_frame=1):
    """Read exact retained oracle segments without making up frame zero or CPU fields."""
    import gzip
    spec=json.loads(Path(manifest).read_text(encoding='utf-8-sig'))
    if spec.get('schema')!='tas-original-ram-segments-v1' or not spec.get('segments'):
        raise ValueError('unsupported retained oracle segment contract')
    expected=first_frame
    for part in spec['segments']:
        path=Path(part['path']).resolve(strict=True)
        with path.open('rb') as source:
            if hashlib.file_digest(source,'sha256').hexdigest()!=part['sha256']:
                raise ValueError('original oracle segment hash changed')
        first,last=part['first'],part['last'];layout=part['layout']
        if type(first)is not int or type(last)is not int or not 0<=first<=last<=endpoint or layout not in ('frame-pages','frame-cycle-pages'):
            raise ValueError('invalid original oracle segment bounds/layout')
        opener=gzip.open if path.suffix=='.gz' else open
        selected=0
        with opener(path,'rt',encoding='utf-8-sig') as source:
            if layout=='frame-cycle-pages':
                header=source.readline().rstrip('\r\n')
                if header==MAGIC:header=source.readline().rstrip('\r\n')
                if header.split('\t')!=HEADER:raise ValueError('original raw RAM header differs')
            for line in source:
                values=line.rstrip('\r\n').split('\t')
                frame=int(values[0])
                if not first<=frame<=last:continue
                offset=2 if layout=='frame-cycle-pages' else 1
                hashes=values[offset:]
                if not line.endswith('\n') or len(hashes)!=PAGE_COUNT or STOCK_HASH_ROW.fullmatch('\t'.join(hashes)) is None:
                    raise ValueError('incomplete selected original RAM observation')
                if frame!=first+selected:raise ValueError('nonconsecutive original segment rows')
                if layout=='frame-cycle-pages' and int(values[1])<0:raise ValueError('negative original public clock')
                selected+=1
                if frame<first_frame:continue
                if frame!=expected:raise ValueError('original oracle segment gap or overlap')
                expected+=1
                yield frame,None,[v.upper() for v in hashes],None
        if selected!=last-first+1:raise ValueError('original oracle segment coverage incomplete')
    if expected!=endpoint+1:raise ValueError('original oracle endpoint incomplete')

def stock_ram_reference(pages, endpoint, first_frame=1):
    """Original RAM only: unavailable CPU and clock are never filled with invented values."""
    if not 1 <= first_frame <= endpoint:
        raise ValueError('original RAM interval outside endpoint')
    if Path(pages).suffix=='.json':
        yield from stock_ram_segments(pages,endpoint,first_frame)
        return
    with Path(pages).open(encoding='utf-8-sig') as stream:
        for frame in range(endpoint + 1):
            line = stream.readline()
            number, separator, hashes = line.rstrip('\r\n').partition('\t')
            if not line.endswith('\n') or not separator or int(number) != frame or STOCK_HASH_ROW.fullmatch(hashes) is None:
                raise ValueError(f'incomplete original RAM observation at {frame}')
            if frame >= first_frame:
                yield frame, None, hashes.upper().split('\t'), None


def line_hash_reference(path):
    """Tekken-style {frame: sha256 of ' '.join(values)} from the gz table."""
    import gzip
    table = {}
    for line in gzip.decompress(Path(path).read_bytes()).decode().splitlines():
        frame, sha = line.split()
        table[int(frame)] = sha
    return table


def line_hash(values):
    return hashlib.sha256(' '.join(values).encode()).hexdigest()


def follow_lines(path, run, cancelled=None, poll=0.5):
    """Yield complete text lines from a file another process is appending to.

    Waits for the file to appear unless <run>/exit.json exists or cancelled is
    set; after either, drains every complete line already written and ends. A
    trailing fragment without a newline is never yielded.
    """
    path, run = Path(path), Path(run)
    done = lambda: (run / 'exit.json').exists() or (cancelled is not None and cancelled.is_set())
    while not path.exists():
        if done():
            return
        time.sleep(poll)
    buffer = b''
    first = True
    with path.open('rb') as stream:
        while True:
            chunk = stream.read(1 << 20)
            if chunk:
                buffer += chunk
                *lines, buffer = buffer.split(b'\n')
                for line in lines:
                    if first:
                        first = False
                        if line.startswith(b'\xef\xbb\xbf'):
                            line = line[3:]
                    yield line.decode('utf-8').rstrip('\r')
                continue
            if done():
                # One more read after the writer is known to have finished.
                chunk = stream.read(1 << 20)
                if chunk:
                    buffer += chunk
                    *lines, buffer = buffer.split(b'\n')
                    for line in lines:
                        yield line.decode('utf-8').rstrip('\r')
                return
            time.sleep(poll)


def parse_rows(lines, name='ram-pages.tsv', first_frame=1):
    """Validate the stream exactly like compare_ram_pages.read_pages, row by row.

    Yields (frame, cycle, hashes, values): the same triple read_pages yields
    plus the raw tab-split values for line-hash references. A run resumed from a
    checkpoint at return K starts at first_frame K+1.
    """
    lines = iter(lines)
    magic = next(lines, None)
    if magic is None:
        return
    if magic != MAGIC:
        raise ValueError(f'unsupported RAM index: {name}')
    header = next(lines, None)
    if header is None:
        return
    if header.split('\t') != HEADER:
        raise ValueError(f'invalid page columns: {name}')
    previous = 0
    for expected_frame, line in enumerate(lines, first_frame):
        row = line.split('\t')
        if len(row) != len(HEADER) or int(row[0]) != expected_frame:
            raise ValueError(f'incomplete or discontinuous RAM index: {name}, frame {expected_frame}')
        cycle = int(row[1])
        if cycle <= previous or any(len(h) != 16 or any(c not in '0123456789ABCDEF' for c in h) for h in row[2:]):
            raise ValueError(f'invalid cycle/hash: {name}, frame {expected_frame}')
        previous = cycle
        yield expected_frame, cycle, row[2:], row


class Watcher(threading.Thread):
    """Compare native returns as they appear; optionally request an early stop.

    reference: page rows (kind='pages', from page_reference) or {frame: sha}
    (kind='line_hash'). endpoint bounds the comparison; it is the last return, so
    a run resumed at return K (first_frame K+1) compares endpoint-K returns. The
    result dict is complete after finish().
    """

    def __init__(self, run, reference, endpoint, kind='pages', stop_on_divergence=False,
                 poll=0.5, progress_every=10000, label='native', first_frame=1):
        super().__init__(name='stream-compare', daemon=True)
        if kind not in ('pages', 'line_hash', 'stock_cpu_pages', 'stock_ram_pages'):
            raise ValueError('unsupported streaming reference kind')
        if type(endpoint) is not int or endpoint < 1:
            raise ValueError('streaming endpoint must be a positive return count')
        if type(first_frame) is not int or not 1 <= first_frame <= endpoint:
            raise ValueError('streaming first return must lie inside the endpoint')
        self.first_frame = first_frame
        self.directory = Path(run)
        self.reference = reference
        self.kind = kind
        self.endpoint = endpoint
        self.poll = poll
        self.progress_every = progress_every
        self.label = label
        self.cancelled = threading.Event()
        self.result = {'kind': kind, 'endpoint': endpoint, 'first_frame': first_frame,
                       'compared_returns': 0, 'native_returns': 0,
                       'first_divergence': None, 'stop_on_divergence': bool(stop_on_divergence),
                       'stopped_on_divergence': False, 'stop_request': None,
                       'reference_exhausted': False, 'complete_through_endpoint': False,
                       'observation_error': None, 'watcher_error': None, 'seconds': None,
                       'scope': 'streaming evidence only; the post-run comparison decides the verdict'}

    def _reference_row(self, native):
        frame = native[0]
        if self.kind == 'line_hash':
            expected = self.reference.get(frame)
            return None if expected is None else (frame, expected)
        return next(self.reference, None)

    def _divergence(self, expected, native):
        frame, cycle, hashes, values = native
        if self.kind == 'line_hash':
            if expected[1] != line_hash(values):
                return {'frame': frame, 'kind': 'line_hash'}
            return None
        changed = [f'{i*PAGE_BYTES:06X}' for i in range(PAGE_COUNT) if expected[2][i] != hashes[i]]
        if expected[0] != frame:
            raise ValueError(f'reference frame mismatch at {frame}')
        if self.kind == 'stock_ram_pages':
            return ({'frame': frame, 'kind': self.kind, 'changed_pages': changed,
                     'source_cycle': None, 'native_cycle': cycle, 'cpu': None} if changed else None)
        if self.kind == 'stock_cpu_pages':
            cpu = next(self.native_cpu, None)
            extra = next(self.native_extra, None)
            if cpu is None or extra is None:
                raise ValueError(f'missing committed native CPU observation at {frame}')
            cpu.update(extra)
            fields = [{'field': key, 'source': expected[3][key], 'native': cpu[key]}
                      for key in CPU_FIELDS if int(expected[3][key], 16) != int(cpu[key], 16)]
            # The source's serialized GPR[0] is scratch, not architectural r0.
            if int(cpu['r0'], 16):
                fields.append({'field': 'r0', 'source': '00000000', 'native': cpu['r0']})
            return ({'frame': frame, 'kind': self.kind, 'changed_pages': changed, 'cpu': fields,
                     'source_cycle': None, 'native_cycle': cycle} if changed or fields else None)
        if expected[1] != cycle or changed:
            return {'frame': frame, 'source_cycle': expected[1], 'native_cycle': cycle, 'changed_pages': changed}
        return None

    def _report(self, divergence):
        if divergence.get('kind') == 'stock_ram_pages':
            print(f'ORIGINAL RAM DIVERGENCE at return {divergence["frame"]}: '
                  f'RAM pages={len(divergence["changed_pages"])}; source CPU/clock unavailable', flush=True)
            return
        if divergence.get('kind') == 'stock_cpu_pages':
            print(f'ORIGINAL DIVERGENCE at return {divergence["frame"]}: '
                  f'CPU fields={len(divergence["cpu"])} RAM pages={len(divergence["changed_pages"])}; '
                  'source absolute cycles unavailable', flush=True)
            return
        if divergence.get('kind') == 'line_hash':
            print(f'LINE-HASH DIVERGENCE at return {divergence["frame"]}', flush=True)
            return
        delta = divergence['native_cycle'] - divergence['source_cycle']
        pages = divergence['changed_pages']
        if pages:
            print(f'RAM DIVERGENCE at return {divergence["frame"]}: clock_delta={delta:+d} '
                  f'changed_pages={len(pages)} first={pages[:8]}', flush=True)
        else:
            print(f'CLOCK DIVERGENCE at return {divergence["frame"]}: clock_delta={delta:+d}', flush=True)

    def run(self):
        started = time.monotonic()
        result = self.result
        try:
            if self.kind == 'stock_cpu_pages':
                self.native_cpu = cpu_rows(follow_lines(self.directory / 'cpu-return.tsv', self.directory,
                                                       self.cancelled, self.poll),
                                           ['pc', 'sr', 'cause', 'epc'] + [f'r{i}' for i in range(32)],first_frame=self.first_frame)
                self.native_extra = cpu_rows(follow_lines(self.directory / 'cpu-extended.tsv', self.directory,
                                                         self.cancelled, self.poll), ['hi', 'lo'],first_frame=self.first_frame)
            lines = follow_lines(self.directory / 'ram-pages.tsv', self.directory, self.cancelled, self.poll)
            for native in parse_rows(lines, first_frame=self.first_frame):
                result['native_returns'] = native[0]
                expected = self._reference_row(native)
                if expected is None:
                    result['reference_exhausted'] = True
                    print(f'reference exhausted at {self.label} return {native[0]}', flush=True)
                    break
                result['compared_returns'] += 1
                divergence = self._divergence(expected, native)
                if divergence is not None:
                    result['first_divergence'] = divergence
                    self._report(divergence)
                    if result['stop_on_divergence']:
                        request = {'reason': 'divergence', 'frame': divergence['frame'], 'kind': self.kind,
                                   'requested_by': 'stream_compare', 'first_divergence': divergence,
                                   'compared_returns': result['compared_returns']}
                        write_stop_request(self.directory, request)
                        result['stopped_on_divergence'] = True
                        result['stop_request'] = request
                        print(f'stop requested at return {divergence["frame"]}', flush=True)
                    break
                count = result['compared_returns']
                if count % self.progress_every == 0:
                    rate = count / max(time.monotonic() - started, 1e-9)
                    print(f'match through return {count} ({rate:.1f} ret/s)', flush=True)
                if count >= self.endpoint - self.first_frame + 1:
                    result['complete_through_endpoint'] = True
                    print(f'MATCH complete through {count}', flush=True)
                    break
        except ValueError as error:
            result['observation_error'] = str(error)
            print(f'streaming observation error: {error}', flush=True)
        except Exception as error:  # never let the watcher hang the harness
            result['watcher_error'] = f'{type(error).__name__}: {error}'
        finally:
            for iterator in (self.reference, getattr(self, 'native_cpu', None), getattr(self, 'native_extra', None)):
                if hasattr(iterator, 'close'):
                    iterator.close()
            if result['stop_on_divergence'] and (result['observation_error'] or result['watcher_error']):
                request = {'reason': 'observation_error', 'requested_by': 'stream_compare',
                           'error': result['observation_error'] or result['watcher_error']}
                write_stop_request(self.directory, request)
                result['stop_request'] = request
            result['seconds'] = time.monotonic() - started

    def finish(self, timeout=None):
        """Drain what the runtime wrote, then return the result dict."""
        self.cancelled.set()
        self.join(timeout)
        if self.is_alive():
            self.result['watcher_error'] = 'watcher did not finish'
        return self.result
