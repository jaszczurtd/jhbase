#!/usr/bin/env bash
# Fetch, build and verify the gate tools pinned in third_party/*_version.conf:
# Unity, cppcheck, PMD and the RISC-V and Xtensa toolchains. The pin files carry revisions
# and digests only (JaszczurHAL reads the same files from its jhbase checkout);
# the directory layout below the tools root (default .build/tools) is this
# script's. Nothing is installed system-wide and the gate itself never
# downloads. --check only verifies and names the repair step.
set -euo pipefail
jb_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
jb_tools="${JHBASE_TOOLS_ROOT:-${jb_root}/.build/tools}"
jb_check=0
jb_jobs="$(nproc 2>/dev/null || printf '4')"
jb_selected=()
while (($#)); do
  case "$1" in
    --check) jb_check=1; shift ;;
    --tools-root) jb_tools="${2:?--tools-root needs a path}"; shift 2 ;;
    -j) jb_jobs="${2:?missing job count}"; shift 2 ;;
    -j[0-9]*) jb_jobs="${1#-j}"; shift ;;
    --help)
      printf '%s\n' 'Usage: scripts/ensure_tools.sh [--check] [--tools-root PATH] [-j N]' \
        '       [unity|cppcheck|pmd|riscv-toolchain|xtensa-toolchain ...]   (default: all)'
      exit 0 ;;
    -*) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
    *) jb_selected+=("$1"); shift ;;
  esac
done
((${#jb_selected[@]})) || jb_selected=(unity cppcheck pmd riscv-toolchain xtensa-toolchain)
mkdir -p "$jb_tools"
jb_tools="$(cd "$jb_tools" && pwd -P)"
jb_stamp=.jhbase-component
for jb_conf in unity cppcheck pmd riscv_toolchain xtensa_toolchain; do
  # shellcheck source=/dev/null
  source "${jb_root}/third_party/${jb_conf}_version.conf"
done

fail() { printf '%s\n' "$*" >&2; exit 1; }
# ready DIR PIN: DIR was published by this script for exactly PIN.
ready() { [[ -f "$1/${jb_stamp}" && "$(cat "$1/${jb_stamp}")" == "$2" ]]; }
# missing NAME DIR: in --check mode stop with the repair command.
missing() {
  ((jb_check)) || return 0
  fail "Pinned $1 is not ready at $2. Run ${jb_root}/scripts/ensure_tools.sh $1"
}
# staging DIR: fresh sibling directory that publish moves into place.
staging() { rm -rf "$1.staging"; mkdir -p "$1.staging"; printf '%s\n' "$1.staging"; }
# publish STAGING DIR PIN: replace DIR with the verified staging tree.
publish() { printf '%s\n' "$3" > "$1/${jb_stamp}"; rm -rf "$2"; mv "$1" "$2"; }
# checkout REPO REF DIR: shallow fetch of exactly REF.
checkout() {
  git init -q "$3"
  git -C "$3" fetch -q --depth 1 "$1" "$2"
  git -C "$3" checkout -q --detach FETCH_HEAD
  [[ "$(git -C "$3" rev-parse HEAD)" == "$2" ]] || fail "Fetched $1 is not $2"
}
# download URL SHA256: path of the cached, verified archive on stdout.
download() {
  local file
  file="${jb_tools}/downloads/$(basename "$1")"
  mkdir -p "${jb_tools}/downloads"
  if ! printf '%s  %s\n' "$2" "$file" | sha256sum --check --status 2>/dev/null; then
    curl -fsSL --retry 3 -o "$file.part" "$1"
    mv "$file.part" "$file"
    printf '%s  %s\n' "$2" "$file" | sha256sum --check --status ||
      { rm -f "$file"; fail "SHA-256 mismatch for $1"; }
  fi
  printf '%s\n' "$file"
}

ensure_unity() {
  local dir="${jb_tools}/Unity" pin="unity|${UNITY_REF}" work
  if ready "$dir" "$pin" && [[ "$(git -C "$dir" rev-parse HEAD)" == "$UNITY_REF" ]]; then
    return
  fi
  missing unity "$dir"
  work="$(staging "$dir")"
  checkout "$UNITY_REPO" "$UNITY_REF" "$work"
  publish "$work" "$dir" "$pin"
  printf 'Unity %s ready: %s\n' "$UNITY_VERSION" "$dir"
}

ensure_cppcheck() {
  local source="${jb_tools}/cppcheck-source" build="${jb_tools}/cppcheck" work
  # The build copies cfg/, addons/ and platforms/ next to the binary; FILESDIR
  # points there too. DISABLE_DMAKE keeps the pinned checkout unmodified.
  local definitions=(-DCMAKE_BUILD_TYPE=Release "-DFILESDIR=${build}/bin" -DBUILD_GUI=OFF
    -DBUILD_TESTS=OFF -DDISABLE_DMAKE=ON)
  local pin="cppcheck|${CPPCHECK_REF}|${definitions[*]}" source_pin="cppcheck-source|${CPPCHECK_REF}"
  if ready "$build" "$pin" && [[ -f "${build}/bin/cfg/std.cfg" ]] &&
      [[ "$("${build}/bin/cppcheck" --version 2>/dev/null)" == "Cppcheck ${CPPCHECK_VERSION}" ]]; then
    return
  fi
  missing cppcheck "$build"
  if ! ready "$source" "$source_pin"; then
    work="$(staging "$source")"
    checkout "$CPPCHECK_REPO" "$CPPCHECK_REF" "$work"
    publish "$work" "$source" "$source_pin"
  fi
  work="$(staging "$build")"
  cmake -S "$source" -B "$work" "${definitions[@]}"
  cmake --build "$work" -j "$jb_jobs"
  [[ "$("${work}/bin/cppcheck" --version)" == "Cppcheck ${CPPCHECK_VERSION}" &&
    -f "${work}/bin/cfg/std.cfg" ]] || fail "The built cppcheck is not ${CPPCHECK_VERSION}"
  publish "$work" "$build" "$pin"
  printf 'cppcheck %s ready: %s/bin/cppcheck\n' "$CPPCHECK_VERSION" "$build"
}

ensure_pmd() {
  local dir="${jb_tools}/pmd" pin="pmd|${PMD_VERSION}|${PMD_SHA256}" archive work
  local binary="${dir}/pmd-bin-${PMD_VERSION}/bin/pmd"
  if ready "$dir" "$pin" && [[ -x "$binary" ]]; then
    return
  fi
  missing pmd "$dir"
  archive="$(download "$PMD_URL" "$PMD_SHA256")"
  work="$(staging "$dir")"
  unzip -q "$archive" -d "$work"
  "${work}/pmd-bin-${PMD_VERSION}/bin/pmd" --version | grep -q "PMD ${PMD_VERSION}" ||
    fail "The extracted PMD is not ${PMD_VERSION}"
  publish "$work" "$dir" "$pin"
  printf 'PMD %s ready: %s\n' "$PMD_VERSION" "$binary"
}

# host_arch: suffix of the per-host SHA-256 keys in the toolchain pins.
host_arch() {
  case "$(uname -m)" in
    x86_64) printf '%s\n' X86_64 ;;
    aarch64 | arm64) printf '%s\n' AARCH64 ;;
    *) fail "No pinned toolchain for $(uname -m)" ;;
  esac
}
# toolchain NAME DIR URL SHA PIN GCC MAJOR: install a prebuilt GCC archive
# (tar.gz or tar.xz); GCC is the compiler path relative to the toolchain root.
toolchain() {
  local name="$1" dir="$2" url="$3" sha="$4" pin="$5" gcc="$6" major="$7" archive work
  if ready "$dir" "$pin" && [[ -x "${dir}/${gcc}" ]]; then
    return
  fi
  missing "$name" "$dir"
  archive="$(download "$url" "$sha")"
  work="$(staging "$dir")"
  tar -xf "$archive" -C "$work"
  # A lone top-level directory inside the archive is the toolchain root.
  local entries=("$work"/*)
  if ((${#entries[@]} == 1)) && [[ -d "${entries[0]}" ]]; then
    mv "${entries[0]}" "$work.inner"
    rm -rf "$work"
    mv "$work.inner" "$work"
  fi
  "${work}/${gcc}" --version | head -1 | grep -q " ${major}\." ||
    fail "The extracted ${name} is not GCC ${major}"
  publish "$work" "$dir" "$pin"
  printf '%s ready: %s\n' "$name" "${dir}/${gcc}"
}

ensure_riscv() {
  local arch sha_key asset
  arch="$(host_arch)"
  sha_key="RISCV_TOOLCHAIN_SHA256_${arch}_LIN"
  asset="${RISCV_TOOLCHAIN_ASSET_STEM}-${arch,,}-lin.tar.gz"
  toolchain riscv-toolchain "${jb_tools}/riscv-toolchain" \
    "${RISCV_TOOLCHAIN_RELEASE_BASE}/${RISCV_TOOLCHAIN_TAG}/${asset}" "${!sha_key}" \
    "riscv-toolchain|${RISCV_TOOLCHAIN_TAG}|${asset}|${!sha_key}" \
    "bin/${RISCV_TOOLCHAIN_TRIPLE}-gcc" "$RISCV_TOOLCHAIN_GCC_MAJOR"
}

ensure_xtensa() {
  local arch sha_key asset
  arch="$(host_arch)"
  sha_key="XTENSA_TOOLCHAIN_SHA256_${arch}_LIN"
  asset="${XTENSA_TOOLCHAIN_ASSET_STEM}-${arch,,}-linux-gnu.tar.xz"
  toolchain xtensa-toolchain "${jb_tools}/xtensa-toolchain" \
    "${XTENSA_TOOLCHAIN_RELEASE_BASE}/${XTENSA_TOOLCHAIN_VERSION}/${asset}" "${!sha_key}" \
    "xtensa-toolchain|${XTENSA_TOOLCHAIN_VERSION}|${asset}|${!sha_key}" \
    bin/xtensa-esp32s3-elf-gcc "$XTENSA_TOOLCHAIN_GCC_MAJOR"
}

for jb_name in "${jb_selected[@]}"; do
  case "$jb_name" in
    unity) ensure_unity ;;
    cppcheck) ensure_cppcheck ;;
    pmd) ensure_pmd ;;
    riscv-toolchain) ensure_riscv ;;
    xtensa-toolchain) ensure_xtensa ;;
    *) fail "Unknown tool: ${jb_name}" ;;
  esac
done
# The JaszczurHAL commit rules: route Git to the tracked hooks, as runmefirst.sh
# does there. A source copy without .git and the --check mode leave Git alone.
if ((!jb_check)) && git -C "$jb_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  chmod +x "$jb_root/.githooks/pre-commit" "$jb_root/.githooks/commit-msg"
  git -C "$jb_root" config core.hooksPath .githooks
  printf 'Git hooks configured: %s\n' "$(git -C "$jb_root" config --get core.hooksPath)"
fi
printf 'Pinned tools ready below %s\n' "$jb_tools"
