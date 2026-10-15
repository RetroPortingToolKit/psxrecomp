"""Virtual files an AOT profile derives from the original disc.

An engine can run code that no disc file contains verbatim: a boot EXE that
unpacks an inner image, overlays stored compressed. The loader's transform is
deterministic, so its output is as much an original input as a raw file, as
long as the transform and every byte it consumes and produces are pinned.

A profile declares such outputs in `prepared_files`; each is then an ordinary
named file for every image method and evidence check (`fixed_address_files`,
`words`, `pointer_strings`, ...). Nothing derived is stored: it is rebuilt from
the user's own disc whenever the profile is used, and the profile carries only
hashes, offsets and sizes.

    "prepared_files": [{
      "name": "ENGINE.IMG",                     # unique; never a real disc file
      "method": "bpe_container",
      "source": {"file": "BOOT.EXE", "file_offset": "0xA00"},
      "magic": "0x1A455042",                    # declared by the method
      "consumed": "0x36228",                    # input bytes the decoder reads
      "source_sha256": "...",                   # of exactly those bytes
      "size": "0xD7800",                        # decoded size
      "sha256": "..."                           # of the decoded bytes
    }]

`file_aliases` maps a logical name to the per-disc names of one file, for sets
whose discs name an identical file differently (SCUS_944.91, SCUS_945.84, ...).
Exactly one candidate must exist on the disc being read.

Methods are decoders over bytes only; they never see a path, the disc, or the
profile's other sections.
"""
import hashlib

import bpe_container


class PreparedError(ValueError):
    """A declared derived file did not reproduce from the disc."""


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def _bpe_container(data, spec):
    return bpe_container.decode(data, magic=number(spec['magic']), offset=0)


# method -> (decoder(data_from_source_offset, spec) -> (output, consumed),
#            keys the spec must declare beyond the common ones)
METHODS = {'bpe_container': (_bpe_container, ('magic',))}
COMMON_KEYS = ('name', 'method', 'source', 'consumed', 'source_sha256', 'size', 'sha256')


class PreparedDisc:
    """An original disc plus the profile's aliases and derived files.

    Reads of a declared alias or prepared name are answered here; everything
    else (`files`, `binary`, `reader`, ...) is the underlying disc's."""

    def __init__(self, disc, aliases=None, prepared=None, receipts=None):
        self.disc = disc
        self.aliases = dict(aliases or {})
        self.prepared = dict(prepared or {})
        self.receipts = list(receipts or [])

    def __getattr__(self, name):
        # Only reached for attributes this wrapper does not define.
        return getattr(self.disc, name)

    def read(self, name):
        key = name.upper()
        if key in self.prepared:
            return self.prepared[key]
        return self.disc.read(self.aliases.get(key, key))


def resolve_aliases(disc, profile):
    """logical name -> the one candidate that exists on this disc."""
    resolved = {}
    for logical, candidates in profile.get('file_aliases', {}).items():
        key = logical.upper()
        if key in disc.files:
            raise PreparedError(f'File alias {logical} is the name of a real disc file')
        if not candidates or not all(isinstance(c, str) and c for c in candidates):
            raise PreparedError(f'File alias {logical} needs candidate file names')
        present = [c.upper() for c in candidates if c.upper() in disc.files]
        if len(present) != 1:
            raise PreparedError(
                f'File alias {logical} needs exactly one of {sorted(c.upper() for c in candidates)} '
                f'on the disc, found {present}')
        resolved[key] = present[0]
    return resolved


def prepare(disc, profile):
    """The disc as the profile sees it: aliases resolved, derived files built.

    Every derived file is verified (input extent, decoded size and hash) before
    any image method can read it; a mismatch is a PreparedError."""
    aliases = resolve_aliases(disc, profile)
    view = PreparedDisc(disc, aliases)
    for spec in profile.get('prepared_files', []):
        method = METHODS.get(spec.get('method'))
        if method is None:
            raise PreparedError(f"Unknown prepared file method: {spec.get('method')}")
        decoder, extra = method
        missing = [key for key in COMMON_KEYS + extra if key not in spec]
        if missing:
            raise PreparedError(f"Prepared file {spec.get('name')} lacks {missing}")
        name = spec['name'].upper()
        if name in view.prepared or name in aliases or name in disc.files:
            raise PreparedError(f'Prepared file name is not unique: {name}')
        source = spec['source']
        data = view.read(source['file'])
        start = number(source.get('file_offset', 0))
        if not 0 <= start < len(data):
            raise PreparedError(f'Prepared file {name}: source offset outside {source["file"]}')
        try:
            output, consumed = decoder(data[start:], spec)
        except ValueError as exc:
            raise PreparedError(f'Prepared file {name} does not decode: {exc}') from exc
        if consumed != number(spec['consumed']):
            raise PreparedError(f'Prepared file {name}: decoder consumed {consumed:#x} bytes, '
                                f"profile pins {number(spec['consumed']):#x}")
        if hashlib.sha256(data[start:start + consumed]).hexdigest() != spec['source_sha256']:
            raise PreparedError(f'Prepared file {name}: source bytes changed')
        if len(output) != number(spec['size']):
            raise PreparedError(f'Prepared file {name}: decoded {len(output):#x} bytes, '
                                f"profile pins {number(spec['size']):#x}")
        if hashlib.sha256(output).hexdigest() != spec['sha256']:
            raise PreparedError(f'Prepared file {name}: decoded bytes changed')
        view.prepared[name] = output
        view.receipts.append(dict(name=name, method=spec['method'], source=source['file'].upper(),
                                  source_offset=start, consumed=consumed,
                                  source_sha256=spec['source_sha256'], size=len(output),
                                  sha256=spec['sha256']))
    return view
