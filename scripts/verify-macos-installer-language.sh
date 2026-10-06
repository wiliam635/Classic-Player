#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PACKAGE="${1:?Provide the macOS distribution package}"
test -f "$PACKAGE"
installer -pkg "$PACKAGE" -target / -showChoicesXML |
  plutil -convert json -o - -- - |
  jq -e '.[0].childItems | map(select(.choiceIdentifier != "app" and .choiceIsSelected == 1)) |
    length == 1 and .[0].choiceIdentifier == "br"' >/dev/null
for language in english spanish; do
  expected="us"
  if [[ "$language" == "spanish" ]]; then expected="es"; fi
  installer -pkg "$PACKAGE" -target / -showChoicesAfterApplyingChangesXML \
    "$ROOT/installer/macos/tests/$language-choice.plist" |
    plutil -convert json -o - -- - |
    jq -e --arg expected "$expected" 'map(select(.choiceAttribute == "selected" and
      .choiceIdentifier != "app" and .attributeSetting == 1)) |
      length == 1 and .[0].choiceIdentifier == $expected' >/dev/null
done
echo "Installer language choices verified: BR / US / ES"
