# Dependencies and build profile

The portable library is C11. Its public headers also support C++17; the library
does not link a C++ runtime. CMake checks the reviewed dependency hashes and
does not download dependencies during configure.

## Status codes and HAL

`jb/status.h` holds the only definition of `hal_status_t` and its helpers, and
`jb/array.h` the only `COUNTOF`. JaszczurHAL depends on jhbase:
`hal/core/hal_status.h` and `hal/core/hal_array.h` include these headers.

`hal_status_t` is 32 bits wide everywhere. On GCC and Clang the header gives
the enum `mode(SI)`, so `-fshort-enums`, the ARM EABI default, leaves it
alone while other enums keep their size. MSVC always uses a 32-bit enum. No
define is needed.

The library does not configure or link HAL. Only the literal test reads the
numeric fixture from a HAL checkout, by default `../JaszczurHAL`; select
another with `JHBASE_HAL_ROOT`. The Unity source hashes are recorded in
[dependencies.cmake](../../cmake/dependencies.cmake).

## Installed package

`cmake --install` writes the headers, the static library and a CMake package:
`find_package(jhbase)` gives `jb::base` and `jb::status`. The package records
the library revision in `jhbase_REVISION` and the layout of the public headers
in `jhbase_ABI_PROFILE`, for example `natural-ptr32-status32-bool8`. The
revision comes from `JHBASE_REVISION`, otherwise from git HEAD of this
checkout with a `-dirty` suffix for local changes, and is `unknown` in a tree
without git. The version file accepts only the same minor version and the same
pointer width.

A parent project that adds jhbase with `add_subdirectory` builds the library
only when it links `jb::base`, and registers no jhbase tests. Its install
writes just the status headers (`abi.h`, `array.h`, `status.h`) and the
`jb::status` package; `JHBASE_INSTALL_LIBRARY=ON` adds the library and the
remaining headers.

## Optional Unicode

`JHBASE_UNICODE=ON` builds `jb::unicode`, linking the base library and an
unmodified utf8proc checkout ignored by Git. The
[pin](../../third_party/utf8proc_version.conf) records utf8proc 2.12.0 with
Unicode 18.0.0. Prepare it explicitly with
`cmake -P scripts/ensure_utf8proc.cmake`; `-DJB_VERIFY_ONLY=ON` checks it
without downloading or replacing anything. To share a checkout managed by a
parent project, set `JHBASE_UTF8PROC_DIR` to its absolute path. An empty value
selects `third_party/utf8proc`; an invalid external path fails without falling
back to it. The same pin applies to both paths. The preparation script only
manages the default path; configure and build leave external sources untouched.
Configure and each build of the Unicode module verify revision,
origin, clean state and actual source bytes against Git objects, including
edits hidden by index flags. Installation takes the license from the selected
checkout. The
[upstream license](https://github.com/JuliaStrings/utf8proc/blob/79cdd5ab40c79afa559f689ffc13b76812dee1ac/LICENSE.md) is MIT for code,
Unicode-DFS-2015 for the tables.

Tests also need the independent normalization corpus and its Unicode-3.0
license. Their [pin](../../third_party/unicode_test_version.conf) records
download URLs and SHA-256 sums; the Unicode version comes from the utf8proc
pin. The license URL uses a full commit in the official ICU repository;
the complete upstream file retains the Unicode notice and additional ICU
notices. Pin validation rejects URLs without a full commit.
`cmake -P scripts/ensure_unicode_test.cmake` prepares them in the ignored
`tests/fixtures/unicode` directory. `-DJB_VERIFY_ONLY=ON` checks them offline.
Configure verifies the actual file hashes; the staged hook validates only
the pin and needs no downloaded data. The Windows test bundle includes the
verified files.

The wrapper uses buffered operations with explicit lengths. Allocating
upstream functions stay unmodified and section GC removes them. Linked
normalization still needs the Unicode tables in flash; avoiding heap does
not remove that cost. The default build leaves the module out. A full
install with Unicode enabled exports `jb::unicode` and ships its license.
Scratch ownership and work limits are described in [the API](api.md#unicode).

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

Unity, cppcheck, PMD, the RISC-V and Xtensa toolchains and the oldest supported
CMake are pinned in the
[third_party](../../third_party) `*_version.conf` files and fetched, verified
and built below `.build/tools` by [ensure_tools.sh](../../scripts/ensure_tools.sh).
Those files are the only pins of Unity, cppcheck, PMD, the RISC-V
toolchain and the oldest supported CMake: JaszczurHAL reads them from its pinned jhbase checkout instead of
keeping copies, so both gates run one revision of each tool. They carry
revisions and digests only; each project chooses its own install directories.
The Xtensa toolchain serves the ESP32-S3 profile. JaszczurHAL gets its Xtensa
compiler through ESP-IDF and checks that this pin matches the ESP-IDF tools
manifest, because the device archive links into ESP-IDF firmware.
