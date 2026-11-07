#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the libFuzzer targets in fuzz/ (clang, ASan + UBSan, a host build in
# build-fuzz/) and runs each for a while.
#
#   tools/fuzz.sh [seconds per target, default 60] [target ...]
#   tools/fuzz.sh 300 handshake zip
#
# Targets: handshake netmsg chat zip inflate json jass paths blp signature update wasm_abi wasm_layout wamr (fuzz/fuzz_<name>.cpp).
# The corpus grows in build-fuzz/corpus/<target> from run to run, seeded by
# fuzz/make_seeds.py. A crashing input lands in build-fuzz/crashes/<target>/;
# after fixing it, copy it to fuzz/regressions/<target>/ so later runs start
# from it. FUZZ_JOBS=n runs n processes per target (libFuzzer -fork).
# FUZZ_BUILD moves build-fuzz/ (the container's: make container-fuzz).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(realpath -m "${FUZZ_BUILD:-$ROOT/build-fuzz}")"
SECONDS_EACH="${1:-60}"
shift || true
TARGETS=("$@")
[[ ${#TARGETS[@]} -gt 0 ]] || TARGETS=(handshake netmsg chat zip inflate json jass paths blp signature update wasm_abi wasm_layout wamr)

cmake -S "$ROOT/fuzz" -B "$BUILD" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "$BUILD"
python3 -I "$ROOT/fuzz/make_seeds.py" "$BUILD/seeds"

failed=()
for target in "${TARGETS[@]}"; do
    corpus="$BUILD/corpus/$target"
    crashes="$BUILD/crashes/$target"
    mkdir -p "$corpus" "$crashes"
    echo "== $target (${SECONDS_EACH}s)"
    args=(-max_total_time="$SECONDS_EACH" -artifact_prefix="$crashes/" -print_final_stats=1 -max_len=65536)
    [[ -f "$ROOT/fuzz/$target.dict" ]] && args+=(-dict="$ROOT/fuzz/$target.dict")
    [[ -n "${FUZZ_JOBS:-}" ]] && args+=(-fork="$FUZZ_JOBS" -ignore_crashes=0)
    if ! "$BUILD/fuzz_$target" "${args[@]}" "$corpus" "$BUILD/seeds/$target" 2>"$BUILD/$target.log"; then
        failed+=("$target")
        echo "   CRASH: see $BUILD/$target.log and $crashes/"
    fi
    grep -E "^#[0-9]+.*DONE|stat::number_of_executed_units|stat::average_exec_per_sec|cov:" "$BUILD/$target.log" |
        tail -n 3 | sed 's/^/   /'
done

if [[ ${#failed[@]} -gt 0 ]]; then
    echo "fuzz: crashes in ${failed[*]}"
    exit 1
fi
echo "fuzz: no crashes"
