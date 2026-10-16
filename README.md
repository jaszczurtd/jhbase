# jhbase

*Also available in [Polish](README.pl.md).*

jhbase provides a portable C11 UTF-8 decoder/encoder and an immutable JSON
parser. Callers supply token storage and scratch; the parser preserves number
tokens without converting them to floating point.

This is a standalone prototype. It borrows the existing HAL status and array
headers and a pinned Unity checkout for tests. It does not configure or link HAL.
The CMake paths and hashes are in [cmake/dependencies.cmake](cmake/dependencies.cmake).

Build with CMake 3.22 or later and a C11/C++17 compiler:

```sh
cmake -S . -B .build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build/host
ctest --test-dir .build/host --output-on-failure
```

The default dependency directory is `../libraries/JaszczurHAL`. Pass
`-DJHBASE_HAL_ROOT=/path/to/JaszczurHAL` and, if needed,
`-DJHBASE_UNITY_ROOT=/path/to/Unity` to select explicit locations.
Nothing is downloaded or written to those directories.

On Linux, run the complete basic acceptance suite with `./runalltests.sh`.
Use `--list-stages` or `--stage host` to select a stage. The
[test guide](doc/en/testing.md) lists tools and platform limits.

See [the API guide](doc/en/api.md) for memory, limits and error handling.
Numeric conversion, a JSON writer, JSON Schema and NFC remain outside this
prototype. Its public ABI may change before the first integration pin.

License: [JaszczurHAL Attribution License (MIT-based)](LICENSE).
