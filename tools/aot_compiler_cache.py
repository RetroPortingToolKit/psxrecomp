"""Content-addressed compiler artifacts for the original-disc AOT release."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tempfile
import zipfile


def file_digest(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def identity(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def publish_bytes(path, data):
    """A killed writer leaves at most a disposable temp file, never a hit."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        staging = Path(stream.name)
        try:
            stream.write(data)
        except BaseException:
            stream.close()
            staging.unlink(missing_ok=True)
            raise
    try:
        os.replace(staging, path)
    finally:
        staging.unlink(missing_ok=True)


def toolchain_identity(gcc, env, flags, runtime_include):
    """Fingerprint the driver, its subprograms and actual runtime header expansion."""
    def query(*args, source=None):
        return subprocess.run([gcc, *args], input=source, capture_output=True,
                              env=env, check=True).stdout

    driver = Path(shutil.which(gcc, path=env.get('PATH')) or gcc).resolve()
    programs = {}
    for name in ('cc1', 'as', 'ld', 'collect2'):
        reported = os.fsdecode(query(f'-print-prog-name={name}').strip())
        found = shutil.which(reported, path=env.get('PATH')) or reported
        path = Path(found)
        if path.is_file():
            programs[name] = file_digest(path)
    # The system headers under these includes can change independently of GCC.
    headers = query(*flags, '-I' + str(runtime_include), '-E', '-P', '-x', 'c', '-',
                    source=b'#include "psx_runtime.h"\n#include "overlay_api.h"\n'
                           b'#include "psx_native_span.h"\n#include <math.h>\n')
    return identity(dict(
        driver=file_digest(driver), programs=programs, flags=flags,
        version=os.fsdecode(query('--version')), target=os.fsdecode(query('-dumpmachine')),
        headers=hashlib.sha256(headers).hexdigest(),
        compiler_env={key: env[key] for key in (
            'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'GCC_EXEC_PREFIX',
            'COMPILER_PATH', 'LIBRARY_PATH', 'SDKROOT', 'DEVELOPER_DIR',
            'MACOSX_DEPLOYMENT_TARGET', 'SOURCE_DATE_EPOCH')
            if key in env}))


def recipe_files(cache):
    return {p.relative_to(cache).as_posix(): p for p in cache.rglob('*')
            if p.is_file() and p.suffix in {'.dll', '.so', '.dylib', '.ranges', '.resident'}}


def restore_recipe(archive, cache, key):
    """Verify all bytes before exposing any artifacts to the release assembler."""
    try:
        with zipfile.ZipFile(archive) as bundle:
            receipt = json.loads(bundle.read('receipt.json'))
            entries = receipt['files']
            if (receipt['key'] != key or not entries or
                    len(bundle.namelist()) != len(entries) + 1 or
                    set(bundle.namelist()) != {'receipt.json', *entries}):
                return False
            contents = {}
            for name, expected in entries.items():
                path = PurePosixPath(name)
                if (path.is_absolute() or '..' in path.parts or str(path) != name
                        or '\\' in name or ':' in name or
                        path.suffix not in {'.dll', '.so', '.dylib', '.ranges', '.resident'}):
                    return False
                data = bundle.read(name)
                if hashlib.sha256(data).hexdigest() != expected:
                    return False
                contents[name] = data
    except (OSError, ValueError, TypeError, KeyError, zipfile.BadZipFile):
        return False
    for name, data in contents.items():
        dest = cache / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
    return True


def save_recipe(archive, cache, key):
    files = recipe_files(cache)
    if not files:
        return
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=archive.parent, delete=False) as stream:
        staging = Path(stream.name)
    try:
        with zipfile.ZipFile(staging, 'w') as bundle:
            entries = {}
            for name, path in sorted(files.items()):
                data = path.read_bytes()
                bundle.writestr(name, data)
                entries[name] = hashlib.sha256(data).hexdigest()
            bundle.writestr('receipt.json', json.dumps(dict(key=key, files=entries)))
        os.replace(staging, archive)
    finally:
        staging.unlink(missing_ok=True)


_EXPORT = '#ifdef _WIN32\n__declspec(dllexport)\n#else\n' \
          '__attribute__((visibility("default")))\n#endif\n'
_SYMBOL = r'(?:func|psx_alias_body)_([0-9A-Fa-f]{8})'
_SIGNATURE = r'void (' + _SYMBOL + r')\(CPUState\* cpu(?:, uint32_t entry)?\)'
_BODY = re.compile(r'(?m)^(?:' + re.escape(_EXPORT) + r')?' + _SIGNATURE + r'\n\{\n')
_DECL = re.compile(r'(?m)^(?:' + re.escape(_EXPORT) + r')*' + _SIGNATURE + r';\n')
_REF = re.compile(r'\b' + _SYMBOL + r'\b')
_BODY_END = re.compile(r'(?m)^}\n')
_ALIAS_COMMENT = re.compile(r'/\* Overlapping-alias body for host func_[0-9A-Fa-f]{8}: \d+ entries \*/')
_FORWARD_MARKER = '/* Forward declarations */\n'
_PAIR_MARKER = 'PSX_OVERLAY_EXPORT uint64_t overlay_pair_id(void) {\n'
# Stable guest-address ranges keep an unrelated recipe's additions and removals
# from shifting every subsequent object. CPS alias wrappers follow their entry.
_ADDRESS_BITS = 10


def split_overlay(source, preamble):
    """Return the DLL-local shim and independently compilable guest code units.

    Only consume the emitter's recognized top-level layout. Unknown shapes
    retain the original monolithic compilation path.
    """
    before, marker, after = source.partition(_FORWARD_MARKER)
    if not marker or before.count(preamble) != 1:
        return None
    decls = {}
    for match in _DECL.finditer(after):
        decls[match[1]] = match[0]
    first = _BODY.search(after)
    if not first or _DECL.sub('', after[:first.start()]).strip():
        return None
    shim, marker, pair = after.partition(_PAIR_MARKER)
    if not marker:
        return None
    groups = {}
    cursor = first.start()
    for match in _BODY.finditer(shim, cursor):
        end = _BODY_END.search(shim, match.end())
        if end is None or _ALIAS_COMMENT.sub('', shim[cursor:match.start()]).strip():
            return None
        cursor = end.end()
        groups.setdefault(int(match[2], 16) >> _ADDRESS_BITS, []).append(shim[match.start():cursor])
    if shim[cursor:].strip():
        return None
    # Keep all local state and its exported callbacks in one DLL-owned TU.
    # Copies of the object containing any guest body bind to this DLL's shim.
    common = before.replace(preamble, '', 1)
    units = []
    for bodies in groups.values():
        body = '\n'.join(bodies)
        refs = {match[0] for match in _REF.finditer(body)}
        declarations = ''.join(decls[name] for name in sorted(refs & decls.keys()))
        units.append(common + declarations + '\n' + body)
    return before + '\n' + _PAIR_MARKER + pair, units


def compile_overlay(source_path, out_dll, gcc, env, flags, link_flags, includes,
                    preamble, object_cache, compiler_key):
    """Compile each equivalent guest code unit once and link the recipe's DLL."""
    source_path = Path(source_path)
    split = split_overlay(source_path.read_text(), preamble)
    if split is None:
        return False
    shim, units = split
    hits = 0
    with tempfile.TemporaryDirectory(dir=source_path.parent, prefix='objects-') as tmp:
        root = Path(tmp)
        obj_paths = []
        unit_cpp = root / 'unit.c'
        for i, unit in enumerate(units):
            obj = root / f'{i}.o'
            unit_cpp.write_text(unit)
            preprocessed = subprocess.run(
                [gcc, *flags, *includes, '-E', '-P', str(unit_cpp)],
                capture_output=True, env=env, check=True).stdout
            key = hashlib.sha256(compiler_key.encode() + preprocessed).hexdigest()
            artifact = object_cache / key[:2] / (key + '.obj')
            try:
                digest, data = artifact.read_bytes().split(b'\n', 1)
                if digest != hashlib.sha256(data).hexdigest().encode():
                    data = None
            except (OSError, ValueError):
                data = None
            if data is None:
                compiled_input = root / 'unit.i'
                compiled_input.write_bytes(preprocessed)
                subprocess.run([gcc, *flags, '-Werror=implicit-function-declaration', '-c', str(compiled_input), '-o', str(obj)],
                               capture_output=True, env=env, check=True)
                data = obj.read_bytes()
                publish_bytes(artifact, hashlib.sha256(data).hexdigest().encode() + b'\n' + data)
            else:
                hits += 1
                obj.write_bytes(data)
            obj_paths.append(obj)
        shim_c = root / 'shim.c'
        shim_c.write_text(shim)
        # A response file avoids the Windows process command-line limit on
        # large recipes. GCC parses forward-slash paths inside double quotes.
        response = root / 'objects.rsp'
        response.write_text('\n'.join(f'"{p.as_posix()}"' for p in obj_paths))
        subprocess.run([gcc, *link_flags, *flags, *includes, str(shim_c),
                        '@' + str(response), '-o', out_dll, '-lm'],
                       capture_output=True, env=env, check=True)
    print(f'  overlay objects: {len(units) - hits} compiled, {hits} reused', flush=True)
    return True
