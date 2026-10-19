# Freestanding profiles: compiler and target ABI flags of each profile. The
# gate (runalltests.sh, stage freestanding) builds and links every profile;
# scripts/measure_device.sh links the archive of a profile into device
# firmware, so the flags of a profile are the firmware ABI of its target
# (the script refuses Pico SDK and STM32 firmware with other -m flags).
# Sourced by both; expects JB_ROOT, JB_HAL, JB_TOOLS and the RISC-V pin.

JB_PROFILES=(cortex-m0plus cortex-m33 cortex-m4 rp2350-riscv esp32s3)

# jb_profile NAME: set JB_PROFILE_TOOL (compiler prefix) and JB_PROFILE_FLAGS.
jb_profile() {
  JB_PROFILE_TOOL=arm-none-eabi
  case "$1" in
    cortex-m0plus) JB_PROFILE_FLAGS=(-mcpu=cortex-m0plus -mthumb -mfloat-abi=soft) ;;
    # RP2350 ARM: the Pico SDK flags of a secure image, FPU instructions with
    # the softfp ABI.
    cortex-m33) JB_PROFILE_FLAGS=(-mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mcmse -mfloat-abi=softfp) ;;
    # STM32G474 firmware uses the hard-float ABI.
    cortex-m4) JB_PROFILE_FLAGS=(-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard) ;;
    # The flags the Pico SDK picks for the pinned GCC (Zcb and Zcmp accepted).
    rp2350-riscv)
      JB_PROFILE_TOOL="${JB_TOOLS}/riscv-toolchain/bin/${RISCV_TOOLCHAIN_TRIPLE}"
      JB_PROFILE_FLAGS=(-march=rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb_zca_zcb_zcmp -mabi=ilp32 -mstrict-align) ;;
    # ESP-IDF compiles Xtensa code with long calls.
    esp32s3)
      JB_PROFILE_TOOL="${JB_TOOLS}/xtensa-toolchain/bin/xtensa-esp32s3-elf"
      JB_PROFILE_FLAGS=(-mlongcalls) ;;
    *) printf 'Unknown freestanding profile: %s\n' "$1" >&2; return 1 ;;
  esac
}

# jb_profile_compile NAME DIR: compile the library and Ryu for a profile into
# a fresh DIR and archive them as DIR/libjhbase.a. Sets JB_PROFILE_OWNED (own
# objects) and JB_PROFILE_OBJECTS (all objects).
jb_profile_compile() {
  local jb_name="$1" jb_directory="$2" jb_source jb_object
  jb_profile "$jb_name"
  cmake -E rm -rf "$jb_directory"
  mkdir -p "$jb_directory"
  JB_PROFILE_COMMON=(-std=c11 -O2 -ffreestanding -fno-builtin -fstrict-aliasing
    -DHAL_STATUS_FIXED_32 -fstack-usage -ffunction-sections -fdata-sections
    "-I${JB_ROOT}/include" "-I${JB_HAL}/src" "-I${JB_ROOT}/third_party/ryu")
  JB_PROFILE_WARNINGS=(-Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow -Wvla)
  JB_PROFILE_OWNED=() JB_PROFILE_OBJECTS=()
  for jb_source in "${JB_ROOT}"/src/*.c; do
    jb_object="${jb_directory}/$(basename "${jb_source%.c}").o"
    "${JB_PROFILE_TOOL}-gcc" "${JB_PROFILE_COMMON[@]}" "${JB_PROFILE_FLAGS[@]}" \
      "${JB_PROFILE_WARNINGS[@]}" -c "$jb_source" -o "$jb_object"
    JB_PROFILE_OWNED+=("$jb_object") JB_PROFILE_OBJECTS+=("$jb_object")
  done
  for jb_source in "${JB_ROOT}"/third_party/ryu/ryu/*.c; do
    jb_object="${jb_directory}/ryu-$(basename "${jb_source%.c}").o"
    "${JB_PROFILE_TOOL}-gcc" "${JB_PROFILE_COMMON[@]}" "${JB_PROFILE_FLAGS[@]}" -DNDEBUG \
      -DRYU_OPTIMIZE_SIZE -DRYU_ONLY_64_BIT_OPS -c "$jb_source" -o "$jb_object"
    JB_PROFILE_OBJECTS+=("$jb_object")
  done
  "${JB_PROFILE_TOOL}-ar" rcs "${jb_directory}/libjhbase.a" "${JB_PROFILE_OBJECTS[@]}"
}
