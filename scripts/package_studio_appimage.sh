#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$(realpath -e "${1:?build dir required}")"
OUT="$(realpath -m "${2:?output required}")"
mkdir -p "$(dirname "$OUT")"
APPDIR="$(mktemp -d)"
trap 'rm -rf -- "$APPDIR"' EXIT
DESTDIR="$APPDIR" cmake --install "$BUILD" --prefix /usr
test -x "$APPDIR/usr/bin/aurora_studio"
cp "$ROOT/packaging/space.bigrat.aurora-studio.desktop" "$APPDIR/space.bigrat.aurora-studio.desktop"
cp "$ROOT/packaging/icons/hicolor/512x512/apps/space.bigrat.aurora-studio.png" "$APPDIR/space.bigrat.aurora-studio.png"
ln -s space.bigrat.aurora-studio.png "$APPDIR/.DirIcon"
cat > "$APPDIR/AppRun" <<'APP_RUN'
#!/bin/sh
set -eu
appdir="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
export PATH="$appdir/usr/bin:${PATH:-/usr/bin:/bin}"
export XDG_DATA_DIRS="$appdir/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$appdir/usr/bin/aurora_studio" "$@"
APP_RUN
chmod +x "$APPDIR/AppRun"
appimagetool --appimage-arch x86_64 --output "$(dirname "$OUT")" --name "$(basename "$OUT")" "$APPDIR"
test -s "$OUT" && test -x "$OUT"
echo "Thin Studio AppImage: $OUT"
