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
    -DCLASSIC_PLAYER_STANDALONE_ONLY=OFF \
    -DCLASSIC_PLAYER_BUILD_WEBUI_APP=OFF \
    -DCLASSIC_PLAYER_BUILD_TESTS=ON \
    -DCMAKE_PREFIX_PATH="$prefix" \
    -DCMAKE_C_COMPILER=/usr/bin/clang \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_SYSROOT="$SDK" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment" \
    -DCMAKE_BUILD_TYPE=Release
  local -a targets=(
    ClassicPlayer_Standalone ClassicPlayer_VST3 ClassicPlayer_AU
    ClassicPlayerVst3HostSmokeTest ClassicPlayerChordDetectorTest
    ClassicPlayerUpdateCheckerTest ClassicPlayerHammondTest
    ClassicPlayerAudioRegressionTest ClassicPlayerSampleEngineTest
  )
  if [[ "$arch" == "arm64" ]]; then
    targets+=(ClassicPlayerAuHostSmokeTest)
  fi
  "$CMAKE" --build "$build" --target "${targets[@]}" --parallel 4
  for test_name in ClassicPlayerChordDetectorTest ClassicPlayerUpdateCheckerTest ClassicPlayerAudioRegressionTest ClassicPlayerSampleEngineTest; do
    local test_executable="$build/${test_name}_artefacts/Release/$test_name"
    if [[ "$arch" == "x86_64" ]]; then
      arch -x86_64 "$test_executable"
    else
      "$test_executable"
    fi
  done
  local ui_test="$build/ClassicPlayerHammondTest_artefacts/Release/ClassicPlayerHammondTest"
  for skin_option in --performance-effects --silver-ui --dark-steel-ui --responsive-ui --live-layout; do
    if [[ "$arch" == "x86_64" ]]; then
      arch -x86_64 "$ui_test" "$skin_option"
    else
      "$ui_test" "$skin_option"
    fi
  done
  local vst3_host_test="$build/ClassicPlayerVst3HostSmokeTest_artefacts/Release/ClassicPlayerVst3HostSmokeTest"
  local vst3_bundle="$build/ClassicPlayer_artefacts/Release/VST3/Classic Player.vst3"
  if [[ "$arch" == "x86_64" ]]; then
    arch -x86_64 "$vst3_host_test" "$vst3_bundle"
  else
    "$vst3_host_test" "$vst3_bundle"
  fi
}

install_dependencies arm64 arm64-osx-classic
install_dependencies x86_64 x64-osx-classic
configure_and_build arm64 arm64-osx-classic 11.0
configure_and_build x86_64 x64-osx-classic 10.13

find_standalone_app() {
  local build_dir="$1"
  find "$build_dir" -type d \
    \( -path "*/ClassicPlayer_artefacts/Release/Standalone/Classic Player.app" \
       -o -path "*ClassicPlayerApp_artefacts*/Classic Player.app" \) -print -quit
}

ARM_APP="$(find_standalone_app "$ROOT/build/macos-compat-arm64")"
X64_APP="$(find_standalone_app "$ROOT/build/macos-compat-x86_64")"
UNIVERSAL="$ROOT/build/Classic Player 2.0 macOS Universal.app"
EXECUTABLE="$UNIVERSAL/Contents/MacOS/ClassicPlayer"

if [[ -z "$ARM_APP" || ! -d "$ARM_APP" || -z "$X64_APP" || ! -d "$X64_APP" ]]; then
  echo "Erro: Classic Player.app não encontrado após o build." >&2
  find "$ROOT/build" -type d -name "Classic Player.app" -print >&2 || true
  exit 1
fi

rm -rf "$UNIVERSAL"
ditto "$ARM_APP" "$UNIVERSAL"
lipo -create \
  "$ARM_APP/Contents/MacOS/Classic Player" \
  "$X64_APP/Contents/MacOS/Classic Player" \
  -output "$EXECUTABLE"
rm "$UNIVERSAL/Contents/MacOS/Classic Player"

/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable ClassicPlayer" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier com.classickeys.classicplayer.macos161" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString 3.0.2" "$UNIVERSAL/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion 3.0.2" "$UNIVERSAL/Contents/Info.plist"
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

create_universal_plugin() {
  local format="$1"
  local extension="$2"
  local executable="$3"
  local arm_bundle="$ROOT/build/macos-compat-arm64/ClassicPlayer_artefacts/Release/$format/Classic Player.$extension"
  local x64_bundle="$ROOT/build/macos-compat-x86_64/ClassicPlayer_artefacts/Release/$format/Classic Player.$extension"
  local universal_bundle="$ROOT/build/Classic Player 2.0 macOS Universal.$extension"
  local universal_binary="$universal_bundle/Contents/MacOS/$executable"

  if [[ ! -d "$arm_bundle" || ! -d "$x64_bundle" ]]; then
    echo "Erro: bundle $extension não foi gerado para as duas arquiteturas." >&2
    exit 1
  fi
  if [[ ! -f "$arm_bundle/Contents/MacOS/$executable" || ! -f "$x64_bundle/Contents/MacOS/$executable" ]]; then
    echo "Erro: executável do bundle $extension ausente." >&2
    exit 1
  fi

  rm -rf "$universal_bundle"
  ditto "$arm_bundle" "$universal_bundle"
  lipo -create \
    "$arm_bundle/Contents/MacOS/$executable" \
    "$x64_bundle/Contents/MacOS/$executable" \
    -output "$universal_binary"
  xattr -cr "$universal_bundle"
  codesign --force --deep --sign - "$universal_bundle"
  codesign --verify --deep --strict --verbose=2 "$universal_bundle"
  local architectures
  architectures="$(lipo -archs "$universal_binary")"
  [[ "$architectures" == *arm64* && "$architectures" == *x86_64* ]] || {
    echo "Erro: $extension não é universal ($architectures)." >&2
    exit 1
  }
}

create_universal_plugin VST3 vst3 "Classic Player"
create_universal_plugin AU component "Classic Player"

# AudioComponentFindNext only sees Audio Units registered in standard plug-in
# locations. Validate the assembled universal bundle there, not from the build
# tree (which can produce a false "not discovered" failure).
AU_INSTALL_DIR="$HOME/Library/Audio/Plug-Ins/Components"
AU_INSTALLED="$AU_INSTALL_DIR/Classic Player.component"
mkdir -p "$AU_INSTALL_DIR"
rm -rf "$AU_INSTALLED"
ditto "$ROOT/build/Classic Player 2.0 macOS Universal.component" "$AU_INSTALLED"
xattr -cr "$AU_INSTALLED"
codesign --verify --deep --strict --verbose=2 "$AU_INSTALLED"

# AudioComponentRegistrar caches Audio Units. A newly copied bundle may pass
# codesign checks but remain invisible to auval and hosts until the registrar
# rescans the plug-in folders. Restart only the runner's registrar, then make
# auval enumerate components before validating/instantiating this AU.
killall -9 AudioComponentRegistrar 2>/dev/null || true
sleep 2
AU_ENUMERATION_LOG="$ROOT/build/auval-components.log"
AU_DISCOVERED=false
for attempt in {1..15}; do
  /usr/bin/auval -a >"$AU_ENUMERATION_LOG" 2>&1 || true
  if grep -Eq 'aumu[[:space:]]+ClPl[[:space:]]+ClKy' "$AU_ENUMERATION_LOG"; then
    AU_DISCOVERED=true
    break
  fi
  sleep 1
done
if [[ "$AU_DISCOVERED" != true ]]; then
  echo "Erro: o Audio Unit universal não foi registrado pelo macOS após a nova varredura." >&2
  tail -n 80 "$AU_ENUMERATION_LOG" >&2 || true
  exit 1
fi
/usr/bin/auval -v aumu ClPl ClKy

AU_HOST_TEST="$ROOT/build/macos-compat-arm64/ClassicPlayerAuHostSmokeTest_artefacts/Release/ClassicPlayerAuHostSmokeTest"
"$AU_HOST_TEST" "$AU_INSTALLED"

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
