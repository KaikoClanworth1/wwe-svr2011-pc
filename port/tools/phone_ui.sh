#!/usr/bin/env bash
# Drives the Android launcher on the phone's cover screen (display 0) over adb:
#   phone_ui.sh shot <name>          screenshot -> port/runs/<name>.png
#   phone_ui.sh tap "<text>"         taps the first element with that text
#   phone_ui.sh swipe up|down        scrolls the page
#   phone_ui.sh start                (re)starts the launcher
# Element positions come from uiautomator's dump of the screen.
set -e
ADB="/d/Android/sdk/platform-tools/adb.exe"
RUNS="$(cd "$(dirname "$0")/.." && pwd)/runs"
export MSYS_NO_PATHCONV=1
display_id() {  # the SurfaceFlinger id of display 0 (the cover screen)
  "$ADB" shell dumpsys SurfaceFlinger --display-id | grep -o '^Display [0-9]*' | sed -n 2p | cut -d' ' -f2
}
case "$1" in
  shot)
    "$ADB" exec-out screencap -p -d "$(display_id)" > "$RUNS/$2.png" ;;
  tap)
    "$ADB" shell uiautomator dump --display 0 /sdcard/ui.xml >/dev/null 2>&1 || "$ADB" shell uiautomator dump /sdcard/ui.xml >/dev/null
    b=$("$ADB" shell cat /sdcard/ui.xml | tr '>' '\n' | grep -F "text=\"$2" | head -1 | grep -o 'bounds="[^"]*"' | grep -o '[0-9]*' | tr '\n' ' ')
    want="$2"
    set -- $b
    [ -n "$4" ] || { echo "not found: $want" >&2; exit 1; }
    "$ADB" shell input -d 0 tap $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 ))
    sleep 1 ;;
  swipe)
    if [ "$2" = up ]; then "$ADB" shell input -d 0 swipe 540 2000 540 800 300; else "$ADB" shell input -d 0 swipe 540 800 540 2000 300; fi
    sleep 1 ;;
  start)
    "$ADB" shell am force-stop io.github.kaikoclanworth1.svr2011
    "$ADB" shell am start -n io.github.kaikoclanworth1.svr2011/.InstallActivity >/dev/null
    sleep 3 ;;
esac
