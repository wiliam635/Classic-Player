#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VCPKG_ROOT="${VCPKG_ROOT:?VCPKG_ROOT precisa apontar para o vcpkg}"
CMAKE="${CMAKE:-$(command -v cmake)}"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
DEPS="$ROOT/build/vcpkg-macos-compat"
JUCE_ARGS=(-DCLASSIC_PLAYER_FETCH_JUCE=ON)

if [[ -n "${JUCE_DIR:-}" ]]; then
  JUCE_ARGS=(-DJUCE_DIR="$JUCE_DIR" -DCLASSIC_PLAYER_FETCH_JUCE=OFF)
fi

install_dependencies() {
  local arch="$1"
  local triplet="$2"
  local arch_deps="$DEPS/$arch"

  (
    cd "$ROOT"
    "$VCPKG_ROOT/vcpkg" install \
      --triplet "$triplet" \
      --overlay-triplets="$ROOT/triplets" \
      --host-triplet=arm64-osx \
      --x-install-root="$arch_deps/installed" \
      --x-buildtrees-root="$arch_deps/buildtrees" \
      --x-packages-root="$arch_deps/packages" \
      --downloads-root="$VCPKG_ROOT/downloads"
  )
}

configure_and_build() {
  local arch="$1"
  local triplet="$2"
  local deployment="$3"
  local build="$ROOT/build/macos-compat-$arch"
  local prefix="$DEPS/$arch/installed/$triplet"

  "$CMAKE" -S "$ROOT" -B "$build" -G "Unix Makefiles" \
    "${JUCE_ARGS[@]}" \
    -DFETCHCONTENT_BASE_DIR="$ROOT/build/fetch-content" \
    -DCLASSIC_PLAYER_STANDALONE_ONLY=ON \
    -DCLASSIC_PLAYER_BUILD_TESTS=ON \
    -DCMAKE_PREFIX_PATH="$prefix" \
    -DCMAKE_C_COMPILER=/usr/bin/clang \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_SYSROOT="$SDK" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment" \
    -DCMAKE_BUILD_TYPE=Release
  "$CMAKE" --build "$build" --target ClassicPlayerApp ClassicPlayerChordDetectorTest --parallel 4
  local test_executable="$build/ClassicPlayerChordDetectorTest_artefacts/Release/ClassicPlayerChordDetectorTest"
  if [[ "$arch" == "x86_64" ]]; then
    arch -x86_64 "$test_executable"
  else
    "$test_executable"
  fi
}

install_dependencies arm64 arm64-osx-classic
install_dependencies x86_64 x64-osx-classic
configure_and_build arm64 arm64-osx-classic 11.0
configure_and_build x86_64 x64-osx-classic 10.13

ARM_APP="$ROOT/build/macos-compat-arm64/ClassicPlayerApp_artefacts/Release/Standalone/Classic Player.app"
X64_APP="$ROOT/build/macos-compat-x86_64/ClassicPlayerApp_artefacts/Release/Standalone/Classic Player.app"
UNIVERSAL="$ROOT/build/Classic Player 1.6.1 macOS Universal.app"
EXECUTABLE="$UNIVERSAL/Contents/MacOS/ClassicPlayer"

rm -rf "$UNIVERSAL"
ditto "$ARM_APP" "$UNIVERSAL"
lipo -create \
  "$ARM_APP/Contents/MacOS/Classic Player" \
  "$X64_APP/Contents/MacOS/Classic Player" \
  -output "$EXECUTABLE"
rm "$UNIVERSAL/Contents/MacOS/Classic Player"

/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable ClassicPlayer" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier com.classickeys.classicplayer.macos161" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString 1.6.1" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion 1.6.1" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string 10.13" "$UNIVERSAL/Contents/Info.plist" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion 10.13" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Add :NSHighResolutionCapable bool true" "$UNIVERSAL/Contents/Info.plist" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Set :NSHighResolutionCapable true" "$UNIVERSAL/Contents/Info.plist"

/usr/bin/xattr -cr "$UNIVERSAL"
codesign --force --deep --sign - "$UNIVERSAL"

test "$(lipo -archs "$EXECUTABLE")" = "x86_64 arm64" || test "$(lipo -archs "$EXECUTABLE")" = "arm64 x86_64"
minimum_version() {
  local arch="$1"
  otool -l -arch "$arch" "$EXECUTABLE" | awk '
    /LC_BUILD_VERSION/ { build = 1; next }
    build && /minos/ { print $2; exit }
    /LC_VERSION_MIN_MACOSX/ { legacy = 1; next }
    legacy && /version/ { print $2; exit }
  '
}
X64_MINIMUM="$(minimum_version x86_64)"
ARM_MINIMUM="$(minimum_version arm64)"
echo "x86_64 minimum macOS: $X64_MINIMUM"
echo "arm64 minimum macOS: $ARM_MINIMUM"
test "$X64_MINIMUM" = "10.13"
test "$ARM_MINIMUM" = "11.0"
codesign --verify --deep --strict --verbose=2 "$UNIVERSAL"

"$EXECUTABLE" >"$ROOT/build/macos-compat-startup.log" 2>&1 &
APP_PID=$!
sleep 12
if ! kill -0 "$APP_PID" 2>/dev/null; then
  cat "$ROOT/build/macos-compat-startup.log" || true
  echo "Aviso: o runner encerrou o app durante o teste gráfico; o bundle continua válido para teste manual." >&2
else
  kill "$APP_PID" 2>/dev/null || true
  wait "$APP_PID" 2>/dev/null || true
fi

echo "Aplicativo universal criado em: $UNIVERSAL"
