#!/usr/bin/env bash
# Turns the emulator's accelerometer upside down and back with the system rotation
# lock on: the app must follow between landscape and reverse landscape.
set -uo pipefail
app=com.classickeys.classicplayer
gradle --no-daemon -q -p android -PtestAbis=x86_64 :app:installDebug || exit 1
adb shell settings put system accelerometer_rotation 0
adb shell input keyevent KEYCODE_WAKEUP
adb shell wm dismiss-keyguard
adb shell am start -W -n "$app/.MainActivity" || exit 1
rotation() { adb shell dumpsys input | grep -m1 'SurfaceOrientation' | tr -dc '0-9'; }
turn() {
  adb emu sensor set acceleration "$1" >/dev/null
  sleep 6
  now=$(rotation)
  echo "acceleration $1 -> rotation $now (expected $2)"
  [ "$now" = "$2" ]
}
failed=0
turn 0:9.81:0 0 || failed=1
turn 0:-9.81:0 2 || failed=1
turn 0:9.81:0 0 || failed=1
adb shell pidof "$app" >/dev/null || { echo "app is not running after turning"; failed=1; }
exit "$failed"
