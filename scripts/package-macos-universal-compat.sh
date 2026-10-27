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
VST3="${VST3_PATH:-$ROOT/build/Classic Player 2.0 macOS Universal.vst3}"
AU="${AU_PATH:-$ROOT/build/Classic Player 2.0 macOS Universal.component}"
for bundle in "$VST3" "$AU"; do
  if [ ! -d "$bundle/Contents/MacOS" ] || [ ! -f "$bundle/Contents/MacOS/Classic Player" ]; then
    echo "Erro: plugin obrigatório ausente ou incompleto: $bundle" >&2
    exit 1
  fi
done
if [ ! -f "$VST3/Contents/Resources/moduleinfo.json" ]; then
  echo "Erro: manifesto VST3 ausente: $VST3/Contents/Resources/moduleinfo.json" >&2
  exit 1
fi
OUTPUT="${OUTPUT_DIR:-$ROOT/../../outputs/installers}"
PACKAGE_VERSION="3.0.2"
DOWNLOAD_VERSION="${DOWNLOAD_VERSION:-$PACKAGE_VERSION}"
PACKAGE_NAME="Classic-Player-${DOWNLOAD_VERSION}-macOS-High-Sierra-a-Tahoe-Universal.pkg"
ZIP_NAME="Classic-Player-${DOWNLOAD_VERSION}-macOS-High-Sierra-a-Tahoe-Universal-App.zip"
WORK="$(mktemp -d "$ROOT/build/package-macos-compat.XXXXXX")"
STAGE="$WORK/root"
PACKAGES="$WORK/packages"

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

mkdir -p "$STAGE/Applications" \
  "$STAGE/Library/Audio/Plug-Ins/VST3" \
  "$STAGE/Library/Audio/Plug-Ins/Components" \
  "$PACKAGES" "$OUTPUT"
ditto --noextattr --noqtn "$APP" "$STAGE/Applications/Classic Player.app"
ditto --noextattr --noqtn "$VST3" "$STAGE/Library/Audio/Plug-Ins/VST3/Classic Player.vst3"
ditto --noextattr --noqtn "$AU" "$STAGE/Library/Audio/Plug-Ins/Components/Classic Player.component"
test ! -L "$STAGE/Applications/Classic Player.app/Classic Player 2.0 macOS Universal.app"
cp "$ROOT/THIRD_PARTY_NORD_SAMPLE_LICENSE.txt" \
  "$STAGE/Applications/Classic Player.app/Contents/Resources/THIRD_PARTY_NORD_SAMPLE_LICENSE.txt"
/usr/bin/xattr -cr "$STAGE/Applications/Classic Player.app"
codesign --force --deep --sign - "$STAGE/Applications/Classic Player.app"
codesign --verify --deep --strict --verbose=2 "$STAGE/Applications/Classic Player.app"
for bundle in \
  "$STAGE/Library/Audio/Plug-Ins/VST3/Classic Player.vst3" \
  "$STAGE/Library/Audio/Plug-Ins/Components/Classic Player.component"; do
  /usr/bin/xattr -cr "$bundle"
  codesign --force --deep --sign - "$bundle"
  codesign --verify --deep --strict --verbose=2 "$bundle"
done

pkgbuild --root "$STAGE" \
  --identifier com.classickeys.classicplayer.standalone \
  --version "$PACKAGE_VERSION" \
  --install-location / \
  --component-plist "$ROOT/installer/macos/components.plist" \
  "$PACKAGES/ClassicPlayer-standalone.pkg"

pkgutil --payload-files "$PACKAGES/ClassicPlayer-standalone.pkg" > "$WORK/payload-files.txt"
grep -Fq "Applications/Classic Player.app/Contents/Resources/AppIcon.icns" "$WORK/payload-files.txt"
grep -Fq "Library/Audio/Plug-Ins/VST3/Classic Player.vst3/Contents/MacOS/Classic Player" "$WORK/payload-files.txt"
grep -Fq "Library/Audio/Plug-Ins/Components/Classic Player.component/Contents/MacOS/Classic Player" "$WORK/payload-files.txt"

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
