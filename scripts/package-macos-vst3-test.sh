#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${APP_PATH:-$ROOT/build/Classic Player 2.0 macOS Universal.app}"
VST3="${VST3_PATH:-$ROOT/outputs/vst3-test/Classic Player.vst3}"
OUTPUT="${OUTPUT_DIR:-$ROOT/outputs/vst3-test}"
PACKAGE="$OUTPUT/Classic-Player-2.0.0-Standalone-VST3-teste-macOS-Universal.pkg"
STAGE="$(mktemp -d "$ROOT/build/package-vst3-test.XXXXXX")"

cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

test -d "$APP"
test -d "$VST3"
test -f "$APP/Contents/MacOS/ClassicPlayer"
test -f "$VST3/Contents/MacOS/Classic Player"
mkdir -p "$STAGE/Applications" "$STAGE/Library/Audio/Plug-Ins/VST3" "$OUTPUT"

# Keep the approved standalone installation untouched; only the test app has
# a distinct filename. The plug-in uses the standard VST3 discovery path.
ditto --noextattr --noqtn "$APP" "$STAGE/Applications/Classic Player VST3 Teste.app"
ditto --noextattr --noqtn "$VST3" "$STAGE/Library/Audio/Plug-Ins/VST3/Classic Player.vst3"

codesign --verify --deep --strict "$STAGE/Applications/Classic Player VST3 Teste.app"
codesign --verify --deep --strict "$STAGE/Library/Audio/Plug-Ins/VST3/Classic Player.vst3"
lipo "$STAGE/Applications/Classic Player VST3 Teste.app/Contents/MacOS/ClassicPlayer" -verify_arch x86_64 arm64
lipo "$STAGE/Library/Audio/Plug-Ins/VST3/Classic Player.vst3/Contents/MacOS/Classic Player" -verify_arch x86_64 arm64

pkgbuild --root "$STAGE" \
  --identifier com.classickeys.classicplayer.vst3-test \
  --version 2.0.0 \
  --install-location / \
  "$PACKAGE"

pkgutil --payload-files "$PACKAGE" | grep -F 'Applications/Classic Player VST3 Teste.app/Contents/MacOS/ClassicPlayer'
pkgutil --payload-files "$PACKAGE" | grep -F 'Library/Audio/Plug-Ins/VST3/Classic Player.vst3/Contents/MacOS/Classic Player'
echo "Instalador conjunto de teste criado em: $PACKAGE"
