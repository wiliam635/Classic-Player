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
OUTPUT="${OUTPUT_DIR:-$ROOT/../../outputs/installers}"
PACKAGE_VERSION="3.0.1"
DOWNLOAD_VERSION="${DOWNLOAD_VERSION:-$PACKAGE_VERSION}"
PACKAGE_NAME="Classic-Player-${DOWNLOAD_VERSION}-macOS-High-Sierra-a-Tahoe-Universal.pkg"
ZIP_NAME="Classic-Player-${DOWNLOAD_VERSION}-macOS-High-Sierra-a-Tahoe-Universal-App.zip"
WORK="$(mktemp -d "$ROOT/build/package-macos-compat.XXXXXX")"
STAGE="$WORK/root"
PACKAGES="$WORK/packages"

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

mkdir -p "$STAGE/Applications" "$PACKAGES" "$OUTPUT"
ditto --noextattr --noqtn "$APP" "$STAGE/Applications/Classic Player.app"
test ! -L "$STAGE/Applications/Classic Player.app/Classic Player 2.0 macOS Universal.app"
cp "$ROOT/THIRD_PARTY_NORD_SAMPLE_LICENSE.txt" \
  "$STAGE/Applications/Classic Player.app/Contents/Resources/THIRD_PARTY_NORD_SAMPLE_LICENSE.txt"
/usr/bin/xattr -cr "$STAGE/Applications/Classic Player.app"
codesign --force --deep --sign - "$STAGE/Applications/Classic Player.app"
codesign --verify --deep --strict --verbose=2 "$STAGE/Applications/Classic Player.app"

pkgbuild --root "$STAGE" \
  --identifier com.classickeys.classicplayer.standalone \
  --version "$PACKAGE_VERSION" \
  --install-location / \
  --component-plist "$ROOT/installer/macos/components-standalone.plist" \
  "$PACKAGES/ClassicPlayer-standalone.pkg"

productbuild \
  --distribution "$ROOT/installer/macos/distribution-compat.xml" \
  --package-path "$PACKAGES" \
  "$OUTPUT/$PACKAGE_NAME"

ditto -c -k --sequesterRsrc --keepParent \
  "$STAGE/Applications/Classic Player.app" "$OUTPUT/$ZIP_NAME"
mkdir -p "$WORK/zip-verify"
ditto -xk "$OUTPUT/$ZIP_NAME" "$WORK/zip-verify"
codesign --verify --deep --strict --verbose=2 "$WORK/zip-verify/Classic Player.app"
pkgutil --check-signature "$OUTPUT/$PACKAGE_NAME" || true
pkgutil --check-signature "$PACKAGES/ClassicPlayer-standalone.pkg" || true
unzip -t "$OUTPUT/$ZIP_NAME"
echo "Instalador criado em: $OUTPUT/$PACKAGE_NAME"
echo "Aplicativo compactado em: $OUTPUT/$ZIP_NAME"
