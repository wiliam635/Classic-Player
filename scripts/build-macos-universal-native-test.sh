#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/Documents/Codex/vcpkg}"
CMAKE="${CMAKE:-/Applications/CMake.app/Contents/bin/cmake}"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
DEPS="$ROOT/build/vcpkg-universal"
JUCE_ARGS=(-DCLASSIC_PLAYER_FETCH_JUCE=ON)
if [[ -n "${JUCE_DIR:-}" ]]; then
  JUCE_ARGS=(-DJUCE_DIR="$JUCE_DIR" -DCLASSIC_PLAYER_FETCH_JUCE=OFF)
fi

configure_and_build() {
  local arch="$1"
  local triplet="$2"
  local build="$ROOT/build/native-test-$arch"
  local prefix="$DEPS/$arch/installed/$triplet"

  "$CMAKE" -S "$ROOT" -B "$build" -G "Unix Makefiles" \
    "${JUCE_ARGS[@]}" \
    -DCLASSIC_PLAYER_STANDALONE_ONLY=ON \
    -DCMAKE_PREFIX_PATH="$prefix" \
    -DCMAKE_C_COMPILER=/usr/bin/clang \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_SYSROOT="$SDK" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DCMAKE_BUILD_TYPE=Release
  "$CMAKE" --build "$build" --target ClassicPlayer_Standalone --parallel 4
}

configure_and_build arm64 arm64-osx-classic
configure_and_build x86_64 x64-osx-classic

ARM_APP="$ROOT/build/native-test-arm64/ClassicPlayer_artefacts/Release/Standalone/Classic Player.app"
X64_APP="$ROOT/build/native-test-x86_64/ClassicPlayer_artefacts/Release/Standalone/Classic Player.app"
UNIVERSAL="$ROOT/build/Classic Player 1.6.0 Preview.app"

rm -rf "$UNIVERSAL"
ditto "$ARM_APP" "$UNIVERSAL"
lipo -create \
  "$ARM_APP/Contents/MacOS/Classic Player" \
  "$X64_APP/Contents/MacOS/Classic Player" \
  -output "$UNIVERSAL/Contents/MacOS/Classic Player"

# Use an executable name without spaces and a fresh bundle identifier.  This
# avoids stale LaunchServices registrations left by the earlier WebView and
# plug-in-wrapper builds that used the same path and bundle identifier.
mv "$UNIVERSAL/Contents/MacOS/Classic Player" \
   "$UNIVERSAL/Contents/MacOS/ClassicPlayer"
/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable ClassicPlayer" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier com.classickeys.classicplayer.native160preview2" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString 1.6.0" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion 1.6.0" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Add :CFBundleInfoDictionaryVersion string 6.0" "$UNIVERSAL/Contents/Info.plist" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Set :CFBundleInfoDictionaryVersion 6.0" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Add :CFBundleDevelopmentRegion string pt_BR" "$UNIVERSAL/Contents/Info.plist" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Set :CFBundleDevelopmentRegion pt_BR" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string 12.0" "$UNIVERSAL/Contents/Info.plist" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion 12.0" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Add :NSPrincipalClass string NSApplication" "$UNIVERSAL/Contents/Info.plist" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Set :NSPrincipalClass NSApplication" "$UNIVERSAL/Contents/Info.plist"
# JUCE resources can inherit Finder quarantine attributes from the downloaded
# SDK. A nested quarantined nib is enough for LaunchServices to reject the app.
/usr/bin/xattr -cr "$UNIVERSAL"
codesign --force --deep --sign - "$UNIVERSAL"

file "$UNIVERSAL/Contents/MacOS/ClassicPlayer"
codesign --verify --deep --strict --verbose=2 "$UNIVERSAL"
echo "Aplicativo nativo universal criado em: $UNIVERSAL"
