# Parser selection

The jhbase token parser reads JSON in caller-owned storage with explicit
memory and work limits. Its C API follows the ABI policy in
the [README](../../README.md).

The required behavior combines explicit byte lengths, embedded NUL, raw numeric
tokens, decoded duplicate-key rejection, original source ranges, offset
diagnostics, caller storage and finite depth/data/comparison budgets. Parsing
and writing share UTF-8 and string decoding; numeric conversion consumes the
original decimal token.

## Compared snapshots

| Candidate | Revision | Observed difference from the required behavior |
| --- | --- | --- |
| [coreJSON v3.3.0](https://github.com/FreeRTOS/coreJSON/tree/e3b7663f6392d8c10e8db57506ec37e4801b145a) | `e3b7663f6392d8c10e8db57506ec37e4801b145a` | Rejects an otherwise valid string containing `\u0000`; accepts repeated plain and escape-equivalent keys. |
| [yyjson 0.12.0](https://github.com/ibireme/yyjson/tree/8b4a38dc994a110abaec8a400615567bd996105f) | `8b4a38dc994a110abaec8a400615567bd996105f` | A pool-backed DOM with raw numbers preserves NUL, but accepts duplicate keys; its representation differs from the source-range token model. |

Both candidates use MIT licenses. They are research snapshots, fetched below
`.build` and excluded from `jb::base`. No upstream source is patched. Configure
verifies the source/header hashes before compiling the comparison.

The comparison uses native `JSON_Validate` and `yyjson_read_opts` with
`YYJSON_READ_NUMBER_AS_RAW` and an explicit pool. It also checks malformed UTF-8,
surrogate pairs, trailing data after NUL, number grammar and depth limits.
Reproduce it with:

```sh
./scripts/prepare_comparison.sh
./runalltests.sh --stage comparison
```

## Why keep the existing core

coreJSON would require changes to NUL handling and additional token construction,
decoded-key checks, diagnostics and work accounting. yyjson would need an
adapter for the required immutable source-range records and limit accounting,
plus duplicate-key checks. The existing core already implements those parts
using the same caller-memory model as the writer and numeric modules.

The benchmark compares different operations: full jhbase token parsing with
duplicate checks, coreJSON validation only, and yyjson pool-backed DOM parsing.
Its timing is diagnostic, not a claim that the APIs or guarantees are equal.
Raw measurements, pool bounds and stack reports remain local build artifacts.
Object sizes and per-function stack estimates are not linked firmware size or
runtime peak stack. On-device measurements and native MSVC remain separate
acceptance requirements.
