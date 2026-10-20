#!/usr/bin/env bash
# Bundle reviewed working-tree sources, the HAL numeric test fixture and the
# pinned Unity sources for the native Windows runner.
set -euo pipefail
jb_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
jb_hal="${JHBASE_HAL_ROOT:-${jb_root}/../JaszczurHAL}"
jb_tools="${JHBASE_TOOLS_ROOT:-${jb_root}/.build/tools}"
jb_output="${jb_root}/.build/windows-transfer"
jb_payload="${jb_output}/payload"
"${jb_root}/scripts/ensure_tools.sh" --check --tools-root "$jb_tools" unity
cmake -DJB_VERIFY_ONLY=ON -P "${jb_root}/scripts/ensure_utf8proc.cmake"
cmake -DJB_VERIFY_ONLY=ON -P "${jb_root}/scripts/ensure_unicode_test.cmake"
mkdir -p "$jb_output"
[[ ! -L "$jb_payload" ]] || { printf '%s\n' 'Payload directory must not be a symlink' >&2; exit 1; }
cmake -E rm -rf "$jb_payload"
mkdir -p "${jb_payload}/jhbase" "${jb_payload}/tools/Unity/src" \
  "${jb_payload}/JaszczurHAL/tests/fixtures/device_tree"
git -C "$jb_root" ls-files --cached --others --exclude-standard -z > "${jb_output}/sources.list"
tar -C "$jb_root" --null --verbatim-files-from --no-recursion \
  -T "${jb_output}/sources.list" -cf - | tar -C "${jb_payload}/jhbase" -xf -
# The ignored source checkout is a dependency of the test bundle, including
# its independent Git metadata so native configure can verify the exact pin.
cp -a "${jb_root}/third_party/utf8proc" "${jb_payload}/jhbase/third_party/utf8proc"
mkdir -p "${jb_payload}/jhbase/tests/fixtures"
cp -a "${jb_root}/tests/fixtures/unicode" "${jb_payload}/jhbase/tests/fixtures/unicode"
for jb_source in unity.c unity.h unity_internals.h; do
  cp "${jb_tools}/Unity/src/${jb_source}" "${jb_payload}/tools/Unity/src/"
done
cp "${jb_tools}/Unity/LICENSE.txt" "${jb_payload}/tools/Unity/"
cp "${jb_hal}/LICENSE" "${jb_payload}/JaszczurHAL/"
cp "${jb_hal}/tests/fixtures/device_tree/cases.json" \
  "${jb_payload}/JaszczurHAL/tests/fixtures/device_tree/"
(
  cd "$jb_payload"
  rg --files --hidden --no-ignore -0 | sort -z | xargs -0 sha256sum
) > "${jb_output}/SHA256SUMS"
cp "${jb_output}/SHA256SUMS" "$jb_payload/"
tar -C "$jb_payload" -czf "${jb_output}/jhbase-windows-source.tar.gz" jhbase JaszczurHAL tools SHA256SUMS
sha256sum "${jb_output}/jhbase-windows-source.tar.gz"
