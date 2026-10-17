# Test the library

`./runalltests.sh` runs the Linux stages below. A failed command stops the
suite. `--stage NAME` runs the same stage independently. Builds and generated
corpora stay below `.build/gate`; the `clean` stage replaces only its own copy.

| Stage | Checks |
| --- | --- |
| `host` | GCC Release/LTO, UTF-8/JSON/numeric/writer/literal tests, C++17, independent shortest-format checks, compiled C/C++ literals and examples, ABI rejection probes. |
| `sanitizers` | Clang ASan, UBSan and LeakSanitizer over library and executables. |
| `memcheck` | Valgrind over the actual test executables, literal probes and examples. |
| `fuzz` | Separate seeded libFuzzer runs for parser/UTF-8, exact numbers/literals and writer operations; each has a bounded run count. |
| `freestanding` | Compile/link an analysis probe for Cortex-M0+, M33, M4 and RP2350 RISC-V; collect sizes/stack usage and reject heap symbols in owned objects and the complete linked probe. |
| `comparison` | Run the pinned coreJSON/yyjson comparison with instrumented upstream sources and the production parser. |
| `clean` | First configure/build/test from a source copy with spaces and no build state, then an incremental build. |
| `analysis` | The cppcheck build pinned by `third_party/cppcheck_version.conf`, checking the portable C modules. |
| `quality` | Shell syntax, clang-format, source whitespace, PMD CPD over owned production/test/example code, and the git hooks exercised in a scratch repository (`tests/check_hooks.sh`). |

Required system tools are CMake, Ninja, GCC/G++, Clang/Clang++ 18 with
libFuzzer, Valgrind, arm-none-eabi GCC/binutils, Java, clang-format, ripgrep,
curl, unzip, tar and Git. Unity, cppcheck, PMD and the RISC-V toolchain are
pinned in `third_party/*_version.conf` and installed below `.build/tools` by
`./scripts/ensure_tools.sh`, which verifies revisions and SHA-256 sums.
The gate only checks that they are present; `--tools-root` or
`JHBASE_TOOLS_ROOT` selects another location. Tests read the reviewed HAL
status/array headers and the existing `tests/fixtures/device_tree/cases.json`
numeric expectations. They do not read device-test firmware.
LeakSanitizer requires an environment without ptrace restrictions.
CTest writes JUnit XML in each host build directory.

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
then removes the worktree. It defaults to HEAD and uses the explicitly selected
HAL dependencies. Their hashes must match that revision. Local uncommitted
changes are excluded, including changes to the runner itself:

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
peak runtime stack or linked firmware size. Hardware measurements require a
separate explicit run. Linux results do not establish MSVC/Win32 acceptance.
No CI jobs or production generator workflows are added by this prototype.
