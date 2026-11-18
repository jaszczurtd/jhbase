# Byte order and SHA-256

[endian.h](../../include/jb/endian.h) reads and writes 16-, 32- and 64-bit
integers in byte buffers, in little-endian or big-endian order.
[sha256.h](../../include/jb/sha256.h) computes SHA-256 digests (FIPS 180-4)
from data that arrives in pieces. Both are portable C11 code, work from C++17
and allocate no memory.

## Byte order

The load functions (`jb_load_le16`, `jb_load_le32`, `jb_load_le64` and their
`be` versions) and the store functions (`jb_store_le16` and so on) access the
buffer one byte at a time. The buffer may start at any address, and the result
does not depend on the byte order of the host. A store writes exactly two, four
or eight bytes and leaves the neighbouring bytes alone. Pointers are not
checked: the caller provides as many bytes as the width needs.

`jb_bswap16`, `jb_bswap32` and `jb_bswap64` reverse the byte order of a value.
`jb_u16_msb` and `jb_u16_lsb` return the high and the low byte of a 16-bit
value, and `jb_u16_from_bytes(msb, lsb)` joins two bytes again.

```c
uint8_t frame[6];
jb_store_be16(frame, 0x1234u);         /* 12 34 */
jb_store_le32(frame + 2, 0xA1B2C3D4u); /* D4 C3 B2 A1 */
uint32_t value = jb_load_le32(frame + 2); /* 0xA1B2C3D4 */
```

All helpers are `static inline` functions in the header, so they need no
library and keep no state.

## SHA-256

`jb_sha256_init()` starts a digest, `jb_sha256_update()` adds the next piece
of data and `jb_sha256_final()` writes the 32-byte result. Pieces of any size
give the same digest as one call with all data. After `jb_sha256_final()` the
context is finished; start it again before the next digest.

```c
jb_sha256_context_t sha;
uint8_t digest[JB_SHA256_DIGEST_BYTES];
jb_sha256_init(&sha);
jb_sha256_update(&sha, header, header_length);
jb_sha256_update(&sha, payload, payload_length);
jb_sha256_final(&sha, digest);
```

The functions do not validate their arguments. `data` may be NULL only when
`length` is zero, and a call with zero length leaves the context unchanged.
Wrappers that report errors, such as `hal_sha256_update_ex()` in JaszczurHAL,
check the arguments first and use the `initialized` field, which is true from
`jb_sha256_init()` until `jb_sha256_final()`.

`jb_sha256_context_t` has a fixed layout: 112 bytes with 32-bit pointers and
120 bytes with 64-bit pointers, aligned like `uint64_t`. The header does not
compile when `#pragma pack` or a compiler flag changes the offsets, the size
or the alignment. Firmware can keep the context in a static variable.
Treat the other fields as private. Separate contexts are independent; access to
one context from several threads needs the caller's synchronization.

## Firmware builds

Both modules are part of `jb::base`. A firmware library that does not link
`jb::base` can compile the SHA-256 core into its own archive:
[sources.cmake](../../cmake/sources.cmake) sets `JHBASE_SHA256_SOURCES` to its
source files. JaszczurHAL builds the core into each HAL library this way. The
status headers of the installed package include `endian.h` and `sha256.h`, so
a package that ships them with its own archive stays complete
(see [the installed package](dependencies.md#installed-package)).

JaszczurHAL keeps its names as aliases of these definitions:
`hal/core/jh_endian.h` maps the `jh_*` helpers and `MSB()`/`LSB()` to the
`jb_*` functions, and `hal_sha256_context_t` is `jb_sha256_context_t`.
