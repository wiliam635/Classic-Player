#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build/macos-arm64-make/ClassicPlayer_artefacts/Release}"
STAGE="$ROOT/build/macos-package-root"
PACKAGES="$ROOT/build/macos-packages"
OUTPUT="${OUTPUT_DIR:-$ROOT/../../outputs/installers}"
PACKAGE_NAME="${PACKAGE_NAME:-Classic-Player-macOS-Apple-Silicon.pkg}"

rm -rf "$STAGE" "$PACKAGES"
mkdir -p "$STAGE/Applications" "$STAGE/Library/Audio/Plug-Ins/VST3" \
         "$STAGE/Library/Audio/Plug-Ins/Components" "$PACKAGES" "$OUTPUT"

ditto "$BUILD/Standalone/Classic Player.app" "$STAGE/Applications/Classic Player.app"
ditto "$BUILD/VST3/Classic Player.vst3" "$STAGE/Library/Audio/Plug-Ins/VST3/Classic Player.vst3"
ditto "$BUILD/AU/Classic Player.component" "$STAGE/Library/Audio/Plug-Ins/Components/Classic Player.component"

for bundle in "$STAGE/Library/Audio/Plug-Ins/VST3/Classic Player.vst3" \
              "$STAGE/Library/Audio/Plug-Ins/Components/Classic Player.component"; do
  version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$bundle/Contents/Info.plist")
  if [[ "$version" != "3.0.3" ]]; then
    echo "Erro: plugin com versão $version; esperado 3.0.3: $bundle" >&2
    exit 1
  fi
done

if [[ -n "${APPLE_SIGN_IDENTITY:-}" ]]; then
  codesign --force --deep --options runtime --timestamp --sign "$APPLE_SIGN_IDENTITY" "$STAGE/Applications/Classic Player.app"
  codesign --force --deep --options runtime --timestamp --sign "$APPLE_SIGN_IDENTITY" "$STAGE/Library/Audio/Plug-Ins/VST3/Classic Player.vst3"
  codesign --force --deep --options runtime --timestamp --sign "$APPLE_SIGN_IDENTITY" "$STAGE/Library/Audio/Plug-Ins/Components/Classic Player.component"
fi

pkgbuild --root "$STAGE" --identifier com.classickeys.classicplayer.bundle \
  --version 1.0.1 --install-location / \
  --component-plist "$ROOT/installer/macos/components.plist" \
  "$PACKAGES/ClassicPlayer-components.pkg"

productbuild --distribution "$ROOT/installer/macos/distribution.xml.in" \
  --package-path "$PACKAGES" "$OUTPUT/$PACKAGE_NAME"

if [[ -n "${APPLE_INSTALLER_IDENTITY:-}" ]]; then
  productsign --sign "$APPLE_INSTALLER_IDENTITY" \
    "$OUTPUT/$PACKAGE_NAME" \
    "$OUTPUT/Classic-Player-macOS-Apple-Silicon-signed.pkg"
fi

echo "Installer criado em: $OUTPUT"
