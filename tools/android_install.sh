#!/bin/bash
# The whole Android setup in one go (docs/ANDROID.md), once the game is set up on this computer (tools/setup_acnh.py):
# checks the Android SDK, the JDK and the phone, gets Turnip (built here, or the prebuilt one from this repository's
# releases), builds and installs the APK, copies the game's data to the phone, and with --save the computer's save.
#
# usage: tools/android_install.sh [--save] [--skip-data]
cd "$(dirname "$0")/.." || exit 1
set -e
SAVE=0
DATA=1
for a in "$@"; do
  case $a in
    --save) SAVE=1 ;;
    --skip-data) DATA=0 ;;
    *) echo "unknown option $a"; exit 1 ;;
  esac
done
say() { printf '\n== %s\n' "$*"; }
fail() { printf '\n!! %s\n' "$*"; exit 1; }

if [ "$(uname)" = Darwin ]; then DEF_SDK=$HOME/Library/Android/sdk; else DEF_SDK=$HOME/Android/Sdk; fi
SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$DEF_SDK}}
say "checks"
[ -f private/acnh/analysis/main.bin ] && [ -d private/acnh/gen/main ] ||
  fail "the game is not set up on this computer yet: python3 tools/setup_acnh.py --game <your game folder>"
for p in "ndk/28.2.13676358" "build-tools/35.0.0" "platforms/android-35"; do
  [ -d "$SDK/$p" ] || fail "Android SDK: $p missing in $SDK (sdkmanager \"${p//\//;}\", or Android Studio's SDK Manager)"
done
command -v adb >/dev/null || export PATH=$SDK/platform-tools:$PATH
command -v adb >/dev/null || fail "adb not found: sdkmanager \"platform-tools\""
if ! command -v java >/dev/null && [ ! -d /opt/homebrew/opt/openjdk@17 ] && [ -z "${JAVA_HOME:-}" ]; then
  fail "a JDK 17 is needed (macOS: brew install openjdk@17; Linux: your distribution's openjdk-17-jdk)"
fi
n=$(adb devices | grep -c "device$" || true)
[ "$n" = 1 ] || fail "connect one phone with USB debugging on (adb devices lists $n)"
echo "SDK $SDK, phone $(adb shell getprop ro.product.model | tr -d '\r')"

TURNIP=private/third_party/mesa/build-android/src/freedreno/vulkan/libvulkan_freedreno.so
if [ ! -f "$TURNIP" ]; then
  say "Turnip (the Adreno Vulkan driver the app carries)"
  mkdir -p "$(dirname "$TURNIP")"
  URL=https://github.com/Nycolazs/ACNHRecomp/releases/latest/download/libvulkan_freedreno.so
  if curl -fL --retry 3 -o "$TURNIP.tmp" "$URL"; then
    mv "$TURNIP.tmp" "$TURNIP"
  else
    rm -f "$TURNIP.tmp"
    echo "no prebuilt Turnip downloaded: building it from Mesa's sources"
    tools/build_turnip.sh
  fi
fi

say "the APK (the first build compiles the whole game: 30-60 minutes)"
tools/build_android.sh --install

if [ $DATA = 1 ]; then
  say "the game's data to the phone (~10 GB; can be stopped and run again)"
  tools/push_android.sh
fi

if [ $SAVE = 1 ]; then
  say "your save"
  [ -d private/acnh/user/device_save ] || fail "no save in private/acnh/user/device_save"
  card=$(adb shell 'for d in /storage/*; do case $d in */emulated|*/self) ;; *) echo $d; break;; esac; done' | tr -d '\r')
  D=${card:-/sdcard}/Android/data/io.github.acnhrecomp/files
  adb shell am force-stop io.github.acnhrecomp
  if adb shell "[ -d $D/user/device_save ]"; then
    stamp=$(date +%Y%m%d-%H%M%S)
    adb shell "mv $D/user/device_save $D/user/device_save.before-$stamp"
    echo "the phone's own save kept as user/device_save.before-$stamp"
  fi
  adb shell mkdir -p "$D/user"
  adb push private/acnh/user/device_save "$D/user/" | tail -1
  [ -f private/acnh/users.txt ] && adb push private/acnh/users.txt "$D/" | tail -1
fi

say "done: open ACNH Recomp on the phone"
