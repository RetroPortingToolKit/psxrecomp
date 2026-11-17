"""Hash declared build inputs. No binary-equivalence or qualification claim."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def write_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def build(spec):
    parts = spec.read_text(encoding="utf-8").split(chr(31))
    if len(parts) % 2:
        raise ValueError("malformed manifest specification")
    fields = dict(zip(parts[::2], parts[1::2]))
    if len(fields) * 2 != len(parts) or fields.pop("schema", None) != "1":
        raise ValueError("duplicate fields or unsupported manifest schema")
    source = Path(fields.pop("source_dir")).resolve()
    roots = []
    labels = set()
    for entry in fields.pop("roots").split(";"):
        label, separator, path = entry.partition("=")
        if not separator or not re.fullmatch(r"[A-Za-z0-9_.-]+", label) or label in labels:
            raise ValueError("invalid/duplicate logical root")
        labels.add(label)
        roots.append((label, Path(path).resolve()))
    roots.sort(key=lambda item: len(str(item[1])), reverse=True)

    def logical(path):
        for label, root in roots:
            try:
                return label + "/" + path.relative_to(root).as_posix()
            except ValueError:
                pass
        raise ValueError("unmapped manifest input: " + str(path))

    def normalize(text):
        for label, root in roots:
            # CMake emits forward slashes; source properties may use native ones.
            for spelling in {root.as_posix(), str(root)}:
                # A root named /game must not rewrite an unrelated /game-other.
                pattern = re.escape(spelling) + r"(?=$|[/\\;\s\"',>])"
                text = re.sub(pattern, lambda match: "${" + label + "}", text)
        return text

    files = {}
    for group in ("sources", "headers", "inputs"):
        for raw in fields.pop(group, "").split(";"):
            if not raw:
                continue
            path = Path(raw)
            if not path.is_absolute():
                path = source / path
            path = path.resolve()
            key = logical(path)
            if not path.is_file():
                raise ValueError("missing manifest input: " + str(path))
            before = path.stat()
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            after = path.stat()
            if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise ValueError("manifest input changed while hashing: " + str(path))
            value = dict(sha256=digest.hexdigest(), bytes=after.st_size)
            if key in files and files[key] != value:
                raise ValueError("colliding logical input: " + key)
            files[key] = value
    config = {}
    for key, value in fields.items():
        key, value = normalize(key), normalize(value)
        if key in config:
            raise ValueError("colliding normalized config key")
        config[key] = value
    manifest = dict(schema=1, coverage="declared-build-inputs-v1",
                    whole_binary=False, heterogeneous_qualified=False,
                    files=files, configuration=config)
    canonical = json.dumps(manifest, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
    return manifest, hashlib.sha256(canonical).hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--spec", type=Path)
    source.add_argument("--spec-pattern", type=Path)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    if args.spec:
        manifest, digest = build(args.spec)
    else:
        specs = sorted(args.spec_pattern.parent.glob(args.spec_pattern.name))
        if not specs:
            raise ValueError("no generated compile-language manifest specifications")
        manifest = dict(schema=1, coverage="declared-build-inputs-v1",
                        whole_binary=False, heterogeneous_qualified=False,
                        files={}, configuration_by_language={})
        for spec in specs:
            part, _ = build(spec)
            language = part["configuration"].get("compile_language", "")
            if language in manifest["configuration_by_language"]:
                raise ValueError("duplicate compile-language manifest")
            manifest["configuration_by_language"][language] = part["configuration"]
            for key, value in part["files"].items():
                if key in manifest["files"] and manifest["files"][key] != value:
                    raise ValueError("input changed between compile-language manifests: " + key)
                manifest["files"][key] = value
        canonical = json.dumps(manifest, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
        digest = hashlib.sha256(canonical).hexdigest()
    manifest["sha256"] = digest
    write_changed(args.manifest, (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode())
    write_changed(args.header, ("#pragma once\n#define PSX_BUILD_MANIFEST_SHA256 \"" + digest +
                  "\"\n#define PSX_BUILD_MANIFEST_SCHEMA 1\n").encode())


if __name__ == "__main__":
    main()
