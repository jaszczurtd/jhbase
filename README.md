# jhbase

*Also available in [Polish](README.pl.md).*

jhbase provides a portable C11 UTF-8 decoder/encoder and an immutable JSON
parser, exact numeric reads, C literals and a sequential JSON writer. Callers
supply token storage and scratch; the parser preserves original number tokens.

This is a standalone prototype. It borrows the existing HAL status and array
headers; it does not configure or link HAL. Their hashes are in
[cmake/dependencies.cmake](cmake/dependencies.cmake). Test and gate tools
(Unity, cppcheck, PMD, the RISC-V and Xtensa toolchains) have their own pins in
`third_party/*_version.conf`; `./scripts/ensure_tools.sh` installs them below
`.build/tools`. Buffered float output uses the pinned Ryu subset; see
[dependencies and licenses](doc/en/dependencies.md).

Build with CMake 3.22 or later and a C11/C++17 compiler, after
`./scripts/ensure_tools.sh unity`:

```sh
cmake -S . -B .build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build/host
ctest --test-dir .build/host --output-on-failure
```

The default HAL directory is `../JaszczurHAL`. Pass
`-DJHBASE_HAL_ROOT=/path/to/JaszczurHAL` and, if needed,
`-DJHBASE_TOOLS_ROOT=/path/to/tools` to select explicit locations.
Configure downloads nothing and writes nothing to those directories.

On Linux, run `./scripts/ensure_tools.sh` and `./scripts/prepare_comparison.sh`
once, then the complete local suite with `./runalltests.sh`.
Use `--list-stages` or `--stage host` to select a stage. The
[test guide](doc/en/testing.md) lists tools and platform limits.
To work in VS Code, open the repository root: [the VS Code guide](doc/en/vscode.md)
lists the tasks, build profiles and IntelliSense refresh, driven by one CMake
script on every host.

Commits follow the JaszczurHAL rules: `./scripts/ensure_tools.sh` sets
`core.hooksPath` to the tracked `.githooks` (or run
`git config core.hooksPath .githooks` once). `commit-msg` requires a
Conventional Commits subject (`type(scope): subject`, at most 100 characters);
`pre-commit` normalizes staged text, runs clang-format on C/C++ and checks the
staged tree (owned-source whitespace, the Ryu manifest, shell syntax).

See [the API guide](doc/en/api.md) for memory, limits and error handling.
JSON Pointer, JSON Schema and NFC remain outside this prototype.
Its public ABI may change before the first integration pin.

License: [JaszczurHAL Attribution License (MIT-based)](LICENSE).
