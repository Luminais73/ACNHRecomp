#!/bin/bash
# A repeatable 60 fps benchmark on a visible, muted window: boots a save at 14:00, skips the arrival and Tom Nook's
# announcement, plays a walk script, then prints the frame rate per 60 frames, the display pacing and the hitches
# over the walk only.
# usage: tools/play/bench_walk.sh <root name> <save> "<walk script>" [NX_...=value ...]
#   COOL=<s> (default 90): seconds to wait first; the fanless M1 Air slows down over back-to-back runs.
# e.g. tools/play/bench_walk.sh north user_1026 "walk up 600 run; walk right 300 run; walk up 400 run; walk left 600 run; walk down 900 run"
cd "$(dirname "$0")/../.." || exit 1
name=$1 save=$2 walk=$3; shift 3
sleep "${COOL:-90}"
R=private/play/roots/$name; [ -n "$name" ] && rm -rf "${R:?}"; mkdir -p "$R"
sed "s/^fullscreen = .*/fullscreen = 0/; s/^frame_rate = .*/frame_rate = 60/; s/^resolution = .*/resolution = 1080/; s/^aspect = .*/aspect = 16:9/" private/acnh/config.ini > "$R/config.ini"
echo 1920x1080 > "$R/window_size"
W="$R/visible.sh"
printf '#!/bin/bash\nunset NX_HEADLESS\nexec "%s/private/acnh/build/acnh" "$@"\n' "$PWD" > "$W"; chmod +x "$W"
tools/play/run.sh "$name" --save "$save" --live --minutes 25 --bin "$W" NX_TIME_OFFSET=$(( $(date -j -f '%H:%M:%S' 14:00:00 +%s) - $(date +%s) )) "$@" >/dev/null 2>&1
sleep 75
L() { python3 tools/play/live.py "$name" "$@"; }
L send "press A 1 30; wait 1200; press A 1 30; wait 1800" >/dev/null
for i in $(seq 1 45); do L send "press A 1 8; wait 50" >/dev/null; done
L send "wait 300" >/dev/null
L shot start --full >/dev/null
N0=$(wc -l < "$R/nx.log")
L send "$walk" >/dev/null
N1=$(wc -l < "$R/nx.log")
L shot end --full >/dev/null
L alive || echo "the game died"
tools/play/run.sh stop "$name" >/dev/null 2>&1
python3 - "$R/nx.log" "$N0" "$N1" <<'PY'
import re, sys
lines = open(sys.argv[1], errors="replace").read().splitlines()
seg = lines[int(sys.argv[2]):int(sys.argv[3])]
fps = [float(m.group(1)) for l in seg if (m := re.match(r"nvn/vk: per frame over 60: ([\d.]+) fps", l))]
hitch = [l for l in seg if l.startswith("hitch:")]
print(f"windows {len(fps)}  fps min {min(fps) if fps else 0:.1f} avg {sum(fps) / max(1, len(fps)):.1f}  hitches {len(hitch)}  "
      f"crashes {sum('CRASH' in l for l in lines)}")
print("fps:", " ".join(f"{f:.0f}" for f in fps))
for l in [l for l in seg if l.startswith("display:")][-3:]: print(l[:110])
for l in hitch[:12]: print(l[:230])
PY
