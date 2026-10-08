#!/usr/bin/env bash
# Wrap a validated package_game_release.sh --stage-only Linux payload.
# No build, emitter or AOT invocation runs here. AppRun/desktop/PNG icon are
# caller-provided, fully rendered title metadata. AppRun must use usr/bin/<exe>
# and usr/share/<payload-name>, preserving user settings/cards in writable data.
# Usage: package_appimage.sh --payload DIR --exe-name NAME --payload-name NAME
#   --app-run FILE --desktop-file FILE.desktop --icon FILE.png --output FILE.AppImage
#   [--framework DIR] [--tool-cache DIR] [--jobs N]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FRAMEWORK="$(cd "${SCRIPT_DIR}/.." && pwd)"
PAYLOAD=""; EXE_NAME=""; PAYLOAD_NAME=""; APP_RUN=""; DESKTOP=""; ICON=""; OUTPUT=""
TOOLS="${XDG_CACHE_HOME:-${HOME:?}/.cache}/recomp-appimage-tools"
JOBS=2
while [[ $# -gt 0 ]]; do
  case "$1" in
    --payload) PAYLOAD="${2:?}"; shift 2;;
    --exe-name) EXE_NAME="${2:?}"; shift 2;;
    --payload-name) PAYLOAD_NAME="${2:?}"; shift 2;;
    --app-run) APP_RUN="${2:?}"; shift 2;;
    --desktop-file) DESKTOP="${2:?}"; shift 2;;
    --icon) ICON="${2:?}"; shift 2;;
    --output) OUTPUT="${2:?}"; shift 2;;
    --framework) FRAMEWORK="${2:?}"; shift 2;;
    --tool-cache) TOOLS="${2:?}"; shift 2;;
    --jobs) JOBS="${2:?}"; shift 2;;
    -h|--help) sed -n '2,8p' "$0"; exit 0;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || {
  echo "AppImage packaging requires native Linux x86_64" >&2; exit 1;
}
for name in EXE_NAME PAYLOAD_NAME; do
  [[ "${!name}" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]*$ ]] || {
    echo "invalid ${name}" >&2; exit 2;
  }
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "invalid jobs" >&2; exit 2; }
[[ -d "$PAYLOAD" && -n "$OUTPUT" && ! -e "$OUTPUT" && ! -L "$OUTPUT" ]] || {
  echo "payload must exist and output must be a fresh path" >&2; exit 1;
}
PAYLOAD="$(cd "$PAYLOAD" && pwd)"
FRAMEWORK="$(cd "$FRAMEWORK" && pwd)"
for file in "$APP_RUN" "$DESKTOP" "$ICON" "$PAYLOAD/$EXE_NAME" "$PAYLOAD/$EXE_NAME.execution.json"; do
  [[ -f "$file" ]] || { echo "missing ${file}" >&2; exit 1; }
done
[[ -d "$PAYLOAD/assets" && -f "$PAYLOAD/game.toml" ]] || {
  echo "payload is missing staged runtime assets/config" >&2; exit 1;
}
DESKTOP_ID="$(basename "$DESKTOP" .desktop)"
[[ "$DESKTOP" == *.desktop && "$DESKTOP_ID" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]*$ ]] || {
  echo "invalid desktop metadata filename" >&2; exit 1;
}
PY="${PSX_RELEASE_STAGE_PYTHON:-python3}"
"$PY" - "$PAYLOAD/$EXE_NAME" "$ICON" <<'PY'
import sys
from pathlib import Path
with Path(sys.argv[1]).open('rb') as stream:
    elf_magic = stream.read(4)
with Path(sys.argv[2]).open('rb') as stream:
    png_magic = stream.read(8)
if elf_magic != b'\x7fELF':
    raise SystemExit('Linux payload executable must be ELF')
if png_magic != b'\x89PNG\r\n\x1a\n':
    raise SystemExit('AppImage icon must be PNG')
PY
if find "$PAYLOAD" -type f \( -iname '*.dll' -o -iname '*.exe' \) -print -quit | grep -q .; then
  echo "Linux payload contains Windows binaries/cache" >&2; exit 1
fi
sh -n "$APP_RUN"
APPDIR="$(mktemp -d "${TMPDIR:-/tmp}/psx-appimage.XXXXXXXX")"
case "$(stat -f -c %T "$APPDIR")" in
  9p|drvfs|ntfs*) echo "AppDir must be on the native Linux filesystem: $APPDIR" >&2; exit 1;;
esac
echo "AppDir: $APPDIR"
DATA="$APPDIR/usr/share/$PAYLOAD_NAME"
mkdir -p "$APPDIR/usr/bin" "$DATA" "$(dirname "$OUTPUT")" "$TOOLS"
cp -a "$PAYLOAD/." "$DATA/"
rm -f "$DATA/$EXE_NAME" "$DATA/$EXE_NAME.execution.json"
install -m 0755 "$PAYLOAD/$EXE_NAME" "$APPDIR/usr/bin/$EXE_NAME"
install -m 0755 "$APP_RUN" "$APPDIR/AppRun"
install -m 0644 "$DESKTOP" "$APPDIR/$DESKTOP_ID.desktop"
install -m 0644 "$ICON" "$APPDIR/$DESKTOP_ID.png"
ln -s "$DESKTOP_ID.png" "$APPDIR/.DirIcon"
# SDL resolves UI resources from the real ELF, rather than writable argv[0].
ln -s "../share/$PAYLOAD_NAME/assets" "$APPDIR/usr/bin/assets"
HELPER="$FRAMEWORK/tools/release_stage.py"
# Official GitHub asset digests checked 2026-10-07 UTC; versioned releases
# avoid drifting continuous URLs. Asset IDs313839329/324406736 respectively.
# These releases are not marked immutable; SHA verification remains mandatory.
LINUXDEPLOY_SHA=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
APPIMAGETOOL_SHA=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
LINUXDEPLOY="$TOOLS/linuxdeploy-x86_64.AppImage"
APPIMAGETOOL="$TOOLS/appimagetool-x86_64.AppImage"
"$PY" "$HELPER" fetch-pinned --url https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage \
  --sha256 "$LINUXDEPLOY_SHA" --destination "$LINUXDEPLOY"
"$PY" "$HELPER" fetch-pinned --url https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
  --sha256 "$APPIMAGETOOL_SHA" --destination "$APPIMAGETOOL"
chmod 0755 "$LINUXDEPLOY" "$APPIMAGETOOL"
export NO_STRIP=1
"$LINUXDEPLOY" --appimage-extract-and-run --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/$EXE_NAME" --desktop-file "$APPDIR/$DESKTOP_ID.desktop" \
  --icon-file "$APPDIR/$DESKTOP_ID.png"
# Deployment may rewrite RPATH. The original sidecar supplies the contract,
# while the helper verifies its embedded identity and hashes the FINAL ELF;
# a prior payload binary_sha256 is never accepted as the final-byte receipt.
"$PY" "$HELPER" stage-execution --binary "$APPDIR/usr/bin/$EXE_NAME" \
  --manifest "$PAYLOAD/$EXE_NAME.execution.json" \
  --output "$APPDIR/usr/bin/$EXE_NAME.execution.json"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"
[[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] || { echo "invalid SOURCE_DATE_EPOCH" >&2; exit 2; }
export SOURCE_DATE_EPOCH
find "$APPDIR" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
ARCH=x86_64 "$APPIMAGETOOL" --appimage-extract-and-run \
  --mksquashfs-opt -processors --mksquashfs-opt "$JOBS" "$APPDIR" "$OUTPUT"
chmod 0755 "$OUTPUT"
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
echo "AppImage: $OUTPUT"
