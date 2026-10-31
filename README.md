# jhbase

*Also available in [Polish](README.pl.md).*

jhbase provides a portable C11 UTF-8 decoder/encoder and an immutable JSON
parser, exact numeric reads, C literals and a sequential JSON writer. Callers
supply token storage and scratch; the parser preserves original number tokens.

jhbase defines the status codes (`jb/status.h`) and `COUNTOF` (`jb/array.h`)
used by JaszczurHAL. The library does not configure or link HAL.

Build with CMake 3.22 or later and a C11/C++17 compiler, after
`./scripts/ensure_tools.sh unity`:

```sh
cmake -S . -B .build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build/host
ctest --test-dir .build/host --output-on-failure
```

The tests read a numeric fixture from the HAL checkout at `../JaszczurHAL`. Pass
`-DJHBASE_HAL_ROOT=/path/to/JaszczurHAL` and, if needed,
`-DJHBASE_TOOLS_ROOT=/path/to/tools` to select explicit locations.
Configure downloads nothing and writes nothing to those directories.
`cmake --install` installs the library, its headers and a CMake package
(`find_package(jhbase)`); see [the installed package](doc/en/dependencies.md#installed-package).

On Linux, run `./scripts/ensure_tools.sh` and `./scripts/prepare_comparison.sh`
once, then the complete local suite with `./runalltests.sh`.
Use `--list-stages` or `--stage host` to select a stage. The
[test guide](doc/en/testing.md) lists tools and platform limits.
It also describes [commit checks](doc/en/testing.md#commit-checks).
To work in VS Code, open the repository root and follow
[the VS Code guide](doc/en/vscode.md).

See [the API guide](doc/en/api.md) for memory, limits and error handling.
Optional Unicode normalization and character properties use
`JHBASE_UNICODE=ON` and `jb::unicode`; see [Unicode](doc/en/api.md#unicode).
JSON Pointer locates values and writes node paths through
[json_pointer.h](include/jb/json_pointer.h).
Host files, processes and owned JSON configuration trees use `JHBASE_HOST=ON`
and `jb::host`; see [host operations](doc/en/host.md).
JSON Schema remains outside this prototype.
The public ABI may still change; integrations should use a reviewed commit pin.
The [dependencies guide](doc/en/dependencies.md) covers preparation, tool pins,
Ryu, utf8proc and their licenses.

License: [JaszczurHAL Attribution License (MIT-based)](LICENSE).
