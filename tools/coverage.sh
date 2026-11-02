#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Line coverage of Tome's tests: a --coverage developer build (COVERAGE_BUILD,
# build-cov/), its unit tests (ctest) and, if the game is in input/, the e2e
# tests (E2E_COVERAGE=1: each game exits by itself so tome.mix and the mods
# write their counters), then the report by section (tools/coverage.py).
#
#   tools/coverage.sh [--no-e2e]     COVERAGE_MIN=90 fails below that
set -uo pipefail
cd "$(dirname "$0")/.."
BUILD="${COVERAGE_BUILD:-build-cov}"
E2E=1
[[ "${1:-}" == --no-e2e ]] && E2E=0
[[ -f input/war3.exe ]] || E2E=0

# Configured like `make`'s build folders (a developer build with the tools).
cov=--coverage
make --no-print-directory build-folder BUILD="$BUILD" DEV=ON DEV_TOOLS=ON BUILD_TYPE=Release \
    CMAKE_EXTRA="-DCMAKE_C_FLAGS=$cov -DCMAKE_CXX_FLAGS=$cov -DCMAKE_EXE_LINKER_FLAGS=$cov -DCMAKE_SHARED_LINKER_FLAGS=$cov" || exit 1
cmake --build "$BUILD" || exit 1
find "$BUILD" -name '*.gcda' -delete

status=0
(cd "$BUILD" && ctest --output-on-failure) || status=1
if ((E2E)); then
    E2E_COVERAGE=1 BUILD="$(realpath -m "$BUILD")" tools/e2e-test.sh "$BUILD" || status=1
    E2E_COVERAGE=1 BUILD="$(realpath -m "$BUILD")" tools/e2e-multiplayer.sh "$BUILD" || status=1
else
    echo "coverage: without the e2e tests (no input/war3.exe, or --no-e2e)"
fi
python3 -I tools/coverage.py "$BUILD" --min "${COVERAGE_MIN:-0}" --files --lines "$BUILD/coverage-missed.txt" || status=1
echo "coverage: lines never run: $BUILD/coverage-missed.txt"
exit $status
