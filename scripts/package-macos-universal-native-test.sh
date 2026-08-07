#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${APP_PATH:-$ROOT/build/Classic Player 1.6.0 Preview.app}"
OUTPUT="${OUTPUT_DIR:-$ROOT/../../outputs/installers}"
PACKAGE_NAME="${PACKAGE_NAME:-Classic-Player-1.6.0-macOS-Universal.pkg}"
WORK="$(mktemp -d "$ROOT/build/package-native-test.XXXXXX")"
STAGE="$WORK/root"
PACKAGES="$WORK/packages"

cleanup() {
  rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$STAGE/Applications" "$PACKAGES" "$OUTPUT"
ditto --noextattr --noqtn "$APP" "$STAGE/Applications/Classic Player.app"
/usr/bin/xattr -cr "$STAGE/Applications/Classic Player.app"

pkgbuild --root "$STAGE" \
  --identifier com.classickeys.classicplayer.standalone \
  --version 1.6.0 \
  --install-location / \
  --component-plist "$ROOT/installer/macos/components-standalone.plist" \
  "$PACKAGES/ClassicPlayer-standalone.pkg"

productbuild \
  --distribution "$ROOT/installer/macos/distribution-standalone.xml" \
  --package-path "$PACKAGES" \
  "$OUTPUT/$PACKAGE_NAME"

pkgutil --check-signature "$OUTPUT/$PACKAGE_NAME" || true
echo "Instalador nativo universal criado em: $OUTPUT/$PACKAGE_NAME"
