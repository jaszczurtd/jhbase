#!/usr/bin/env bash
# Local acceptance of the jhbase library on Linux. The gate installs nothing:
# scripts/ensure_tools.sh provides the pinned tools below the tools root first.
set -euo pipefail

JB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
JB_HAL="${JHBASE_HAL_ROOT:-${JB_ROOT}/../libraries/JaszczurHAL}"
JB_TOOLS="${JHBASE_TOOLS_ROOT:-${JB_ROOT}/.build/tools}"
JB_JOBS=4
JB_STAGE=""
JB_COMMIT=""
JB_STAGES=(host sanitizers memcheck fuzz freestanding comparison clean analysis quality)
while (($#)); do
  case "$1" in
    --stage) JB_STAGE="${2:?--stage needs a name}"; shift 2 ;;
    --commit)
      JB_COMMIT=HEAD; shift
      if (($#)) && [[ "$1" != -* ]]; then JB_COMMIT="$1"; shift; fi ;;
    --hal-root) JB_HAL="${2:?--hal-root needs a path}"; shift 2 ;;
    --tools-root) JB_TOOLS="${2:?--tools-root needs a path}"; shift 2 ;;
    --list-stages) printf '%s\n' "${JB_STAGES[@]}"; exit 0 ;;
    -j) JB_JOBS="${2:?missing job count}"; shift 2 ;;
    -j[0-9]*) JB_JOBS="${1#-j}"; shift ;;
    --help)
      printf '%s\n' 'Usage: ./runalltests.sh [--stage NAME] [--commit [REF]] [--hal-root PATH]' \
        '                       [--tools-root PATH] [-j N]'
      exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
  esac
done
[[ "$JB_JOBS" =~ ^[1-9][0-9]*$ ]] || { printf '%s\n' 'Invalid job count' >&2; exit 2; }
JB_HAL="$(cd "$JB_HAL" && pwd -P)"
mkdir -p "$JB_TOOLS"
JB_TOOLS="$(cd "$JB_TOOLS" && pwd -P)"
JB_GATE="${JB_ROOT}/.build/gate"
mkdir -p "$JB_GATE"
export LC_ALL=C

if [[ -n "$JB_COMMIT" ]]; then
  JB_REVISION="$(git -C "$JB_ROOT" rev-parse --verify "${JB_COMMIT}^{commit}")"
  JB_CHECKOUT="$(mktemp -d "${JB_GATE}/commit-XXXXXXXX")"
  rmdir "$JB_CHECKOUT"
  git -C "$JB_ROOT" worktree add --detach "$JB_CHECKOUT" "$JB_REVISION"
  trap 'git -C "$JB_ROOT" worktree remove --force "$JB_CHECKOUT"' EXIT
  JB_ARGS=(--hal-root "$JB_HAL" -j "$JB_JOBS")
  [[ -z "$JB_STAGE" ]] || JB_ARGS+=(--stage "$JB_STAGE")
  JHBASE_TOOLS_ROOT="$JB_TOOLS" \
    JHBASE_COMPARISON_ROOT="${JHBASE_COMPARISON_ROOT:-${JB_ROOT}/.build/comparison}" \
    "${JB_CHECKOUT}/runalltests.sh" "${JB_ARGS[@]}"
  exit 0
fi

if [[ -n "$JB_STAGE" ]]; then
  jb_found=0
  for jb_name in "${JB_STAGES[@]}"; do
    [[ "$jb_name" != "$JB_STAGE" ]] || jb_found=1
  done
  ((jb_found)) || { printf 'Unknown stage: %s\n' "$JB_STAGE" >&2; exit 2; }
  JB_STAGES=("$JB_STAGE")
fi

# Verify the pinned tools the selected stages use; the check downloads nothing.
jb_tools_needed=()
for jb_name in "${JB_STAGES[@]}"; do
  case "$jb_name" in
    host | sanitizers | memcheck | clean) jb_tool=unity ;;
    freestanding) jb_tool=riscv-toolchain ;;
    analysis) jb_tool=cppcheck ;;
    quality) jb_tool=pmd ;;
    *) continue ;;
  esac
  [[ " ${jb_tools_needed[*]-} " == *" ${jb_tool} "* ]] || jb_tools_needed+=("$jb_tool")
done
if ((${#jb_tools_needed[@]})); then
  "${JB_ROOT}/scripts/ensure_tools.sh" --check --tools-root "$JB_TOOLS" "${jb_tools_needed[@]}"
fi
# shellcheck source=/dev/null
source "${JB_ROOT}/third_party/pmd_version.conf"
# shellcheck source=/dev/null
source "${JB_ROOT}/third_party/riscv_toolchain_version.conf"
JB_UNITY="${JB_TOOLS}/Unity"

cmake "-DJHBASE_HAL_ROOT=${JB_HAL}" -P "${JB_ROOT}/cmake/dependencies.cmake"

configure() {
  local jb_dir="$1"; shift
  cmake -S "$JB_ROOT" -B "$jb_dir" -G Ninja \
    "-DJHBASE_HAL_ROOT=${JB_HAL}" "-DJHBASE_UNITY_ROOT=${JB_UNITY}" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON -DJHBASE_LTO=OFF -DJHBASE_SANITIZERS=OFF \
    -DJHBASE_FUZZ=OFF -DJHBASE_COMPARE=OFF \
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
  for jb_test in test_utf8 test_json test_numbers test_writer test_cxx test_float_format test_allocation \
      test_literal_probe_c test_literal_probe_cpp example_json_numbers_c \
      example_json_numbers_cpp example_json_writer_c example_json_writer_cpp \
      test_api_en_c test_api_en_cpp test_api_pl_c test_api_pl_cpp; do
    valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=all \
      --errors-for-leak-kinds=all "${JB_GATE}/memcheck/${jb_test}"
  done
  valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=all \
    --errors-for-leak-kinds=all "${JB_GATE}/memcheck/test_literals" \
    "${JB_HAL}/tests/fixtures/device_tree/cases.json"
}

fuzz() {
  configure "${JB_GATE}/fuzz" -DBUILD_TESTING=OFF -DJHBASE_FUZZ=ON \
    -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18
  for jb_fuzzer in json numbers writer; do
    mkdir -p "${JB_GATE}/fuzz/${jb_fuzzer}/corpus" "${JB_GATE}/fuzz/${jb_fuzzer}/artifacts"
    cp "${JB_ROOT}"/tests/corpus/* "${JB_GATE}/fuzz/${jb_fuzzer}/corpus/"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
      "${JB_GATE}/fuzz/fuzz_${jb_fuzzer}" "${JB_GATE}/fuzz/${jb_fuzzer}/corpus" \
        -runs=100000 -seed=8259 -max_len=4096 -timeout=3 \
        "-artifact_prefix=${JB_GATE}/fuzz/${jb_fuzzer}/artifacts/"
  done
}

freestanding_profile() {
  local jb_name="$1" jb_tool="$2"; shift 2
  local jb_directory="${JB_GATE}/arm/${jb_name}"
  cmake -E rm -rf "$jb_directory"
  mkdir -p "$jb_directory"
  local jb_owned=() jb_objects=()
  local jb_flags=(-std=c11 -O2 -ffreestanding -fno-builtin -fstrict-aliasing
    -DHAL_STATUS_FIXED_32 -fstack-usage -ffunction-sections -fdata-sections
    "-I${JB_ROOT}/include" "-I${JB_HAL}/src" "-I${JB_ROOT}/third_party/ryu")
  for jb_source in "${JB_ROOT}"/src/*.c; do
    local jb_object="${jb_directory}/$(basename "${jb_source%.c}").o"
    "${jb_tool}-gcc" "${jb_flags[@]}" "$@" -Wall -Wextra -Werror -Wpedantic \
      -Wconversion -Wshadow -Wvla -c "$jb_source" -o "$jb_object"
    jb_owned+=("$jb_object"); jb_objects+=("$jb_object")
  done
  for jb_source in "${JB_ROOT}"/third_party/ryu/ryu/*.c; do
    local jb_object="${jb_directory}/ryu-$(basename "${jb_source%.c}").o"
    "${jb_tool}-gcc" "${jb_flags[@]}" "$@" -DNDEBUG -DRYU_OPTIMIZE_SIZE \
      -DRYU_ONLY_64_BIT_OPS -c "$jb_source" -o "$jb_object"
    jb_objects+=("$jb_object")
  done
  "${jb_tool}-gcc" "${jb_flags[@]}" "$@" -Wall -Wextra -Werror -Wpedantic \
    -Wconversion -Wshadow -Wvla -c "${JB_ROOT}/tests/link_probe.c" \
    -o "${jb_directory}/link_probe.o"
  "${jb_tool}-gcc" "$@" -nostartfiles -Wl,--gc-sections,--fatal-warnings \
    "-Wl,-T,${JB_ROOT}/tests/link_probe.ld" "${jb_directory}/link_probe.o" \
    "${jb_objects[@]}" -o "${jb_directory}/link_probe.elf"
  "${jb_tool}-nm" -u "${jb_owned[@]}" > "${jb_directory}/undefined.txt"
  "${jb_tool}-nm" "${jb_directory}/link_probe.elf" > "${jb_directory}/linked-symbols.txt"
  # Ryu's unused allocating APIs are removed by section GC. Check both owned
  # object references and the complete linked probe, including upstream/libc.
  if rg -n '\b(malloc|calloc|realloc|free|alloca|_malloc_r|_calloc_r|_realloc_r|_free_r)\b' \
      "${jb_directory}/undefined.txt" "${jb_directory}/linked-symbols.txt"; then
    printf '%s\n' 'Portable code depends on dynamic allocation' >&2; return 1
  fi
  "${jb_tool}-size" "${jb_objects[@]}" > "${jb_directory}/object-sizes.txt"
  "${jb_tool}-size" "${jb_directory}/link_probe.elf" > "${jb_directory}/linked-probe-size.txt"
}

freestanding() {
  for jb_cpu in cortex-m0plus cortex-m33 cortex-m4; do
    freestanding_profile "$jb_cpu" arm-none-eabi -mcpu="$jb_cpu" -mthumb
  done
  freestanding_profile rp2350-riscv \
    "${JB_TOOLS}/riscv-toolchain/bin/${RISCV_TOOLCHAIN_TRIPLE}" \
    -march=rv32imac_zicsr_zifencei -mabi=ilp32
}

comparison() {
  local jb_reference="${JHBASE_COMPARISON_ROOT:-${JB_ROOT}/.build/comparison}"
  configure "${JB_GATE}/comparison" -DBUILD_TESTING=OFF -DJHBASE_COMPARE=ON \
    -DJHBASE_SANITIZERS=ON -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
    "-DJHBASE_COREJSON_ROOT=${jb_reference}/coreJSON" "-DJHBASE_YYJSON_ROOT=${jb_reference}/yyjson"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "${JB_GATE}/comparison/compare_parsers" | tee "${JB_GATE}/comparison/results.csv"
  JHBASE_COMPARISON_ROOT="$jb_reference" "${JB_ROOT}/scripts/compare_targets.sh"
}

clean() {
  # All writes stay inside this project, including objects of external Unity.
  local jb_copy="${JB_GATE}/clean source"
  cmake -E rm -rf "$jb_copy"
  mkdir -p "$jb_copy"
  tar -C "$JB_ROOT" --exclude=./.git --exclude=./.build --exclude=./evidence \
    --exclude=./NOW.md -cf - . | tar -C "$jb_copy" -xf -
  cmake -S "$jb_copy" -B "${jb_copy}/.build/host" -G Ninja \
    "-DJHBASE_HAL_ROOT=${JB_HAL}" "-DJHBASE_UNITY_ROOT=${JB_UNITY}" \
    -DCMAKE_BUILD_TYPE=Release -DJHBASE_LTO=ON \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_FLAGS= -DCMAKE_CXX_FLAGS=
  cmake --build "${jb_copy}/.build/host" -j "$JB_JOBS"
  ctest --test-dir "${jb_copy}/.build/host" --output-on-failure \
    --output-junit "${jb_copy}/.build/host/ctest.xml"
  # This records a second no-op build, preserving the first-run evidence.
  cmake --build "${jb_copy}/.build/host" -j "$JB_JOBS"
}

analysis() {
  "${JB_TOOLS}/cppcheck/bin/cppcheck" \
    --enable=warning,style,performance,portability --error-exitcode=1 \
    --std=c11 --language=c -DHAL_STATUS_FIXED_32 \
    "-I${JB_ROOT}/include" "-I${JB_HAL}/src" "-I${JB_ROOT}/third_party/ryu" \
    "${JB_ROOT}/src"
}

quality() {
  bash -n "${JB_ROOT}/runalltests.sh"
  for jb_script in "${JB_ROOT}"/scripts/*.sh "${JB_ROOT}"/tests/*.sh; do bash -n "$jb_script"; done
  for jb_hook in "${JB_ROOT}"/.githooks/*; do sh -n "$jb_hook"; done
  # The JaszczurHAL commit rules, exercised in a scratch repository.
  "${JB_ROOT}/tests/check_hooks.sh"
  clang-format --dry-run --Werror "${JB_ROOT}"/include/jb/*.h \
    "${JB_ROOT}"/src/*.[ch] "${JB_ROOT}"/tests/*.c "${JB_ROOT}"/tests/*.h \
    "${JB_ROOT}"/tests/*.cpp "${JB_ROOT}"/examples/*.c
  cmake "-DJB_SOURCE=${JB_ROOT}" -P "${JB_ROOT}/tests/check_source.cmake"
  if git -C "$JB_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -C "$JB_ROOT" diff --check
  fi
  local jb_pmd="${JB_TOOLS}/pmd/pmd-bin-${PMD_VERSION}/bin/pmd"
  # A flat list keeps device-test sources out of the default checks.
  printf '%s\n' "${JB_ROOT}"/tests/*.[ch] "${JB_ROOT}"/tests/*.cpp \
    "${JB_ROOT}"/examples/*.c > "${JB_GATE}/test-sources.txt"
  "$jb_pmd" cpd --minimum-tokens 100 --language cpp --dir "${JB_ROOT}/src" \
    --dir "${JB_ROOT}/include" --format text --report-file "${JB_GATE}/production-cpd.txt"
  "$jb_pmd" cpd --minimum-tokens 100 --language cpp --file-list "${JB_GATE}/test-sources.txt" \
    --format text --report-file "${JB_GATE}/tests-cpd.txt"
}

for jb_name in "${JB_STAGES[@]}"; do
  printf 'Stage: %s\n' "$jb_name"
  "$jb_name"
done
printf '%s\n' 'PASS: all requested jhbase prototype stages'
