#!/usr/bin/env bash
# Direct host build: compile, link and run the test suite without a generator.
#
# CMake remains the canonical build (see CMakeLists.txt) and is what CI uses.
# This script exists because some sandboxed environments stall compiler
# processes spawned by make, while direct invocations run fine. It is also
# simply faster for a full rebuild.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-build-direct}
JOBS=${JOBS:-6}
CXX=${CXX:-c++}
FLAGS="-std=c++17 -g -O0 -Wall -Wextra -Wpedantic -Wshadow -Wconversion"
INC="-Icore/include -Iplatform/sim/include -Itests -Ithird_party"

mkdir -p "$OUT"

SOURCES=$(find core/src platform/sim/src tests -name '*.cpp' | sort)

compile_one() {
  src="$1"
  obj="$OUT/$(echo "$src" | tr '/' '_' | sed 's/\.cpp$/.o/')"
  if [ -f "$obj" ] && [ "$obj" -nt "$src" ]; then exit 0; fi
  $CXX $FLAGS $INC -c "$src" -o "$obj"
}
export -f compile_one
export CXX FLAGS INC OUT

echo "$SOURCES" | xargs -P "$JOBS" -I{} bash -c 'compile_one "$@"' _ {}

$CXX $FLAGS "$OUT"/*.o -o "$OUT/meatpilot_tests"
echo "built $OUT/meatpilot_tests"
