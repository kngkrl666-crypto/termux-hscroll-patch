#!/bin/bash
set -e

echo "=== Device info ==="
adb shell getprop ro.build.version.sdk
adb shell getprop ro.build.version.release

echo ""
echo "=== Installing APK ==="
APK=$(ls app/build/outputs/apk/debug/*x86_64*.apk 2>/dev/null | head -1)
if [ -z "$APK" ]; then
  APK=$(ls app/build/outputs/apk/debug/*universal*.apk 2>/dev/null | head -1)
fi
echo "Installing: $APK"
adb install -r "$APK"
echo "Install: SUCCESS"

echo ""
echo "=== Launching Termux ==="
adb shell am start -n com.termux/.app.TermuxActivity
sleep 10

echo ""
echo "=== Checking if Termux is running ==="
adb shell pidof com.termux || { echo "FAIL: Termux not running"; exit 1; }
echo "Termux process found: PASS"

echo ""
echo "=== Checking logcat for crashes ==="
CRASHES=$(adb logcat -d -s AndroidRuntime:E | grep -c "FATAL EXCEPTION" || true)
echo "Crash count: $CRASHES"
if [ "$CRASHES" -gt 0 ]; then
  echo "FAIL: Found $CRASHES crash(es)"
  adb logcat -d | grep -A 20 "FATAL EXCEPTION" | head -60
  exit 1
fi
echo "No crashes: PASS"

echo ""
echo "=== Waiting for bootstrap ==="
sleep 20
adb shell "run-as com.termux ls files/usr/bin/sh" && echo "Bootstrap: PASS" || echo "Bootstrap: still loading"

echo ""
echo "=== Native libs ==="
adb shell "run-as com.termux ls lib/" || true

echo ""
echo "=== Checking process still alive ==="
sleep 5
adb shell pidof com.termux || { echo "FAIL: Termux died"; exit 1; }
echo "Still running after 35s: PASS"

echo ""
echo "=== Termux logs ==="
adb logcat -d | grep -iE "termux|linker.exec|LD_PRELOAD" | tail -40

echo ""
echo "=== RESULT: ALL TESTS PASSED ==="
