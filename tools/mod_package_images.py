#!/usr/bin/env python3
"""Original inputs with one mod package's declarative byte operations applied.

AOT profiles may name executable bytes that exist only once a trusted mod
package is active: patched boot-EXE text, or an engine the patched EXE copies
to a fixed address. This reader reproduces the package's selected operations on
the original disc so the pipeline's existing methods and evidence checks can
read those bytes. It implements the static declarative subset of the runtime
resolver (runtime/src/mod_packages.cpp) and fails closed on anything else:

* Identity: exact manifest SHA-256, package id/version, a [[target]] matching
  the game id, original disc and boot-EXE hashes.
* Selection: named features with explicit or default option values. Choice and
  boolean values are validated; `when` / `when_option` conditions compare the
  effective values exactly as the runtime does.
* `disc_user` patches and file-backed `[[overlay]]` operations: payload and
  stock-range SHA-256 checks, applied to logical 2048-byte sector data --
  INCLUDING the boot EXE's own sectors, because the runtime serves every CD
  read through them and the BIOS loads the boot EXE with CD reads.
* `main_exe` patches: equal-length `expected`/`replace` bytes. The runtime
  checks every guard against RAM after the BIOS has loaded the boot EXE (so
  against the disc-operation-applied image) and, if any guard fails, rejects
  every main_exe write and boots unmodified. The view models exactly that: a
  guard must match the image as loaded, or the view fails closed rather than
  describe bytes the runtime would never produce.
* Plugins select native host callbacks. They carry no guest bytes, so they are
  reported, never modelled; a profile must list the exact selected set.
* `[[requirement]]` entries activate a feature of ANOTHER package while their
  `when` holds (runtime: implicit requirements). That package is not this view,
  so an active requirement is reported as "<package>/<feature>", never
  modelled; a profile must list the exact active set, the same rule as plugins.

`replace_from`, `fields`, `when_integer`, `disc_raw`, legacy packages and any
unknown manifest section or key are rejected rather than approximated.

A package that encodes the same boot-EXE edit twice -- once as a disc overlay
over the EXE's sectors and again as a main_exe patch guarded by the STOCK bytes
-- is exactly the plan the runtime rejects ("expected-byte guard failed"): the
disc copy has already changed the bytes the patch guards. Encode each edit once.
"""
import hashlib
from pathlib import Path, PurePosixPath
import struct
import tomllib

SECTOR = 2048
MANIFEST_KEYS = {'format_version', 'id', 'version', 'name', 'author', 'description', 'license',
                 'source_name', 'source_url', 'resolver', 'save_compatibility', 'author_link',
                 'target', 'feature', 'option', 'plugin', 'patch', 'overlay', 'requirement',
                 # Presentation / release metadata the runtime parses
                 # (mod_packages.cpp); it never changes the image.
                 'channel'}
CONDITION_KEYS = {'when', 'when_option', 'when_value'}
SECTION_KEYS = {
    'patch': {'feature', 'target', 'address', 'offset', 'expected', 'replace'} | CONDITION_KEYS,
    'overlay': {'feature', 'target', 'offset', 'file', 'sha256', 'expected_sha256'} | CONDITION_KEYS,
    'plugin': {'feature', 'id', 'order'} | CONDITION_KEYS,
    'requirement': {'feature', 'package', 'version', 'requires_feature'} | CONDITION_KEYS,
    # author / hidden / channel are launcher presentation and release
    # filtering (mod_packages.cpp parse); they never change the image.
    'feature': {'id', 'name', 'author', 'description', 'group', 'default_enabled', 'hidden',
                'channel'},
    'option': {'feature', 'id', 'label', 'description', 'group', 'type', 'default', 'choice',
               'min', 'max', 'step', 'slider', 'allow_wheel'},
    'target': {'game_id', 'disc_sha256', 'exe_sha256'},
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def hex_bytes(text):
    try:
        return bytes.fromhex(''.join(str(text).split()))
    except ValueError as error:
        raise ValueError(f'Invalid mod patch hex: {error}') from None


def boot_executable(disc):
    """The SYSTEM.CNF boot file, resolved against the ISO directory."""
    text = disc.read('SYSTEM.CNF').decode('ascii', 'replace')
    for line in text.splitlines():
        key, _, value = line.partition('=')
        if key.strip().upper() == 'BOOT':
            name = value.strip().split(':', 1)[-1].lstrip('\\/').split(';', 1)[0]
            name = name.replace('\\', '/').upper()
            require(name in disc.files, f'Boot executable missing from disc: {name}')
            return name
    raise ValueError('SYSTEM.CNF has no BOOT entry')


class ModPackageView:
    """Disc-like view: `files`, `read(name)`, `binary`, like the pipeline Disc."""

    def __init__(self, disc, project_root, spec, game_id):
        self.disc, self.name = disc, spec.get('name', spec['id'])
        self.files, self.binary = disc.files, disc.binary
        root = Path(project_root)
        manifest = (root / spec['manifest']).resolve()
        # Text identity: a checkout's line-ending conversion is not a change.
        text = manifest.read_bytes().replace(b'\r\n', b'\n')
        require(hashlib.sha256(text).hexdigest() == spec['manifest_sha256'],
                f'Mod manifest changed: {spec["manifest"]}')
        self.package_dir = manifest.parent
        self.manifest = tomllib.loads(manifest.read_text(encoding='utf-8-sig'))
        m = self.manifest
        unknown = set(m) - MANIFEST_KEYS
        require(not unknown, f'Unsupported mod manifest sections for AOT: {sorted(unknown)}')
        require((m.get('id'), m.get('version')) == (spec['id'], spec['version']),
                'Mod package id/version mismatch')
        require(m.get('resolver', 'declarative') == 'declarative', 'Only declarative packages are supported')
        require(number(m.get('format_version', 1)) >= 2 and m.get('feature'),
                'Legacy feature-less packages are not supported')
        for section, keys in SECTION_KEYS.items():
            for item in m.get(section, []):
                extra = set(item) - keys
                require(not extra, f'Unsupported [[{section}]] keys for AOT: {sorted(extra)}')
        self.boot = boot_executable(disc)
        exe = disc.read(self.boot)
        require(exe[:8] == b'PS-X EXE', f'{self.boot}: missing PS-X EXE header')
        self._verify_target(game_id, exe)
        self.values = self._select(spec.get('features', {}))
        self.plugins = sorted(p['id'] for p in m.get('plugin', []) if self._active(p))
        self.requirements = sorted({f"{r['package']}/{r['requires_feature']}"
                                    for r in m.get('requirement', []) if self._active(r)})
        self.exe_base = struct.unpack_from('<I', exe, 0x18)[0]
        self.exe_size = struct.unpack_from('<I', exe, 0x1C)[0]
        self._files = {}
        self._user_writes = []     # (offset, bytes) in logical 2048-byte user data
        self._main_writes = []     # (address, bytes)
        self._collect()

    # -- identity and selection ------------------------------------------------
    def _verify_target(self, game_id, exe):
        disc_sha = hashlib.sha256(Path(self.binary).read_bytes()).hexdigest()
        exe_sha = hashlib.sha256(exe).hexdigest()
        for target in self.manifest.get('target', []):
            if target.get('game_id') not in (game_id, '*'):
                continue
            if target.get('disc_sha256', disc_sha) != disc_sha:
                continue
            if target.get('exe_sha256', exe_sha) != exe_sha:
                continue
            return
        raise ValueError(f'Mod package does not target this game/disc: {self.manifest["id"]}')

    def _select(self, features):
        known = {f['id'] for f in self.manifest['feature']}
        require(features and set(features) <= known, 'Selected mod features must be declared')
        options = {}
        for option in self.manifest.get('option', []):
            options.setdefault(option['feature'], {})[option['id']] = option
        values = {}
        for feature, chosen in features.items():
            declared = options.get(feature, {})
            extra = set(chosen) - set(declared)
            require(not extra, f'Unknown option for feature {feature}: {sorted(extra)}')
            values[feature] = {}
            for option_id, option in declared.items():
                value = str(chosen.get(option_id, option.get('default', '')))
                kind = option.get('type')
                if kind == 'choice':
                    require(value in {c['value'] for c in option.get('choice', [])},
                            f'Invalid choice {feature}.{option_id}={value}')
                elif kind == 'boolean':
                    value = value.lower()
                    require(value in ('true', 'false'), f'Invalid boolean {feature}.{option_id}')
                values[feature][option_id] = value
        return values

    def _active(self, item):
        feature = item.get('feature')
        if feature not in self.values:
            return False
        when = dict(item.get('when', {}))
        if 'when_option' in item or 'when_value' in item:
            require('when_option' in item and 'when_value' in item,
                    'Condition requires both when_option and when_value')
            require(when.get(item['when_option'], item['when_value']) == item['when_value'],
                    'Conflicting mod conditions')
            when[item['when_option']] = item['when_value']
        for option_id, expected in when.items():
            require(option_id in self.values[feature], f'Condition names unknown option: {option_id}')
            if self.values[feature][option_id] != str(expected):
                return False
        return True

    # -- operations -------------------------------------------------------------
    def _original_user(self, offset, size):
        first, last = offset // SECTOR, (offset + size - 1) // SECTOR
        data = b''.join(self.disc.reader.read_sector_data(lba) for lba in range(first, last + 1))
        start = offset - first * SECTOR
        return data[start:start + size]

    def _collect(self):
        main_patches = []
        for patch in self.manifest.get('patch', []):
            if not self._active(patch):
                continue
            expected, replace = hex_bytes(patch['expected']), hex_bytes(patch.get('replace', ''))
            require(expected and len(expected) == len(replace),
                    'Mod patch expected/replace must be equal-length non-empty hex')
            target = patch['target']
            if target == 'main_exe':
                main_patches.append((number(patch['address']), expected, replace))
            elif target == 'disc_user':
                offset = number(patch['offset'])
                require(offset // SECTOR == (offset + len(expected) - 1) // SECTOR,
                        'disc_user patch crosses a sector boundary')
                require(self._original_user(offset, len(expected)) == expected,
                        f'disc_user expected bytes changed at {offset:#x}')
                self._user_writes.append((offset, replace))
            else:
                raise ValueError(f'Unsupported mod patch target for AOT: {target}')
        for overlay in self.manifest.get('overlay', []):
            if not self._active(overlay):
                continue
            require(overlay['target'] == 'disc_user',
                    f'Unsupported mod overlay target for AOT: {overlay["target"]}')
            relative = PurePosixPath(overlay['file'])
            require(not relative.is_absolute() and '..' not in relative.parts,
                    'Mod overlay path escapes its package')
            payload = (self.package_dir / relative).read_bytes()
            require(payload and hashlib.sha256(payload).hexdigest() == overlay['sha256'],
                    f'Mod overlay payload changed: {overlay["file"]}')
            offset = number(overlay['offset'])
            if 'expected_sha256' in overlay:
                original = self._original_user(offset, len(payload))
                require(hashlib.sha256(original).hexdigest() == overlay['expected_sha256'],
                        f'Mod overlay stock range changed: {overlay["file"]}')
            self._user_writes.append((offset, payload))
        spans = sorted((start, start + len(body)) for start, body in self._user_writes)
        require(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])),
                'Overlapping disc_user mod operations')

        # The boot EXE exactly as the BIOS loads it: through the CD path, so
        # with every disc operation over its sectors applied. main_exe guards
        # are checked against THAT image -- the runtime checks them against RAM
        # after the load -- and a failure there rejects the whole main_exe plan.
        exe = self._disc_file(self.boot)
        stock = self.disc.read(self.boot)
        require(exe[:8] == b'PS-X EXE' and exe[0x10:0x20] == stock[0x10:0x20],
                "Disc operations change the boot executable's PS-X EXE header "
                '(entry, load address or size); not modelled for AOT')
        for address, expected, replace in main_patches:
            start = 0x800 + address - self.exe_base
            require(address >= self.exe_base and start + len(expected) <= len(exe),
                    f'main_exe patch outside the boot executable image: {address:#x}')
            if exe[start:start + len(expected)] != expected:
                if stock[start:start + len(expected)] == expected:
                    raise ValueError(
                        f'main_exe expected-byte guard at {address:#x} matches the stock boot '
                        'executable but not the one the BIOS loads: a disc operation over the '
                        "EXE's sectors has already changed those bytes, so the runtime would "
                        'reject every main_exe write (the edit is encoded twice; keep one)')
                raise ValueError(f'main_exe expected bytes changed at {address:#x}')
            self._main_writes.append((address, replace))
        spans = sorted((start, start + len(body)) for start, body in self._main_writes)
        require(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])),
                'Overlapping main_exe mod operations')

    def _disc_file(self, name):
        """A file's bytes as CD reads return them: disc operations applied."""
        data = bytearray(self.disc.read(name))
        lba, size = self.files[name]
        start = lba * SECTOR
        for offset, body in self._user_writes:
            lo, hi = max(start, offset), min(start + size, offset + len(body))
            if lo < hi:
                data[lo - start:hi - start] = body[lo - offset:hi - offset]
        return data

    def read(self, name):
        name = name.upper()
        if name in self._files:
            return self._files[name]
        data = self._disc_file(name)
        if name == self.boot:
            for address, body in self._main_writes:
                offset = 0x800 + address - self.exe_base
                data[offset:offset + len(body)] = body
        self._files[name] = bytes(data)
        return self._files[name]

    def written_words(self):
        """(address, word) for every aligned boot-EXE word the package wrote.

        A main_exe write owns every word it covers. A disc operation over the
        boot EXE's sectors is sector-granular, so it owns only the words whose
        loaded value differs from the stock EXE -- an unchanged stock word
        inside a replaced sector is not evidence of anything the package did.
        Both layers count, because the runtime delivers both."""
        image = self.read(self.boot)
        stock = self.disc.read(self.boot)
        words = {}
        for address, body in sorted(self._main_writes):
            start = (address + 3) & ~3
            for word_address in range(start, address + len(body) - 3, 4):
                offset = 0x800 + word_address - self.exe_base
                words[word_address] = struct.unpack_from('<I', image, offset)[0]
        lba, size = self.files[self.boot]
        file_start = lba * SECTOR
        for offset, body in self._user_writes:
            lo = max(file_start, offset) - file_start
            hi = min(file_start + size, offset + len(body)) - file_start
            lo = max(lo, 0x800)  # the PS-X EXE header is not loaded to RAM
            lo = (lo + 3) & ~3
            for file_offset in range(lo, hi - 3, 4):
                word = struct.unpack_from('<I', image, file_offset)[0]
                if word != struct.unpack_from('<I', stock, file_offset)[0]:
                    words.setdefault(self.exe_base + file_offset - 0x800, word)
        return sorted(words.items())

    def receipt(self):
        """Inventory facts about the applied operations; never game bytes."""
        return dict(name=self.name, id=self.manifest['id'], version=self.manifest['version'],
                    features=self.values, plugins=self.plugins,
                    requirements=self.requirements, boot_executable=self.boot,
                    main_exe_writes=len(self._main_writes), disc_user_writes=len(self._user_writes),
                    main_exe_bytes=sum(len(b) for _, b in self._main_writes),
                    disc_user_bytes=sum(len(b) for _, b in self._user_writes))
