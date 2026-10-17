#!/usr/bin/env bash
# Fetch research snapshots only. Nothing is installed or linked into jhbase.
set -euo pipefail
jb_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
jb_reference="${jb_root}/.build/comparison"
mkdir -p "$jb_reference"
fetch() {
  local jb_name="$1" jb_url="$2" jb_tag="$3" jb_revision="$4"
  local jb_directory="${jb_reference}/${jb_name}"
  if [[ ! -e "$jb_directory" ]]; then
    git clone --depth 1 --branch "$jb_tag" "$jb_url" "$jb_directory"
  fi
  [[ "$(git -C "$jb_directory" rev-parse HEAD)" == "$jb_revision" ]] || {
    printf 'Wrong comparison revision: %s\n' "$jb_directory" >&2; exit 1;
  }
  [[ -z "$(git -C "$jb_directory" status --porcelain)" ]] || {
    printf 'Modified comparison checkout: %s\n' "$jb_directory" >&2; exit 1;
  }
}
fetch coreJSON https://github.com/FreeRTOS/coreJSON.git v3.3.0 e3b7663f6392d8c10e8db57506ec37e4801b145a
fetch yyjson https://github.com/ibireme/yyjson.git 0.12.0 8b4a38dc994a110abaec8a400615567bd996105f
printf '%s\n' 'Pinned comparison sources are ready. No production dependency changed.'
