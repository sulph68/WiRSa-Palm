#!/bin/bash
# verify.sh - the checks to run before a flash, in one go. From anywhere:
#
#   Firmware/tools/verify.sh [--no-build] [PIO_PROJECT_DIR] [PALM_APPS_DIR]
#
#   1. check_wire.py: the frame tables of docs/protocol/Protocol-wire.md against the firmware headers and the Palm sources
#   2. the host tests in test_host/ (g++ only, no board)
#   3. a build with every feature switch off in turn, and the groups that must still build (needs PlatformIO)
#   4. the default build
#
# PIO_PROJECT_DIR is a PlatformIO project whose src is this Firmware/src (a stand-in ini is enough). Without it, or with --no-build, steps 3 and 4 are skipped.
# PIO names the pio command (default: pio, else ~/.platformio/penv/bin/pio). The exit status is 1 if any step failed.
FW="$(cd "$(dirname "$0")/.." && pwd)"
BUILD=1
ARGS=()
for a in "$@"; do [ "$a" = "--no-build" ] && BUILD=0 || ARGS+=("$a"); done
PROJ="${ARGS[0]:-}"
PALM="${ARGS[1]:-}"
PIO="${PIO:-$(command -v pio || echo ~/.platformio/penv/bin/pio)}"
FAILED=0
note() { printf '\n== %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; FAILED=1; }

cd "$FW" || exit 1

note "1. wire tables"
python3 tools/check_wire.py $PALM || fail "check_wire.py"

note "2. host tests"
CXX="g++ -std=c++17 -Wall -Wextra"
T=test_host
run() {   # name, sources...
  local name=$1; shift
  if $CXX "$@" -o "/tmp/verify_$name" > "/tmp/verify_$name.log" 2>&1 && "/tmp/verify_$name" >> "/tmp/verify_$name.log" 2>&1; then echo "ok    $name"; else fail "$name (see /tmp/verify_$name.log)"; fi
}
run autobaud    -Iinclude src/modules/autobaud.cpp $T/autobaud_test.cpp
run vdisk       -Iinclude src/modules/vdisk.cpp $T/vdisk_test.cpp
run mesh_pb     -Iinclude src/modules/mesh_pb.cpp $T/mesh_pb_test.cpp
run mesh_core   -Iinclude src/modules/mesh_pb.cpp src/modules/mesh_core.cpp $T/mesh_core_test.cpp
run mesh_spool  -Iinclude src/modules/mesh_spool.cpp $T/mesh_spool_test.cpp
run mesh_slots  -Iinclude $T/mesh_slots_test.cpp
run palm_text   -Iinclude src/modules/palm_text.cpp src/modules/translit.cpp src/modules/translit_data.cpp $T/palm_text_test.cpp
run carddav     -Iinclude src/modules/carddav_card.cpp src/modules/palm_text.cpp src/modules/translit.cpp src/modules/translit_data.cpp $T/carddav_card_test.cpp
run relay_proto -I$T/fake -Iinclude src/modules/relay_proto.cpp $T/relay_proto_test.cpp

if [ "$BUILD" = 1 ] && [ -n "$PROJ" ]; then
  note "3. builds with a feature off"
  cd "$PROJ" || exit 1
  build() { local r; r=$(PLATFORMIO_BUILD_FLAGS="$1" "$PIO" run 2>&1); if echo "$r" | grep -q SUCCESS; then echo "ok    ${1:-default}"; else fail "build $1: $(echo "$r" | grep -m2 ' error' | cut -c1-140 | tr '\n' ' ')"; fi; }
  for R in 0 1; do for I in 0 1; do for G in 0 1; do for C in 0 1; do
    build "-DWIRSA_FEATURE_CALDAV_REPORT=$R -DWIRSA_FEATURE_CALDAV_ICS=$I -DWIRSA_FEATURE_CALDAV_GOOGLE=$G -DWIRSA_FEATURE_CARDDAV=$C"
  done; done; done; done
  for F in CALDAV BITWARDEN MESH SLIP FTPSERVER HOTSYNC SASD UTILITIES PALMCTL DUAL_BAUD AUTOBAUD BOOT_HOTSYNC; do build "-DWIRSA_FEATURE_$F=0"; done
  build "-DWIRSA_BW_QUEUE=0"; build "-DWIRSA_CDDV_QUEUE=0"; build "-DWIRSA_CDDV_KEEPALIVE=0"
  build "-DWIRSA_FEATURE_CALDAV=0 -DWIRSA_FEATURE_BITWARDEN=0 -DWIRSA_FEATURE_MESH=0 -DWIRSA_FEATURE_SLIP=0"
  build "-DWIRSA_FEATURE_FTPSERVER=0 -DWIRSA_FEATURE_HOTSYNC=0 -DWIRSA_FEATURE_SASD=0 -DWIRSA_FEATURE_SLIP=0 -DWIRSA_FEATURE_BITWARDEN=0 -DWIRSA_FEATURE_CALDAV=0 -DWIRSA_FEATURE_MESH=0 -DWIRSA_FEATURE_UTILITIES=0"
  build "-DWIRSA_FEATURE_PALMCTL=0 -DWIRSA_FEATURE_DUAL_BAUD=0 -DWIRSA_FEATURE_AUTOBAUD=0 -DWIRSA_FEATURE_BOOT_HOTSYNC=0"
  build "-DWIRSA_FEATURE_HOTSYNC=0 -DWIRSA_FEATURE_BOOT_HOTSYNC=1"
  note "4. default build"
  build ""
else
  note "3, 4. builds skipped (no project directory given, or --no-build)"
fi

[ "$FAILED" = 0 ] && printf '\nAll checks passed.\n' || printf '\nSomething FAILED (see above).\n'
exit "$FAILED"
