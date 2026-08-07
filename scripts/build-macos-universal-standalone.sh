#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/Documents/Codex/vcpkg}"
CMAKE="${CMAKE:-/Applications/CMake.app/Contents/bin/cmake}"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
DEPS="$ROOT/build/vcpkg-universal"

export PATH="/opt/homebrew/bin:/Applications/CMake.app/Contents/bin:$PATH"
JUCE_ARGS=(-DCLASSIC_PLAYER_FETCH_JUCE=ON)
if [[ -n "${JUCE_DIR:-}" ]]; then
  JUCE_ARGS=(-DJUCE_DIR="$JUCE_DIR" -DCLASSIC_PLAYER_FETCH_JUCE=OFF)
fi

install_dependencies() {
  local arch="$1"
  local triplet="$2"
  local arch_deps="$DEPS/$arch"

  pushd "$ROOT" >/dev/null
  "$VCPKG_ROOT/vcpkg" install \
    --triplet "$triplet" \
    --overlay-triplets="$ROOT/triplets" \
    --host-triplet=arm64-osx \
    --x-install-root="$arch_deps/installed" \
    --x-buildtrees-root="$arch_deps/buildtrees" \
    --x-packages-root="$arch_deps/packages" \
    --downloads-root="$VCPKG_ROOT/downloads" \
    --binarysource=clear
  popd >/dev/null
}

install_dependencies arm64 arm64-osx-classic
install_dependencies x86_64 x64-osx-classic

configure_and_build() {
  local arch="$1"
  local triplet="$2"
  local build="$ROOT/build/standalone-$arch"
  local arch_deps="$DEPS/$arch"
  "$CMAKE" -S "$ROOT" -B "$build" -G "Unix Makefiles" \
    "${JUCE_ARGS[@]}" \
    -DCLASSIC_PLAYER_STANDALONE_ONLY=ON \
    -DCMAKE_PREFIX_PATH="$arch_deps/installed/$triplet" \
    -DCMAKE_C_COMPILER=/usr/bin/clang \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_SYSROOT="$SDK" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DCMAKE_BUILD_TYPE=Release
  "$CMAKE" --build "$build" --target ClassicPlayerApp --parallel 4
}

configure_and_build arm64 arm64-osx-classic
configure_and_build x86_64 x64-osx-classic

ARM_APP="$ROOT/build/standalone-arm64/ClassicPlayerApp_artefacts/Release/Classic Player.app"
X64_APP="$ROOT/build/standalone-x86_64/ClassicPlayerApp_artefacts/Release/Classic Player.app"
UNIVERSAL="$ROOT/build/Classic Player Universal.app"

rm -rf "$UNIVERSAL"
ditto "$ARM_APP" "$UNIVERSAL"
lipo -create \
  "$ARM_APP/Contents/MacOS/Classic Player" \
  "$X64_APP/Contents/MacOS/Classic Player" \
  -output "$UNIVERSAL/Contents/MacOS/Classic Player"
xattr -dr com.apple.quarantine "$UNIVERSAL" 2>/dev/null || true
codesign --force --deep --sign - "$UNIVERSAL"

file "$UNIVERSAL/Contents/MacOS/Classic Player"
codesign --verify --deep --strict --verbose=2 "$UNIVERSAL"
echo "Aplicativo universal criado em: $UNIVERSAL"
