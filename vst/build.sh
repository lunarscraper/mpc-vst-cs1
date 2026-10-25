#!/usr/bin/env bash
# Build CS-1 as a VST2 instrument for the MPC OS plugin host (armhf).
#   vst/build/cs1.so                -> /sdcard/vst/ on the device
#   vst/build/pluginlist-entry.xml  the <PLUGIN> line for MPC.settings' pluginList-arm
#   vst/build/skin/                 -> /sdcard/Synths/ on the device
# cs1_vst.cpp + eight copies of the engine cs1_engine.c (the firmware in cs1/, one copy per instance), no
# dependencies. tools/gen_cs1.py writes module.json, layout.conf and cs1_keys.h (committed); vst.json/module.json
# only feed tools/gen_vst.py for params.h + the skin. Offline test + benchmark: test.sh.
#   CS1_SKIN_ONLY=1 bash vst/build.sh   params.h + the skin, no plugin (no docker needed)
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst}" && pwd)"
U="$(id -u):$(id -g)"
mkdir -p build

# 0. the parameter table and the layout (same output as the committed files)
python3 tools/gen_cs1.py

# 1. skin artwork renderer (host binary; the renderer vendored in mpc-vst-plugins)
if command -v gcc >/dev/null; then
  gcc -O2 -I"$MPC_VST/tools/vendor/force-shadow/tools" -o build/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
else
  docker run --rm -u "$U" -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w gcc:12 \
    gcc -O2 -I/mv/tools/vendor/force-shadow/tools -o build/shadow_art /mv/tools/shadow_art.c -lm
fi

# 2. params.h, skin, pluginlist-entry.xml (needs Pillow)
if python3 -c "import PIL" 2>/dev/null; then
  python3 "$MPC_VST/tools/gen_vst.py" vst.json
else
  docker run --rm -u "$U" -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w python:3.11-slim sh -c \
    "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 /mv/tools/gen_vst.py vst.json"
fi
cp "$MPC_VST/wrapper/popup.h" build/   # popup open-flag handling shared with mpc-vst's own wrapper
[ "${CS1_SKIN_ONLY:-}" = 1 ] && { echo "skin only"; exit 0; }

# 3. the plugin (armhf, glibc 2.36 so it loads on the device's 2.39). First-generation MPC OS
#    units are Rockchip RK3288 (Cortex-A17, NEON/VFPv4), hence the tuning flags. The engine is the
#    firmware's integer code (C, gnu11), compiled once per slot; the shift-of-a-negative warnings
#    are the firmware's own idiom (arithmetic shifts, as GCC defines them).
docker run --rm --platform linux/arm/v7 -v "$PWD":/b -w /b arm32v7/gcc:12 bash -euxc '
  ARM="-mtune=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard"
  mkdir -p build/arm
  for n in 0 1 2 3 4 5 6 7; do
    gcc -O3 -std=gnu11 -fPIC -fvisibility=hidden $ARM -Wall -Wno-unused-function -Wno-unused-variable \
        -Wno-unused-but-set-variable -Ics1/gen -I. -DCS1_SLOT=$n -c cs1_engine.c -o build/arm/engine$n.o
  done
  g++ -O3 -ffast-math -fPIC -fvisibility=hidden -std=c++17 $ARM \
      -Wall -Wextra -Wno-unused-parameter -Ibuild -I. -o build/cs1.so -shared cs1_vst.cpp build/arm/engine*.o \
      -static-libstdc++ -static-libgcc -lpthread -lm -Wl,--no-undefined
  strip build/cs1.so
  echo "-- needed --"; readelf -d build/cs1.so | grep NEEDED
  echo "-- highest glibc (device has 2.39) --"; readelf -V build/cs1.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/cs1.so
