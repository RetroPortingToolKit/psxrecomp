"""Write host-owned stereo manifests from TCP metadata and complete PNG sets."""
import json
from pathlib import Path


def write_capture_manifests(directory, records):
    directory = Path(directory)
    written = 0
    for record in records:
        pair_id = int(record["pair_id"])
        if pair_id < 0:
            raise ValueError("negative capture pair ID")
        prefix = f"p{pair_id:06d}"
        images = [directory / f"{prefix}_{eye}.png" for eye in ("left", "right", "sbs")]
        if not all(path.is_file() and path.stat().st_size for path in images):
            continue
        manifest = directory / f"{prefix}.json"
        if not manifest.exists():
            manifest.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
            written += 1
    return written
