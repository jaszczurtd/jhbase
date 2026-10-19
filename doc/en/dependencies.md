# Dependencies and build profile

The portable library is C11. Its public headers also support C++17; the library
does not link a C++ runtime. CMake checks the reviewed dependency hashes and
does not download dependencies during configure.

## Shared HAL inputs

Until the first integration pin, `jb/status.h` borrows the sole definition of
`hal_status_t` from HAL. Tests and examples also use `hal_array.h`, and the
literal test reads the numeric fixture from the explicit HAL checkout. No HAL
runtime, SDK or production generator is configured or linked.

The HAL header hashes and the Unity source hashes are recorded in
[dependencies.cmake](../../cmake/dependencies.cmake). The default HAL directory
is `../JaszczurHAL`; select another with `JHBASE_HAL_ROOT`.

`jb::status` propagates `HAL_STATUS_FIXED_32`. On GCC/Clang this gives the status
enum a 32-bit representation without changing global enum-size flags or ARM
ABI attributes used by libc. MSVC already uses a 32-bit enum. Define the
selector consistently before the first status include when compiling directly.
The original HAL default layout remains unchanged without the selector.

## Buffered Ryu output

The unmodified subset in [third_party/ryu](../../third_party/ryu/version.json)
comes from [Ryu revision 4c0618b0](https://github.com/ulfjack/ryu/tree/4c0618b0e44f7ef027ebae05d2cc7812048f7c8f).
Its [SHA-256 manifest](../../third_party/ryu/SHA256SUMS) covers every shipped
source, header and license. This distribution selects
[Boost Software License 1.0](../../third_party/ryu/LICENSE-Boost); the upstream
[Apache 2.0 alternative](../../third_party/ryu/LICENSE-Apache2) is also preserved.
The project's [own license](../../LICENSE) applies to jhbase code.

The writer calls only `f2s_buffered_n` and `d2s_buffered_n`, using
`RYU_OPTIMIZE_SIZE` and `RYU_ONLY_64_BIT_OPS` on every target. These functions
write into fixed local buffers. Decimal input conversion and fixed-precision
C literals use the jhbase exact arithmetic module instead.

Upstream sources also contain allocating helpers. They remain unmodified and
outside the public jhbase API. Section garbage collection removes them from
linked programs. `jb::base` propagates that link policy; direct builds must
use function/data sections and section GC, or the MSVC `/Gy /Gw` and `/OPT:REF`
equivalents. Upstream assertions are disabled for this bounded internal
conversion path; instrumented tests exercise the actual buffered functions.

The freestanding check examines owned object references and the complete
linked analysis probe, including Ryu and libc, for allocation symbols. A host
allocation guard also aborts on malloc/calloc/realloc/free during library calls.
The link probe has an analysis-only memory map; it is not flashable firmware.
Measurements on boards use the firmware in `tests/device`, described in
[testing](testing.md#device-measurements).

## Research and quality tools

coreJSON and yyjson are pinned research snapshots described in
[parser selection](parser-choice.md); neither is part of the portable library.
Unity is a host test dependency. Valgrind, sanitizers, cppcheck and PMD CPD
are quality tools, not runtime dependencies. Tool requirements and independent
Windows checks are listed in [testing](testing.md) and [Windows setup](windows.md).

Unity, cppcheck, PMD and the RISC-V and Xtensa toolchains are pinned in the
[third_party](../../third_party) `*_version.conf` files and fetched, verified
and built below `.build/tools` by [ensure_tools.sh](../../scripts/ensure_tools.sh).
Those files are the only pins of Unity, cppcheck, PMD and the RISC-V
toolchain: JaszczurHAL reads them from its pinned jhbase checkout instead of
keeping copies, so both gates run one revision of each tool. They carry
revisions and digests only; each consumer chooses its own install directories.
The Xtensa toolchain serves the ESP32-S3 profile. JaszczurHAL gets its Xtensa
compiler through ESP-IDF and checks that this pin matches the ESP-IDF tools
manifest, because the device archive links into ESP-IDF firmware.
