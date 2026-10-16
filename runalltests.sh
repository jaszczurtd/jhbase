#!/usr/bin/env bash
# Acceptance for the standalone UTF-8/JSON parser prototype. No HAL gate or SDK.
set -euo pipefail

JB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
JB_HAL="${JHBASE_HAL_ROOT:-${JB_ROOT}/../libraries/JaszczurHAL}"
JB_JOBS=4
JB_STAGE=""
JB_STAGES=(host sanitizers memcheck fuzz freestanding clean analysis quality)
while (($#)); do
  case "$1" in
    --stage) JB_STAGE="${2:?--stage needs a name}"; shift 2 ;;
    --hal-root) JB_HAL="${2:?--hal-root needs a path}"; shift 2 ;;
    --list-stages) printf '%s\n' "${JB_STAGES[@]}"; exit 0 ;;
    -j) JB_JOBS="${2:?missing job count}"; shift 2 ;;
    -j[0-9]*) JB_JOBS="${1#-j}"; shift ;;
    --help)
      printf '%s\n' 'Usage: ./runalltests.sh [--stage NAME] [--hal-root PATH] [-j N]'
      exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
  esac
done
[[ "$JB_JOBS" =~ ^[1-9][0-9]*$ ]] || { printf '%s\n' 'Invalid job count' >&2; exit 2; }
JB_HAL="$(cd "$JB_HAL" && pwd -P)"
JB_GATE="${JB_ROOT}/.build/gate"
mkdir -p "$JB_GATE"
export LC_ALL=C

configure() {
  local jb_dir="$1"; shift
  cmake -S "$JB_ROOT" -B "$jb_dir" -G Ninja \
    "-DJHBASE_HAL_ROOT=${JB_HAL}" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_FLAGS= -DCMAKE_CXX_FLAGS= "$@"
  cmake --build "$jb_dir" -j "$JB_JOBS"
}

host() {
  configure "${JB_GATE}/host" -DJHBASE_LTO=ON
  ctest --test-dir "${JB_GATE}/host" --output-on-failure \
    --output-junit "${JB_GATE}/host/ctest.xml"
}

sanitizers() {
  configure "${JB_GATE}/sanitizers" -DJHBASE_SANITIZERS=ON \
    -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    ctest --test-dir "${JB_GATE}/sanitizers" --output-on-failure \
      --output-junit "${JB_GATE}/sanitizers/ctest.xml"
}

memcheck() {
  configure "${JB_GATE}/memcheck" -DJHBASE_LTO=ON
  for jb_test in test_utf8 test_json test_cxx; do
    valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=all \
      --errors-for-leak-kinds=all "${JB_GATE}/memcheck/${jb_test}"
  done
}

fuzz() {
  configure "${JB_GATE}/fuzz" -DBUILD_TESTING=OFF -DJHBASE_FUZZ=ON \
    -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18
  mkdir -p "${JB_GATE}/fuzz/corpus" "${JB_GATE}/fuzz/artifacts"
  cp "${JB_ROOT}"/tests/corpus/* "${JB_GATE}/fuzz/corpus/"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "${JB_GATE}/fuzz/fuzz_json" "${JB_GATE}/fuzz/corpus" -runs=100000 \
      -seed=8259 -max_len=4096 -timeout=3 \
      "-artifact_prefix=${JB_GATE}/fuzz/artifacts/"
}

freestanding() {
  mkdir -p "${JB_GATE}/arm"
  for jb_cpu in cortex-m0plus cortex-m33 cortex-m4; do
    for jb_source in "${JB_ROOT}"/src/*.c; do
      local jb_object="${JB_GATE}/arm/${jb_cpu}-$(basename "${jb_source%.c}").o"
      arm-none-eabi-gcc -std=c11 -O2 -ffreestanding -fno-builtin -fstrict-aliasing \
        -fno-short-enums \
        -fstack-usage -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow \
        -mcpu="$jb_cpu" -mthumb "-I${JB_ROOT}/include" "-I${JB_HAL}/src" \
        -c "$jb_source" -o "$jb_object"
    done
  done
  arm-none-eabi-nm -u "${JB_GATE}"/arm/*.o > "${JB_GATE}/arm/undefined.txt"
  if rg -n '\b(malloc|calloc|realloc|free|alloca)\b' "${JB_GATE}/arm/undefined.txt"; then
    printf '%s\n' 'Portable code depends on dynamic allocation' >&2
    return 1
  fi
}

clean() {
  # All writes stay inside this project, including objects of external Unity.
  local jb_copy="${JB_GATE}/clean source"
  cmake -E rm -rf "$jb_copy"
  mkdir -p "$jb_copy"
  tar -C "$JB_ROOT" --exclude=./.git --exclude=./.build --exclude=./evidence \
    --exclude=./NOW.md -cf - . | tar -C "$jb_copy" -xf -
  cmake -S "$jb_copy" -B "${jb_copy}/.build/host" -G Ninja \
    "-DJHBASE_HAL_ROOT=${JB_HAL}" -DCMAKE_BUILD_TYPE=Release -DJHBASE_LTO=ON \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_FLAGS= -DCMAKE_CXX_FLAGS=
  cmake --build "${jb_copy}/.build/host" -j "$JB_JOBS"
  ctest --test-dir "${jb_copy}/.build/host" --output-on-failure \
    --output-junit "${jb_copy}/.build/host/ctest.xml"
  # This records a second no-op build, preserving the first-run evidence.
  cmake --build "${jb_copy}/.build/host" -j "$JB_JOBS"
}

analysis() {
  "${JB_HAL}/.build/tools/cppcheck/bin/cppcheck" \
    --enable=warning,style,performance,portability --error-exitcode=1 \
    --std=c11 --language=c "-I${JB_ROOT}/include" "-I${JB_HAL}/src" \
    "${JB_ROOT}/src"
}

quality() {
  bash -n "${JB_ROOT}/runalltests.sh"
  clang-format --dry-run --Werror "${JB_ROOT}"/include/jb/*.h \
    "${JB_ROOT}"/src/*.[ch] "${JB_ROOT}"/tests/*.c "${JB_ROOT}"/tests/*.cpp
  git -C "$JB_ROOT" diff --check
}

if [[ -n "$JB_STAGE" ]]; then
  jb_found=0
  for jb_name in "${JB_STAGES[@]}"; do
    [[ "$jb_name" != "$JB_STAGE" ]] || jb_found=1
  done
  ((jb_found)) || { printf 'Unknown stage: %s\n' "$JB_STAGE" >&2; exit 2; }
  JB_STAGES=("$JB_STAGE")
fi
for jb_name in "${JB_STAGES[@]}"; do
  printf 'Stage: %s\n' "$jb_name"
  "$jb_name"
done
printf '%s\n' 'PASS: all requested jhbase prototype stages'
