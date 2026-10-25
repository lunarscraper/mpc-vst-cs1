#!/usr/bin/env bash
# Offline x86 test (ASan/UBSan) of cs1_vst.cpp with its eight engines, then a CPU benchmark (-O3, as on the device):
# run after build.sh (needs build/params.h, build/popup.h). Prints PASSED/FAILED; exit code follows.
#   CS1_NO_DOCKER=1 bash vst/test.sh   the same with the computer's own gcc
set -euo pipefail
cd "$(dirname "$0")"
read -r -d '' RUN <<'EOF' || true
  set -euo pipefail
  mkdir -p build/x86
  SAN="-fsanitize=address,undefined -fno-sanitize=shift-base"   # (the firmware shifts negative values on purpose)
  E="-std=gnu11 -fPIC -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Ics1/gen -I."
  for n in 0 1 2 3 4 5 6 7; do
    gcc -O0 -g $SAN $E -DCS1_SLOT=$n -c cs1_engine.c -o build/x86/san$n.o
    gcc -O3 $E -fvisibility=hidden -DCS1_SLOT=$n -c cs1_engine.c -o build/x86/o3_$n.o
  done
  g++ -O0 -g $SAN -std=c++17 -fPIC -shared -Ibuild -I. -o build/x86/cs1-x86.so cs1_vst.cpp build/x86/san*.o -lpthread
  g++ -O0 -g $SAN -std=c++17 -o build/x86/host_test host_test.cpp -ldl
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ./build/x86/host_test ./build/x86/cs1-x86.so
  g++ -O3 -ffast-math -std=c++17 -fPIC -shared -fvisibility=hidden -Ibuild -I. -o build/x86/cs1-o3.so cs1_vst.cpp build/x86/o3_*.o -lpthread
  g++ -O2 -std=c++17 -o build/x86/bench host_test.cpp -ldl
  ./build/x86/bench ./build/x86/cs1-o3.so bench
EOF
if [ "${CS1_NO_DOCKER:-}" = 1 ]; then bash -c "$RUN"
else docker run --rm -v "$PWD":/b -w /b gcc:12 bash -c "$RUN"; fi
