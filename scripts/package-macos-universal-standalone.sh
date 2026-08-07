#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${APP_PATH:-$ROOT/build/Classic Player Universal.app}"
OUTPUT="${OUTPUT_DIR:-$ROOT/../../outputs/installers}"
PACKAGE_NAME="${PACKAGE_NAME:-Classic-Player-1.3.2-macOS-Universal-Low-Latency.pkg}"
WORK="$(mktemp -d "$ROOT/build/package-universal.XXXXXX")"
STAGE="$WORK/root"
PACKAGES="$WORK/packages"

cleanup() {
  rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$STAGE/Applications" "$PACKAGES" "$OUTPUT"
ditto "$APP" "$STAGE/Applications/Classic Player.app"

if [[ -n "${APPLE_SIGN_IDENTITY:-}" ]]; then
  codesign --force --deep --options runtime --timestamp \
    --sign "$APPLE_SIGN_IDENTITY" "$STAGE/Applications/Classic Player.app"
fi

pkgbuild --root "$STAGE" \
  --identifier com.classickeys.classicplayer.standalone \
  --version 1.3.2 \
  --install-location / \
  --component-plist "$ROOT/installer/macos/components-standalone.plist" \
  "$PACKAGES/ClassicPlayer-standalone.pkg"

productbuild \
  --distribution "$ROOT/installer/macos/distribution-standalone.xml" \
  --package-path "$PACKAGES" \
  "$OUTPUT/$PACKAGE_NAME"

if [[ -n "${APPLE_INSTALLER_IDENTITY:-}" ]]; then
  productsign --sign "$APPLE_INSTALLER_IDENTITY" \
    "$OUTPUT/$PACKAGE_NAME" \
    "$OUTPUT/${PACKAGE_NAME%.pkg}-signed.pkg"
fi

pkgutil --check-signature "$OUTPUT/$PACKAGE_NAME" || true
echo "Instalador universal criado em: $OUTPUT/$PACKAGE_NAME"
