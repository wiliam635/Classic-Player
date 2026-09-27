#!/usr/bin/env bash
# The emulator action launches script lines in separate shells. Keep the test
# exit status and artifact collection in one process, including on test failure.
set -uo pipefail
mkdir -p android-test-artifacts
adb logcat -c
result=0
gradle --no-daemon -p android -PtestAbis=x86_64 :app:connectedDebugAndroidTest || result=$?
adb logcat -d > android-test-artifacts/logcat.txt || true
adb pull /sdcard/Android/data/com.classickeys.classicplayer/files/audio-regression android-test-artifacts/ || true
exit "$result"
