#!/usr/bin/env bash
# ADR-182 C2 (I1) under ThreadSanitizer, Linux / WSL2 with GCC or Clang.
#
# Builds test_agentengine_test_driver twice with -fsanitize=thread:
#   normal  -- must pass 1,000 concurrent send/observe cycles with no TSan report;
#   control -- AGENTENGINE_TEST_DRIVER_C2_RACE makes session_snapshot read history() from the MCP
#              thread during a run; TSan must report a data race (proves the check can fail).
# Usage: tools/test_driver/c2_tsan.sh [build-root]   (default: $HOME/ae-build-tsan)
set -u
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
ROOT="${1:-$HOME/ae-build-tsan}"
common=(-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAGENTENGINE_WITH_PDF=OFF
        -DAGENTENGINE_BUILD_EXAMPLES=OFF -DAGENTENGINE_WITH_HTTPS=OFF
        -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread)

build() {  # $1 = tree, $2 = extra CXX flags
    cmake -S "$SRC" -B "$1" "${common[@]}" -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer $2" \
        > "$1.configure.log" 2>&1 || { echo "configure failed: $1.configure.log"; exit 2; }
    cmake --build "$1" --target test_agentengine_test_driver -j "$(nproc)" > "$1.build.log" 2>&1 \
        || { echo "build failed: $1.build.log"; exit 2; }
}

build "$ROOT" ""
build "$ROOT-race" "-DAGENTENGINE_TEST_DRIVER_C2_RACE"

export AE_C2_ITERATIONS=1000 TSAN_OPTIONS="halt_on_error=0 exitcode=0"
"$ROOT/tests/test_agentengine_test_driver" > "$ROOT/c2.log" 2>&1
normal_rc=$?
normal_races=$(grep -c 'WARNING: ThreadSanitizer' "$ROOT/c2.log")
"$ROOT-race/tests/test_agentengine_test_driver" > "$ROOT-race/c2.log" 2>&1
control_races=$(grep -c 'WARNING: ThreadSanitizer' "$ROOT-race/c2.log")

echo "normal:  exit $normal_rc, TSan reports $normal_races ($ROOT/c2.log)"
echo "control: TSan reports $control_races ($ROOT-race/c2.log)"
if [ "$normal_rc" -eq 0 ] && [ "$normal_races" -eq 0 ] && [ "$control_races" -gt 0 ]; then
    echo "C2: PASS"; exit 0
fi
echo "C2: FAIL"; exit 1
