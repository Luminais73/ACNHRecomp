#!/bin/bash
# Double-click in Finder (or run it) to play on a Mac: rebuilds what changed in the runtime (seconds; nothing when
# up to date), then starts the game with the settings of private/acnh/config.ini. A copy on the desktop works too:
# it finds the repository from the path below, written by tools/install_mac_shortcut.sh.
REPO="$(cd "$(dirname "$0")" && pwd)"
[ -f "$REPO/tools/acnh_mac.sh" ] || REPO="$(cat "$HOME/.acnhrecomp_repo" 2>/dev/null)"
cd "$REPO" || { echo "ACNH Recomp folder not found ($REPO)"; read -r -p "Press Enter to close"; exit 1; }
BUILD=private/acnh/build

# Not set up yet: the launcher (built here the first time, a minute) asks for the game folder, recompiles the game
# from it and starts it.
if [ ! -x "$BUILD/acnh" ]; then
  LB=private/launcher-build
  if [ ! -x "$LB/acnhrecomp_launcher" ]; then
    if ! command -v cmake >/dev/null || ! command -v ninja >/dev/null || ! brew --prefix sdl3 >/dev/null 2>&1; then
      echo "To open the launcher this needs: brew install cmake ninja sdl3"
      read -r -p "Press Enter to close"
      exit 1
    fi
    echo "Preparing the launcher (once)..."
    mkdir -p private
    { cmake -S . -B "$LB" -G Ninja -DNX_GAME=acnh -DNX_GEN_DIR="$PWD/$LB/no-gen" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$(brew --prefix)" && cmake --build "$LB" --target acnhrecomp_launcher; } > "$LB.log" 2>&1 ||
      { tail -20 "$LB.log"; read -r -p "The launcher did not build (full output: $LB.log). Press Enter to close"; exit 1; }
  fi
  exec "$LB/acnhrecomp_launcher"
fi

if command -v ninja >/dev/null && [ -f "$BUILD/build.ninja" ]; then
  # Another build in progress (tools and agents share private/build.lock): wait for it.
  # A lock older than 2 hours is a leftover of a build that was killed.
  [ -n "$(find private/build.lock -maxdepth 0 -mmin +120 2>/dev/null)" ] && rmdir private/build.lock 2>/dev/null
  until mkdir private/build.lock 2>/dev/null; do echo "Waiting for another build to finish..."; sleep 5; done
  trap 'rmdir private/build.lock 2>/dev/null' EXIT
  echo "Updating the build..."
  if ! ninja -C "$BUILD" acnh > private/acnh/logs/play_build.log 2>&1; then
    tail -20 private/acnh/logs/play_build.log
    echo
    echo "The build failed (full output: private/acnh/logs/play_build.log)."
    read -r -p "Press Enter to close"
    exit 1
  fi
  rmdir private/build.lock 2>/dev/null
  trap - EXIT
fi

# The pt-BR fan translation, when its zip is in Downloads and not installed yet (Settings > Game > language
# Português (Brasil) then uses it). It stays on this computer: private/ is never published.
if [ ! -d private/acnh/mods/ptbr/romfs ]; then
  ptbr=$(ls -t "$HOME"/Downloads/acnh_ptbr*.zip 2>/dev/null | head -1)
  if [ -n "$ptbr" ] && mkdir -p private/acnh/mods/ptbr &&
     unzip -qo "$ptbr" 'romfs/*' -d private/acnh/mods/ptbr 2>/dev/null; then
    # As the setup installs it: archives as the game reads them, long dialogue lines re-wrapped.
    python3 -c "import sys; sys.path.insert(0, 'tools'); import setup_acnh as s
s.fix_archives('private/acnh/mods/ptbr/romfs')
s.rewrap_messages('private/acnh/extract/romfs', 'private/acnh/mods/ptbr/romfs')"
    echo "pt-BR translation installed ($ptbr)"
  fi
fi

echo "Starting Animal Crossing: New Horizons..."
tools/acnh_mac.sh
status=$?
if [ $status -ne 0 ]; then
  echo
  echo "The game closed with an error ($status). Settings > About > Create problem report, or run:"
  echo "  NX_REPORT=1 $BUILD/acnh"
  read -r -p "Press Enter to close"
fi
