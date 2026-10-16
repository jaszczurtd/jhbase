# Basic prototype acceptance

`./runalltests.sh` runs the Linux stages below. A failed command stops the
suite. `--stage NAME` runs the same stage independently. Builds and generated
corpora stay below `.build/gate`; the `clean` stage replaces only its own copy.

| Stage | Checks |
| --- | --- |
| `host` | GCC Release/LTO, Unity UTF-8/JSON tests, C++17 usage and ABI rejection probes. |
| `sanitizers` | Clang ASan, UBSan and LeakSanitizer over library and executables. |
| `memcheck` | Valgrind over the actual UTF-8, JSON and C++ test executables. |
| `fuzz` | A seeded, bounded libFuzzer smoke run over parser, measurement, reads and UTF-8. |
| `freestanding` | Compile the portable code for Cortex-M0+, M33 and M4; collect stack usage and reject heap symbols. |
| `clean` | First configure/build/test from a source copy with spaces and no build state, then an incremental build. |
| `analysis` | The pinned cppcheck executable from HAL, checking the portable C modules. |
| `quality` | Shell syntax, clang-format and Git whitespace checks. |

Required tools are CMake, Ninja, GCC/G++, Clang/Clang++ 18 with libFuzzer,
Valgrind, arm-none-eabi GCC/binutils, HAL's pinned cppcheck build, clang-format,
ripgrep, tar and Git.
No tool is installed automatically. The tests read only the pinned HAL
status/array headers and Unity files; they do not read device-test firmware.
LeakSanitizer requires an environment without ptrace restrictions.
CTest writes JUnit XML in each host build directory.

The tests contain independent byte and grammar vectors, decoded duplicate
names, exact raw numeric spellings, capacity boundaries, unchanged outputs,
deep nesting and scratch reuse. They are independent of Python and SDKs.
Fuzzing also checks token ranges, parent/sibling relations and decoded UTF-8.
This bounded run is a smoke check, not proof of exhaustive fuzz coverage.

For native MSVC, point CMake at Windows checkouts of the same dependencies:

```powershell
cmake -S . -B .build/msvc -G "Visual Studio 17 2022" -A x64 -DJHBASE_HAL_ROOT=C:/src/JaszczurHAL
cmake --build .build/msvc --config Release
ctest --test-dir .build/msvc -C Release --output-on-failure
```

These commands run the same CTest cases, including natural-layout and packing
probes. The short-enum GCC flag probe is specific to GCC/Clang.
An ARM compile does not verify on-device behavior, and a Linux run does not
verify MSVC or Win32. Full stage 1a acceptance still needs native MSVC,
broader parser comparison, numeric conversion, a writer and target measurements.
No CI jobs or production HAL workflows are changed by this prototype.
