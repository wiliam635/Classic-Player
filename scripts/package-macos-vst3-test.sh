#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${APP_PATH:-$ROOT/build/Classic Player 2.0 macOS Universal.app}"
VST3="${VST3_PATH:-$ROOT/outputs/vst3-test/Classic Player.vst3}"
AU="${AU_PATH:-$ROOT/outputs/vst3-test/Classic Player.component}"
OUTPUT="${OUTPUT_DIR:-$ROOT/outputs/vst3-test}"
PACKAGE="$OUTPUT/Classic-Player-2.0.4-Standalone-VST3-AU-teste-macOS-Universal.pkg"
APP_NAME="Classic Player Teste.app"
COMPONENTS="$ROOT/installer/macos/components-vst3-test.plist"
PACKAGE_ID="com.classickeys.classicplayer.vst3-au-test-201"
if [[ "${FINAL_RELEASE:-0}" == "1" ]]; then
  PACKAGE="$OUTPUT/Classic-Player-2.0.4-macOS-Universal.pkg"
  APP_NAME="Classic Player.app"
  COMPONENTS="$ROOT/installer/macos/components-vst3-final.plist"
  PACKAGE_ID="com.classickeys.classicplayer.standalone"
fi
STAGE="$(mktemp -d "$ROOT/build/package-vst3-test.XXXXXX")"
PACKAGES="$(mktemp -d "$ROOT/build/package-language.XXXXXX")"

cleanup() { rm -rf "$STAGE" "$PACKAGES"; }
trap cleanup EXIT

test -d "$APP"
test -d "$VST3"
test -d "$AU"
test -f "$APP/Contents/MacOS/ClassicPlayer"
test -f "$VST3/Contents/MacOS/Classic Player"
test -f "$AU/Contents/MacOS/Classic Player"
mkdir -p "$STAGE/Applications" "$STAGE/Library/Audio/Plug-Ins/VST3" \
  "$STAGE/Library/Audio/Plug-Ins/Components" "$OUTPUT"

# Keep the approved standalone installation untouched; only the test app has
# a distinct filename. The plug-in uses the standard VST3 discovery path.
ditto --noextattr --noqtn "$APP" "$STAGE/Applications/$APP_NAME"
ditto --noextattr --noqtn "$VST3" "$STAGE/Library/Audio/Plug-Ins/VST3/Classic Player.vst3"
ditto --noextattr --noqtn "$AU" "$STAGE/Library/Audio/Plug-Ins/Components/Classic Player.component"

codesign --verify --deep --strict "$STAGE/Applications/$APP_NAME"
codesign --verify --deep --strict "$STAGE/Library/Audio/Plug-Ins/VST3/Classic Player.vst3"
codesign --verify --deep --strict "$STAGE/Library/Audio/Plug-Ins/Components/Classic Player.component"
lipo "$STAGE/Applications/$APP_NAME/Contents/MacOS/ClassicPlayer" -verify_arch x86_64 arm64
lipo "$STAGE/Library/Audio/Plug-Ins/VST3/Classic Player.vst3/Contents/MacOS/Classic Player" -verify_arch x86_64 arm64
lipo "$STAGE/Library/Audio/Plug-Ins/Components/Classic Player.component/Contents/MacOS/Classic Player" -verify_arch x86_64 arm64

pkgbuild --root "$STAGE" \
  --identifier "$PACKAGE_ID" \
  --version 2.0.4 \
  --install-location / \
  --component-plist "$COMPONENTS" \
  "$PACKAGES/ClassicPlayer-components.pkg"

for language in br us es; do
  pkgbuild --root "$ROOT/installer/macos/languages/$language" \
    --identifier "com.classickeys.classicplayer.language.$language" --version 2.0.4 \
    --install-location "/Library/Application Support/Classic Keys/Classic Player" \
    "$PACKAGES/Language-$language.pkg"
done
sed "s/COMPONENT_PACKAGE_ID/$PACKAGE_ID/g" "$ROOT/installer/macos/distribution-language.xml" \
  > "$PACKAGES/Distribution.xml"
productbuild --distribution "$PACKAGES/Distribution.xml" --package-path "$PACKAGES" "$PACKAGE"
bash "$ROOT/scripts/verify-macos-installer-language.sh" "$PACKAGE"

pkgutil --payload-files "$PACKAGES/ClassicPlayer-components.pkg" | grep -F "Applications/$APP_NAME/Contents/MacOS/ClassicPlayer"
pkgutil --payload-files "$PACKAGES/ClassicPlayer-components.pkg" | grep -F 'Library/Audio/Plug-Ins/VST3/Classic Player.vst3/Contents/MacOS/Classic Player'
pkgutil --payload-files "$PACKAGES/ClassicPlayer-components.pkg" | grep -F 'Library/Audio/Plug-Ins/Components/Classic Player.component/Contents/MacOS/Classic Player'
echo "Instalador conjunto de teste criado em: $PACKAGE"
