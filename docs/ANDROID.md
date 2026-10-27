# Android

ACNHRecomp runs on Android phones with a Snapdragon (Adreno 6xx or newer), through Turnip (Mesa's Vulkan driver
for Adreno, carried inside the app). It was brought up and tuned on a Galaxy S20 FE (Snapdragon 865, Adreno 650):
on the island the game holds its own 30 frames a second and the phone shows 60 with the built-in frame generation,
at 900 rendered rows scaled to the 2400x1080 screen (the full 20:9 screen, under the camera cutout).

The app is built on your computer from **your own copy of the game**, like the desktop versions: nothing of the game
is in this repository or in its releases, and the APK you build must not be shared.

## What you need

- A computer with the game already set up by `tools/setup_acnh.py` (README, "Installation"): macOS (Apple
  Silicon) or Linux x86-64. Its `private/acnh` folder (generated code, `spirv`, `fonts`, the RomFS link) is what the
  phone build uses.
- The Android SDK with `ndk;28.2.13676358`, `build-tools;35.0.0` and `platforms;android-35` (Android Studio's SDK
  Manager, or `sdkmanager "ndk;28.2.13676358" "build-tools;35.0.0" "platforms;android-35"`), and a JDK 17
  (`brew install openjdk@17` on macOS). The SDK is looked for in `ANDROID_SDK_ROOT`, else `~/Library/Android/sdk`
  (macOS) or `~/Android/Sdk` (Linux).
- Turnip: `tools/build_turnip.sh` builds it from Mesa's sources (needs Homebrew's `bison`, `flex` and Python 3.10+),
  or take `libvulkan_freedreno.so` from a release of this repository and put it in
  `private/third_party/mesa/build-android/src/freedreno/vulkan/`.
- The phone: Android 10 or newer, USB debugging on (Settings > About > tap the build number 7 times, then
  Developer options > USB debugging), connected by USB, `adb devices` listing it. About 11 GB free on the memory card
  or the phone's storage.

## Build, install, copy the data

All in one (checks what is missing and says how to get it):

```sh
tools/android_install.sh --save      # --save: also your computer's save (the phone's own is kept beside it)
```

Or step by step:

```sh
tools/build_android.sh --install     # the APK (the first build compiles the whole game: 30-60 min), installed by adb
tools/push_android.sh                # the game's data to the phone (~10 GB: romfs.pak, spirv.pak, fonts, mods, ...)
```

`push_android.sh` puts everything in the app's folder on the memory card when there is one
(`Android/data/io.github.acnhrecomp/files`), else in the phone's own storage. It can be interrupted and run again:
it resumes. Run it again after updating the repository and rebuilding (it re-sends only what changed).

The pt-BR translation: install it on the computer as for the desktop version (the setup installs a
`*ptbr*.zip` found in the game folder or `private/translations/`); `push_android.sh` sends `mods/` with it.

### Your save

To continue the island you play on the computer, copy its save to the phone (with the game closed on both):

```sh
D=/storage/<card>/Android/data/io.github.acnhrecomp/files   # adb shell ls /storage to find <card>
adb push private/acnh/user/device_save "$D/user/"
adb push private/acnh/users.txt "$D/"
```

Saves are written to `user/device_save` on the phone; copy it back the same way with `adb pull`.

## Playing

Open **ACNH Recomp** on the phone. The first start takes a minute or two: shaders are prepared once, then kept.

- **Touch controls**: the left half of the screen is the stick; A, B, X, Y at the bottom right; L/ZL, R/ZR in the
  top corners; - and + at the top centre with the settings gear between them; a small d-pad bottom left.
- **A controller** (Bluetooth or USB) hides the touch controls by itself while it is connected. The settings can
  also hide them for good.
- **Settings**: the gear at the top centre (or Minus + Plus on a controller). Language (pt-BR included), aspect,
  resolution, frame generation, frame rate. On a phone they show no keyboard hints.

### Settings that matter on a phone

| Setting | Default on Android | Notes |
|---|---|---|
| Frame generation | Built-in | the game's 30 fps shown at 60; off: 30 |
| Resolution | Auto | the rows the GPU keeps at 30 fps (900 on an Adreno 650), scaled to the screen |
| Aspect | Auto | the phone's own shape (20:9), HUD at the edges |
| Prepare shaders while loading / async shaders | on | new objects can show a few frames late instead of freezing the game |
| Unlock GPU clock (root) | off | rooted Samsung phones only, see below |

### Samsung phones: the GPU clock cap

Samsung's system manager caps the GPU (441 of 587 MHz on the S20 FE) and the CPU, even with the phone cold. The game
then runs at ~25 fps instead of 30. On a **rooted** phone, *Unlock GPU clock (root)* in the settings lifts the GPU cap
while the game runs (the app asks Magisk for root once: allow it). The CPU caps are left alone: the game does not get
faster with them, only warmer. A phone that gets hot (75-80 °C on the S20 FE after minutes of play) throttles by
itself; a cooler helps.

## Measured (Galaxy S20 FE, island, 900 rows, frame generation)

| | Before (2026-10-26) | Now |
|---|---|---|
| Game frames a second | 23-25, frames 36-50 ms apart | 29.7-30.0, 31-36 ms apart |
| Shown | ~40-48 | ~60 |
| Hitches over 10 minutes standing | many | 0 |

Native 1080 rows run at ~26 game frames a second on the Adreno 650 (the GPU is the limit there).

## Troubleshooting

- **Black screen or the app closes at start**: `adb shell cat <data folder>/nx.log | tail -50` and open an issue
  with it. `adb logcat -s acnh` shows the same log live.
- **The app asks for access to all files**: it found no data in its own folders (no `analysis/main.bin`): run
  `tools/push_android.sh` again, or allow the access if you put the data in `Internal storage/Animal Crossing: New
  Horizons` yourself.
- **Settings for testing** go in `config.txt` in the data folder, one `NX_...=value` per line (they win over the
  settings screen). E.g. `NX_RENDER_HEIGHT=1080` for native 1080 rows.
