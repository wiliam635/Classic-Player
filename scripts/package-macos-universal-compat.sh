#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${APP_PATH:-}"
if [ -z "$APP" ]; then
  APP="$(find "$ROOT/build" -type d -path "*/ClassicPlayerApp_artefacts/Release/Classic Player.app" -print -quit 2>/dev/null || true)"
fi
if [ -z "$APP" ] || [ ! -d "$APP" ]; then
  echo "Erro: Classic Player.app não encontrado no build."
  find "$ROOT/build" -type d -name "*.app" -print 2>/dev/null || true
  exit 1
fi
# Compatibility path for older workflow checks.
ln -sfn "$APP" "$ROOT/build/Classic Player 1.6.1 macOS Universal.app"
OUTPUT="${OUTPUT_DIR:-$ROOT/../../outputs/installers}"
PACKAGE_NAME="Classic-Player-1.6.1-macOS-High-Sierra-a-Tahoe-Universal.pkg"
ZIP_NAME="Classic-Player-1.6.1-macOS-High-Sierra-a-Tahoe-Universal-App.zip"
WORK="$(mktemp -d "$ROOT/build/package-macos-compat.XXXXXX")"
STAGE="$WORK/root"
PACKAGES="$WORK/packages"

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

mkdir -p "$STAGE/Applications" "$PACKAGES" "$OUTPUT"
ditto --noextattr --noqtn "$APP" "$STAGE/Applications/Classic Player.app"
/usr/bin/xattr -cr "$STAGE/Applications/Classic Player.app"

pkgbuild --root "$STAGE" \
  --identifier com.classickeys.classicplayer.standalone \
  --version 1.6.1 \
  --install-location / \
  --component-plist "$ROOT/installer/macos/components-standalone.plist" \
  "$PACKAGES/ClassicPlayer-standalone.pkg"

productbuild \
  --distribution "$ROOT/installer/macos/distribution-compat.xml" \
  --package-path "$PACKAGES" \
  "$OUTPUT/$PACKAGE_NAME"

ditto -c -k --sequesterRsrc --keepParent "$APP" "$OUTPUT/$ZIP_NAME"
pkgutil --check-signature "$OUTPUT/$PACKAGE_NAME" || true
pkgutil --check-signature "$PACKAGES/ClassicPlayer-standalone.pkg" || true
unzip -t "$OUTPUT/$ZIP_NAME"
echo "Instalador criado em: $OUTPUT/$PACKAGE_NAME"
echo "Aplicativo compactado em: $OUTPUT/$ZIP_NAME"
