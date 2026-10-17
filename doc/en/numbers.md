# Exact numbers and C literals

Include [json_number.h](../../include/jb/json_number.h) for numeric reads and
[literal.h](../../include/jb/literal.h) for C output. Each function accepts
nodes from a successful parse; the original input and tokens stay alive and
unchanged. The parser retains every original number token, including values
outside native integer or floating ranges.

## Integer reads

| Function | Result |
| --- | --- |
| `jb_json_get_u64(node, out)` | Converts an exact integer to `uint64_t`. Negative nonzero values and values above `UINT64_MAX` give `HAL_EOVERFLOW`. |
| `jb_json_get_i64(node, out)` | Converts an exact integer to `int64_t`, including `INT64_MIN`; range overflow gives `HAL_EOVERFLOW`. |
| `jb_json_number_is_integer(node, out)` | Reports whether the decimal value is integral, even when it cannot fit a native integer. |

All result pointers are required. `HAL_ETYPE` means a wrong JSON kind or a
fractional value requested as an integer. `HAL_EINVAL` means bad arguments,
and `HAL_ESTATE` means an invalid node. Outputs change only on success.

`100`, `100.0` and `1e2` all convert to integer 100. `1.5` is rejected without
rounding. `-0`, including forms with enormous exponents, converts to zero.
There is no boolean or string coercion. These reads need no extra scratch;
their scan is bounded by the parser's number-token length limit.

```c
uint64_t identifier;
hal_status_t status = jb_json_get_u64(id_node, &identifier);
if (status != HAL_OK) {
    return status;
}
```

The complete [numeric example](../../examples/json_numbers.c) reads the full
unsigned 64-bit range without first converting it to `double`.

## Exact comparison

`jb_json_number_compare_workspace_size(left, right, out_bytes)` computes the
required scratch for these tokens. It checks size arithmetic before publishing
the result. `jb_json_number_compare(left, right, work, max_work, out_order)`
returns -1, 0 or 1 for the exact decimal ordering.

```c
size_t needed;
hal_status_t status = jb_json_number_compare_workspace_size(left, right, &needed);
if (status != HAL_OK) {
    return status;
}
if (needed > work.size) {
    return HAL_EOVERFLOW;
}
int order;
status = jb_json_number_compare(left, right, work, 1000000, &order);
```

The comparison distinguishes `9007199254740992.0` from `9007199254740993.0`,
and treats `1`, `1.0` and `1e0` as equal. Huge exponents are compared without
expanding their values into enormous buffers. Unaligned byte scratch is valid.
The workspace and output must be disjoint from each other and from both documents.
Insufficient scratch or exhausted work gives `HAL_EOVERFLOW`; zero permits no
work. Every digit/limb step is charged. `out_order` changes only on success.

## IEEE conversion

`jb_json_float_workspace_size(node, bit_width, out_bytes)` accepts widths 32 and
64 and computes scratch for the token. The four reads below all take
`(node, options, work, out)`, with required options and result pointers:

| Function | Result |
| --- | --- |
| `jb_json_get_f32_bits` | IEEE binary32 in a `uint32_t`, rounded directly from exact decimal. |
| `jb_json_get_f64_bits` | IEEE binary64 in a `uint64_t`. |
| `jb_json_get_f32` | Native `float` with the same binary32 result. |
| `jb_json_get_f64` | Native `double` with the same binary64 result. |

Rounding is nearest, ties to even, independent of locale and the active FPU
rounding mode. Native reads copy the representation after the build checks the
IEEE format. Subnormal values are supported. Overflow to infinity always gives
`HAL_EOVERFLOW`; outputs remain unchanged.

`jb_json_float_options_t` controls two independent zero policies and the finite
work budget:

| Field | Meaning |
| --- | --- |
| `reject_nonzero_to_zero` | Reject a nonzero decimal that rounds to zero. |
| `normalize_negative_zero` | Return positive zero for either signed zero. |
| `max_work` | Maximum digit/limb steps; zero permits none. |

```c
const jb_json_float_options_t options = {true, true, 1000000};
uint32_t bits;
hal_status_t status = jb_json_get_f32_bits(value_node, &options, work, &bits);
```

For `0.22`, binary32 bits are `3e6147ae`. A token slightly above the halfway
value, `1.0000000596046447753906250000000000000000001`, becomes `3f800001`.
There is no intermediate binary64 rounding. If underflow-to-zero is allowed,
zero keeps its sign unless normalization is also enabled.

## C literals

`jb_c_literal_workspace_size(node, type, out_bytes)` computes scratch; integer,
boolean and string literals need zero bytes. Floating literals share the exact
conversion and formatting scratch. `jb_c_literal(node, type, work, max_work,
buffer, capacity, required)` writes a non-terminated literal.

| Selector | C output |
| --- | --- |
| `JB_C_BOOL` | `1` or `0`; requires a JSON boolean. |
| `JB_C_U8`, `JB_C_U16`, `JB_C_U32` | Checked unsigned range and `U` suffix. |
| `JB_C_U64` | Full unsigned range and `ULL` suffix. |
| `JB_C_I8`, `JB_C_I16`, `JB_C_I32` | Checked signed range; negative values are parenthesized. |
| `JB_C_I64` | Checked signed range and `LL` suffix. |
| `JB_C_F32`, `JB_C_F64` | One exact IEEE rounding, then 9/17 significant digits in scientific notation. |
| `JB_C_STRING` | Quoted UTF-8 bytes; non-ASCII/control bytes use three-digit octal escapes. |

Signed minima use subtraction forms such as `(-2147483647 - 1)` and
`(-9223372036854775807LL - 1LL)`. Floating literals reject overflow and rounded
underflow-to-zero, normalize negative zero, and parenthesize negative values.
For binary32 `0.22`, the result is `2.19999999e-01f`.

String output preserves embedded NUL. Quotes, backslashes and question marks
are escaped; three-digit octal prevents a following digit from extending an
escape. No terminator is counted in `required`.

```c
char literal[48];
size_t required;
hal_status_t status = jb_c_literal(gain_node, JB_C_F32, work, 1000000,
                                  literal, sizeof(literal), &required);
```

`required` is mandatory. A small output buffer, including `(NULL, 0)`, reports
the complete required length and returns `HAL_EOVERFLOW` without writing.
Other failures preserve both outputs. Wrong kind/fraction gives `HAL_ETYPE`;
range, work or scratch exhaustion gives `HAL_EOVERFLOW`. The full numeric
example checks both the bits and the generated literal.
