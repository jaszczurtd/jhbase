# Test the library

`./runalltests.sh` runs the Linux stages below. A failed command stops the
suite. `--stage NAME` runs the same stage independently. Builds and generated
corpora stay below `.build/gate`; the `clean` stage replaces only its own copy.

| Stage | Checks |
| --- | --- |
| `host` | GCC Release/LTO, UTF-8/JSON/numeric/writer/literal tests, C++17, independent shortest-format checks, compiled C/C++ literals and examples, ABI rejection probes, C and C++ consumers of the installed package. |
| `sanitizers` | Clang ASan, UBSan and LeakSanitizer over library and executables. |
| `memcheck` | Valgrind over the actual test executables, literal probes and examples. |
| `fuzz` | Separate seeded libFuzzer runs for parser/UTF-8, exact numbers/literals and writer operations; each has a bounded run count. |
| `freestanding` | Compile, archive and link an analysis probe for the profiles in `scripts/freestanding.sh`: Cortex-M0+ (RP2040), M33 (RP2350 ARM, softfp), M4 (STM32G474, hard-float), RP2350 RISC-V and ESP32-S3 (Xtensa), each with the machine flags of its firmware; collect sizes/stack usage and reject heap symbols in owned objects and the complete linked probe. |
| `comparison` | Run the pinned coreJSON/yyjson comparison with instrumented upstream sources and the production parser. |
| `clean` | First configure/build/test from a source copy with spaces and no build state, then an incremental build. |
| `analysis` | The cppcheck build pinned by `third_party/cppcheck_version.conf`, checking the portable C modules. |
| `quality` | Shell syntax, clang-format, source whitespace, PMD CPD over owned production/test/example code, and the git hooks exercised in a scratch repository (`tests/check_hooks.sh`). |

Required system tools are CMake, Ninja, GCC/G++, Clang/Clang++ 18 with
libFuzzer, Valgrind, arm-none-eabi GCC/binutils, Java, clang-format, ripgrep,
curl, unzip, tar and Git. Unity, cppcheck, PMD and the RISC-V and Xtensa
toolchains are pinned in `third_party/*_version.conf` and installed below `.build/tools` by
`./scripts/ensure_tools.sh`, which verifies revisions and SHA-256 sums.
The gate only checks that they are present; `--tools-root` or
`JHBASE_TOOLS_ROOT` selects another location. Tests read the existing HAL
`tests/fixtures/device_tree/cases.json` numeric expectations. They do not read device-test firmware.
LeakSanitizer requires an environment without ptrace restrictions.
CTest writes JUnit XML in each host build directory. The `vscode_workspace`
suite (label `tooling`) checks the VS Code task documents and runs the
workspace driver on a scratch copy of the tree; see [VS Code](vscode.md).
`-DJHBASE_VSCODE_CHECK=OFF` leaves it out of a build.

The tests contain independent byte and grammar vectors, decoded duplicate
names, exact raw numeric spellings, capacity boundaries, unchanged outputs,
deep nesting and scratch reuse. They are independent of Python and SDKs.
Fuzzing also checks token ranges, parent/sibling relations and decoded UTF-8.
This bounded run is a smoke check, not proof of exhaustive fuzz coverage.

Install the pinned tools and comparison sources once, then run the complete
Linux gate:

```sh
./scripts/ensure_tools.sh
./scripts/prepare_comparison.sh
./runalltests.sh -j4
./runalltests.sh --list-stages
./runalltests.sh --stage host -j4
```

Both preparation scripts download only pinned, verified content below `.build`.
The gate itself downloads nothing and stops with the repair command when a
pinned tool is missing. See [parser selection](parser-choice.md).
The `comparison` stage accepts `JHBASE_COMPARISON_ROOT` for an explicit cache.

`--commit [REF]` runs that commit's own runner from a detached clean worktree,
then removes the worktree. It defaults to HEAD and reads the numeric fixture
from the explicitly selected HAL checkout. Local uncommitted changes are
excluded, including changes to the runner itself:

```sh
./runalltests.sh --commit HEAD --hal-root /path/to/matching/HAL -j4
```

Each fuzzer currently runs with `-runs=100000 -seed=8259 -max_len=4096 -timeout=3`.
Corpora and crash artifacts have separate directories for each executable.
Sanitizers and leak checking stay enabled. CPD rejects duplicate blocks of at
least 100 tokens in both owned scopes; upstream comparison code is outside
these scopes. Device-test code is excluded from default checks.

The literal probe uses independent expected bits from the existing HAL fixture,
then compiles and runs actual generated literals as C11 and C++17. Numeric reads
also use fixed IEEE vectors and direct native decimal reads; shortest output is
checked against C++17 `to_chars`/`from_chars`.
Complete examples are extracted from each API Markdown file and exercised as
C11 and C++17. Changes to their code trigger regeneration; translated examples
must stay identical. The standalone numeric/writer examples run in both languages too.

For native x64 and Win32 MSVC, use [the Windows checklist](windows.md) and
`runalltests.ps1`. It runs the same CTest suites, including layout/packing probes.
The short-enum compiler-flag probes apply only to GCC/Clang: they reject an
unselected short status enum and accept the explicit fixed-width selector.
The GNU linker allocation-guard test is Linux-specific.

Cross compilation and static `.su` reports do not establish on-device timing,
peak runtime stack or linked firmware size; the
[device measurements](#device-measurements) do. Linux results do not establish
MSVC/Win32 acceptance. No CI jobs or production generator workflows are added
by this prototype.

## Device measurements

`scripts/measure_device.sh` measures the library on a board. It is a separate,
explicit run: the gate never builds, flashes or reads device code, and the
default checks skip `tests/device`.

The script builds the archive of the target's freestanding profile, links it
into the JaszczurHAL firmware in `tests/device` and flashes the board. The
firmware parses and writes the documents in `tests/device/inputs`, reads their
numbers and prints a report. The script adds the bytes the library takes in
the linked image, read from the linker map.

| Target | Assemblies | Profile | Flashing and report port |
| --- | --- | --- | --- |
| `rp2040` | `pico`, `picow` | `cortex-m0plus` | picotool from the HAL `.build/tools`; the board's USB serial port |
| `rp2350-arm` | `pico2w` | `cortex-m33` | picotool from the HAL `.build/tools`; the board's USB serial port |
| `rp2350-riscv` | `pico2-riscv` | `rp2350-riscv` | picotool from the HAL `.build/tools`; the board's USB serial port |
| `stm32g474` | `nucleo`, `nucleo-canhat` | `cortex-m4` | OpenOCD through ST-LINK; the ST-LINK serial port |
| `esp32s3` | `esp32s3-zero` | `esp32s3` | ESP-IDF set up by HAL; the USB Serial/JTAG port |

```sh
./scripts/measure_device.sh --target rp2040 --assembly picow --serial <serial>
./scripts/measure_device.sh --target rp2350-arm --assembly pico2w --serial <serial>
./scripts/measure_device.sh --target rp2350-riscv --assembly pico2-riscv --serial <serial>
./scripts/measure_device.sh --target stm32g474 --assembly nucleo-canhat
./scripts/measure_device.sh --target esp32s3 --assembly esp32s3-zero
```

The script needs a JaszczurHAL checkout with its tools installed
(`--hal-root`, default `../JaszczurHAL`). RP boards also need
`--serial`, the number `picotool info -a` prints (`flash id` on RP2040,
`chipid` on RP2350): picotool flashes only that board, and the report port is
found by it. The board may wait in BOOTSEL or
run JaszczurHAL firmware, which enters BOOTSEL when the script closes its USB
port at 1200 bps. `--port` overrides
the detected serial port and `--no-flash` only builds and prints the size.
Builds and `report.txt` stay in `tests/device/.build/<assembly>`. For ESP32-S3
the script refuses an ESP-IDF build that does not use the Xtensa toolchain
pinned in `third_party/xtensa_toolchain_version.conf`.

The archive and the firmware must share one ABI. For the Pico SDK and STM32
builds the script compares the machine flags (the `-m` options) of `app.c`
with the profile and stops when they differ. RP2350 RISC-V firmware is
compiled by the same pinned GCC from `.build/tools` that compiles the archive.

The `pico2-riscv` assembly describes a Pico 2 with its LED on GPIO25. It also
runs on a Pico 2 W, but the LED stays dark there: that board's LED is wired to
the CYW43, which HAL does not drive in RISC-V builds. The serial report is then
the only sign of the result.

Each `JBM case=` line describes one case:

- `ns` is the time per call: the fastest of five batches of at least 40 ms,
  with interrupts enabled.
- `stack` counts the bytes written below the measuring frame, with interrupts
  masked. Values below 16 B cannot be told apart (`baseline_stack` is an empty
  call).
- `scratch_used` is the highest scratch byte the case wrote;
  `scratch_required` is what the `*_workspace_size` functions ask for.

`JBM size` gives the library code and constants (`flash`) and its static data
(`ram`) in the linked image. `JBM end` sums up eight independent checks:
correct rounding of binary32 and binary64 vectors, exact decimal comparison,
full uint64 and int64 ranges, measure against parse, UTF-8 validation and
written floats reading back to the same bits. `result=PASS` requires all of
them. The LED blinks during the measurement, then once per second for PASS or
five times per second for FAIL; an addressable LED shows blue, then green or
red.
