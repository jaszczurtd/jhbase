#!/usr/bin/env bash
# Compile research code separately; reports are object/stack estimates, not HIL.
set -euo pipefail
jb_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
jb_reference="${JHBASE_COMPARISON_ROOT:-${jb_root}/.build/comparison}"
jb_output="${jb_root}/.build/gate/comparison/targets"
mkdir -p "$jb_output"
for jb_cpu in cortex-m0plus cortex-m33 cortex-m4; do
  for jb_engine in coreJSON yyjson; do
    if [[ "$jb_engine" == coreJSON ]]; then
      jb_source="${jb_reference}/coreJSON/source/core_json.c"
      jb_include="${jb_reference}/coreJSON/source/include"
    else
      jb_source="${jb_reference}/yyjson/src/yyjson.c"
      jb_include="${jb_reference}/yyjson/src"
    fi
    arm-none-eabi-gcc -std=c11 -O2 -fstack-usage -ffunction-sections -fdata-sections \
      -mcpu="$jb_cpu" -mthumb -DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_UTILS=1 \
      "-I${jb_include}" -c "$jb_source" -o "${jb_output}/${jb_cpu}-${jb_engine}.o"
  done
done
arm-none-eabi-size "${jb_output}"/*.o > "${jb_output}/object-sizes.txt"
arm-none-eabi-nm -u "${jb_output}"/*.o > "${jb_output}/undefined.txt"
