# UTF-8 and JSON

jhbase validates UTF-8, decodes Unicode scalar values and reads JSON through
immutable nodes. The portable implementation uses C11 and caller-owned
storage. Include [utf8.h](../../include/jb/utf8.h) for text operations and
[json.h](../../include/jb/json.h) for JSON. Both headers also work in C++17.

For exact conversions and C literals, see [numbers](numbers.md).
For sequential output, see [the writer](writer.md). JSON Pointer, JSON Schema
and NFC remain outside this prototype. Build instructions and platform limits
are in [the test guide](testing.md).

## Common rules

Lengths and capacities are bytes unless a parameter explicitly counts tokens
or array items. A span does not imply a terminating NUL. UTF-8 and decoded
JSON strings can contain U+0000; use their lengths rather than `strlen()`.

Result pointers are required unless marked optional. Ordinary outputs remain
unchanged on failure. Exceptions are parser document invalidation, diagnostics
and required sizes reported by copy/encode functions. Do not overlap input,
token storage, scratch or result buffers.

Independent instances share no mutable state. Concurrent reads of an immutable
document are possible while its input, token array and document object remain
alive and unchanged. Parsing, buffer reuse and mutation require caller
synchronization. Examples with static buffers have a single owner.

## UTF-8

A Unicode scalar value is a code point from U+0000 to U+10FFFF, excluding
U+D800-U+DFFF surrogates. The functions reject overlong encodings, invalid
continuation bytes and out-of-range values. They do not normalize text or
count grapheme clusters.

### `jb_utf8_decode`

```c
hal_status_t jb_utf8_decode(const char *input, size_t length,
                           uint32_t *out_scalar, size_t *out_consumed);
```

Reads the first scalar from input and reports how many bytes it consumed,
from one through four. Bytes after that scalar are not validated. Advance
the pointer by `out_consumed` to read the next scalar.

`input` may be `NULL` only when length is zero. Both output pointers are
required. `HAL_OK` publishes both results. `HAL_EPROTO` means invalid or
incomplete UTF-8, including empty input. `HAL_EINVAL` means invalid pointers.
The function retains no state between calls; an incomplete sequence must be
provided again with its remaining bytes.

### `jb_utf8_encode`

```c
hal_status_t jb_utf8_encode(uint32_t scalar, char *buffer, size_t capacity,
                           size_t *required);
```

Encodes one scalar without a terminator. `required` receives its encoded byte
length and must not be `NULL`. With `buffer == NULL` and `capacity == 0`, the
function measures only and returns `HAL_OK`.

A real `buffer` must fit the entire encoding. `HAL_EOVERFLOW` reports the
required byte count without changing buffer. `HAL_EINVAL` rejects surrogates,
values above U+10FFFF, a missing required pointer or `NULL` buffer with
nonzero capacity.

### `jb_utf8_validate`

```c
hal_status_t jb_utf8_validate(const char *input, size_t length,
                             size_t *error_offset);
```

Validates exactly length bytes, including bytes after embedded NUL.
Unlike decode, it checks the whole span. Empty input, including (`NULL`, 0),
is valid.

`error_offset` is optional. `HAL_OK` sets it to `SIZE_MAX`. `HAL_EPROTO` sets it
to the lead-byte offset of the first invalid or incomplete sequence.
`HAL_EINVAL` rejects `NULL` input with nonzero length and leaves the offset
unchanged.

**Example: validate text and re-encode its first scalar**

For the bytes E2 82 AC followed by A, the result is the three-byte euro sign.
A bad suffix makes validation fail even if the first scalar is valid.
Empty input passes validation but decode then returns `HAL_EPROTO`.

```c
#include <jb/utf8.h>

/** Re-encode the first scalar after validating the entire input. */
hal_status_t example_first_scalar(const char *text, size_t length,
                                 char *output, size_t capacity,
                                 size_t *output_length, size_t *bad_offset) {
    hal_status_t status = jb_utf8_validate(text, length, bad_offset);
    if (status != HAL_OK) {
        return status;
    }

    uint32_t scalar;
    size_t consumed;
    status = jb_utf8_decode(text, length, &scalar, &consumed);
    if (status != HAL_OK) {
        return status;
    }

    size_t required;
    status = jb_utf8_encode(scalar, NULL, 0, &required);
    if (status != HAL_OK) {
        return status;
    }
    if (required > capacity) {
        return HAL_EOVERFLOW;
    }
    return jb_utf8_encode(scalar, output, capacity, output_length);
}
```

## Parsing

### Types and memory

| Type | Purpose |
| --- | --- |
| `jb_json_span_t` | Borrowed data pointer and length, without a terminator. |
| `jb_json_workspace_t` | Mutable scratch pointer and byte size. `NULL` is allowed only at zero size. |
| `jb_json_token_t` | Parser-owned record of one value, its source range and tree relations. |
| `jb_json_doc_t` | Immutable view of input and token storage; valid is set only by successful parse. |
| `jb_json_node_t` | Reference to a document object and token index. |
| `jb_json_limits_t` | Finite allowances for input, containers and comparison work. |
| `jb_json_requirements_t` | Value count and maximum open-container depth discovered by measure. |
| `jb_json_error_t` | Diagnostic reason, byte offset, related location and known required capacity. |

A scalar JSON value is null, a boolean, a number or a string.
A token represents a value. For example, `{"a": 1}` needs two tokens:
the object and its member value. The value also records the member name.
Tokens are in preorder; root has index zero. An absent parent, first child
or next sibling uses `JB_JSON_NODE_NONE`, equal to `SIZE_MAX`.

Token offset/length includes delimiters, such as string quotes and container
brackets. `key_offset`/`key_length` excludes name quotes. `has_key` distinguishes
an empty object name from a value with no name. Do not edit token fields.

Input and `tokens` must remain alive and unchanged while the document is used.
A node also borrows the document object itself: returning a node that points
to a local document is invalid. Reusing a token array invalidates all earlier
documents referring to it. There is no destroy function for borrowed storage.

Parser scratch may be reused immediately after parsing. It can be a static
`unsigned char` array at any alignment. Records are copied through `memcpy`;
scratch is not accessed by casting it to a struct pointer.

### Limits

Every field of `jb_json_limits_t` is a finite allowance. Zero always allows
zero work or data; it never means unlimited.

| Field | What it limits |
| --- | --- |
| `max_input_bytes` | Entire input, including leading and trailing whitespace. |
| `max_nodes` | Total value tokens, including root. |
| `max_depth` | Simultaneously open arrays and objects; a scalar root has depth zero. |
| `max_string_bytes` | Decoded UTF-8 bytes in one string or member name. |
| `max_number_bytes` | Bytes in one original number token. |
| `max_members` | Direct children of each array or object. |
| `max_key_compare_bytes` | Comparison work shared by all objects in this parse. |

Each key comparison costs one unit plus both decoded keys' bytes actually
compared. Comparing empty names still costs one unit. Duplicate detection
scans previous siblings and can be quadratic; the member and comparison
limits bound that work. Reads such as find have no separate work budget.

### `jb_json_parse_workspace_size`

```c
hal_status_t jb_json_parse_workspace_size(const jb_json_limits_t *limits,
                                         size_t *out_bytes);
```

Computes the scratch byte capacity required for `limits->max_depth`.
Both pointers are required. `HAL_OK` writes `out_bytes`; `HAL_EINVAL` rejects
missing pointers and `HAL_EOVERFLOW` reports size arithmetic overflow.

Supply this capacity to measure or parse even when a particular `input` is
shallower than the configured limit. With `max_depth == 0`, no scratch is
required and only scalar roots can be accepted. Token storage is separate.

### `jb_json_parse`

```c
hal_status_t jb_json_parse(jb_json_span_t input, const jb_json_limits_t *limits,
                          jb_json_token_t *tokens, size_t token_capacity,
                          jb_json_workspace_t work, jb_json_doc_t *out_doc,
                          jb_json_error_t *error);
```

Parses exactly one complete value and optional surrounding JSON whitespace.
A scalar root is accepted. Comments, trailing commas, BOM, NaN, Infinity,
invalid UTF-8, bad escapes, unpaired surrogates and trailing data fail.
Member names must be unique after escape decoding: `"a"` and `"\u0061"`
collide in the same object. Names are not normalized to NFC.

limits and `out_doc` are required; `error` is optional. `tokens` may be `NULL`
only at zero capacity, which cannot hold a valid document. `token_capacity`
counts records, not bytes: use `COUNTOF(tokens)` from the HAL array header.
Input and scratch allow `NULL` only at zero length/size.

`HAL_OK` publishes a valid document. `HAL_EPROTO` rejects the input,
`HAL_EOVERFLOW` reports a storage or policy limit and `HAL_EINVAL` rejects
invalid arguments. On every failure, `out_doc` becomes invalid and token
storage may contain partial records. Do not navigate that result.

**Example: read a boolean with fixed storage**

Pass `{"enabled": true}` and its byte length to get `true` in `out_enabled`.
A missing field returns `HAL_ENOENT`; `{"enabled": "true"}` returns `HAL_ETYPE`.
The helper returns only the copied boolean, so no borrowed node escapes.
The scratch size is checked against the implementation before parsing.

```c
#include <jb/json.h>
#include <jb/array.h>

/** Read a boolean field using fixed, single-owner parser storage. */
hal_status_t example_read_enabled(const char *data, size_t length,
                                 bool *out_enabled) {
    static jb_json_token_t tokens[16];
    static unsigned char scratch[1024];
    const jb_json_limits_t limits = {
        512, COUNTOF(tokens), 4, 128, 64, 8, 1024
    };
    if (data == NULL || out_enabled == NULL) {
        return HAL_EINVAL;
    }

    size_t scratch_bytes;
    hal_status_t status = jb_json_parse_workspace_size(&limits, &scratch_bytes);
    if (status != HAL_OK) {
        return status;
    }
    if (scratch_bytes > sizeof(scratch)) {
        return HAL_EOVERFLOW;
    }

    const jb_json_span_t input = {data, length};
    const jb_json_workspace_t work = {scratch, sizeof(scratch)};
    jb_json_doc_t doc;
    status = jb_json_parse(input, &limits, tokens, COUNTOF(tokens),
                           work, &doc, NULL);
    if (status != HAL_OK) {
        return status;
    }

    jb_json_node_t root;
    jb_json_node_t enabled = {NULL, 0}; /* Set only on success. */
    const jb_json_span_t key = {"enabled", 7};
    status = jb_json_root(&doc, &root);
    if (status == HAL_OK) {
        status = jb_json_find(root, key, &enabled);
    }
    if (status == HAL_OK) {
        status = jb_json_get_bool(enabled, out_enabled);
    }
    return status;
}
```

### `jb_json_measure`

```c
hal_status_t jb_json_measure(jb_json_span_t input, const jb_json_limits_t *limits,
                            jb_json_workspace_t work,
                            jb_json_requirements_t *out,
                            jb_json_error_t *error);
```

Checks syntax, Unicode and structural limits, then reports the required
value-token count and maximum depth. limits and `out` are required;
`error` is optional. Scratch requirements are the same as for parse.

Measurement does not check duplicate names or consume their comparison
budget. It is a sizing operation; parse must still validate the document.
`HAL_EINVAL`, `HAL_EPROTO` and `HAL_EOVERFLOW` have the same meanings as for
parse. The requirements object changes only on success.

**Example: measure and allocate tokens on a host**

This helper allocates only token storage; scratch remains fixed and has
one owner. It checks the multiplication before `malloc`, frees tokens after
use and publishes the count only after successful parse. A document with
duplicate names can pass measure and still fail parse. Firmware normally
uses a fixed token array as in the previous example.

```c
#include <jb/json.h>
#include <stdlib.h>

/** Host-only allocation: publish the count only after a complete parse. */
hal_status_t example_measured_parse(const char *data, size_t length,
                                   size_t *out_nodes) {
    static unsigned char scratch[1024];
    const jb_json_limits_t limits = {4096, 256, 8, 1024, 128, 64, 8192};
    if (out_nodes == NULL) {
        return HAL_EINVAL;
    }

    size_t scratch_bytes;
    hal_status_t status = jb_json_parse_workspace_size(&limits, &scratch_bytes);
    if (status != HAL_OK) {
        return status;
    }
    if (scratch_bytes > sizeof(scratch)) {
        return HAL_EOVERFLOW;
    }

    const jb_json_span_t input = {data, length};
    const jb_json_workspace_t work = {scratch, sizeof(scratch)};
    jb_json_requirements_t needed;
    status = jb_json_measure(input, &limits, work, &needed, NULL);
    if (status != HAL_OK) {
        return status;
    }
    if (needed.node_count > SIZE_MAX / sizeof(jb_json_token_t)) {
        return HAL_EOVERFLOW;
    }
    jb_json_token_t *tokens =
        (jb_json_token_t *)malloc(needed.node_count * sizeof(jb_json_token_t));
    if (tokens == NULL) {
        return HAL_ENOMEM;
    }

    jb_json_doc_t doc;
    status = jb_json_parse(input, &limits, tokens, needed.node_count,
                           work, &doc, NULL);
    if (status == HAL_OK) {
        *out_nodes = doc.token_count;
    }
    free(tokens);
    return status;
}
```

## Ownership and reads

The following functions read a successfully parsed document. Unless stated
otherwise, invalid documents or out-of-range node indices give `HAL_ESTATE`,
missing output pointers give `HAL_EINVAL`, and failed reads leave outputs
unchanged. Nodes belong to their document; token indices are not portable
between documents.

### `jb_json_root`

```c
hal_status_t jb_json_root(const jb_json_doc_t *doc, jb_json_node_t *out);
```

Returns the root node, including when root is a scalar. Both pointers are
required. An invalid or empty document gives `HAL_ESTATE`. Keep doc itself
alive for every later use of the returned node.

### `jb_json_type`

```c
hal_status_t jb_json_type(jb_json_node_t node, jb_json_type_t *out);
```

Reports one of `JB_JSON_NULL`, `JB_JSON_BOOL`, `JB_JSON_NUMBER`, `JB_JSON_STRING`,
`JB_JSON_ARRAY` or `JB_JSON_OBJECT`. `out` is required. Checking the type does
not convert the value or change the document.

### `jb_json_count`

```c
hal_status_t jb_json_count(jb_json_node_t node, size_t *out);
```

Reports direct members for an object or direct items for an array. It does
not count descendants. `out` is required. A scalar gives `HAL_ETYPE`;
an empty container succeeds with zero.

### `jb_json_first`

```c
hal_status_t jb_json_first(jb_json_node_t container, jb_json_node_t *out);
```

Returns the first member/item in source order. `out` is required.
An empty container returns `HAL_ENOENT`; a scalar returns `HAL_ETYPE`.

### `jb_json_next`

```c
hal_status_t jb_json_next(jb_json_node_t node, jb_json_node_t *out);
```

Returns the next sibling, skipping descendants of the current node.
`out` is required. End of siblings, including root with no sibling, gives
`HAL_ENOENT`. first/next traverse a container in linear total time.

### `jb_json_find`

```c
hal_status_t jb_json_find(jb_json_node_t object, jb_json_span_t key,
                         jb_json_node_t *out);
```

Finds an exact decoded, case-sensitive member name in an object.
`key` contains plain UTF-8 bytes, not JSON escape syntax or a path expression.
(`NULL`, 0) denotes an empty name. Embedded NUL is significant.
For example, a stored `"\u0061"` is found with the one-byte key `"a"`.

`out` is required. Missing names give `HAL_ENOENT`, a non-object gives
`HAL_ETYPE`, an invalid pointer/length gives `HAL_EINVAL`, and invalid key UTF-8 gives `HAL_EPROTO`.
Lookup scans members and compares names without NFC or case folding.
Repeated lookups over large objects should be bounded by the caller.

**Example: summarize a boolean array**

For `{"flags": [true, false, true]}`, this helper reports total 3 and `true`
count 2. For an empty array both are zero. A non-boolean item fails without
publishing either output. `HAL_ENOENT` from first/next ends iteration normally;
`HAL_ENOENT` from find means that the required `"flags"` `member` is missing.

```c
#include <jb/json.h>

/** Count all items and true values in the "flags" array. */
hal_status_t example_flags_summary(const jb_json_doc_t *doc,
                                  size_t *out_total, size_t *out_true) {
    if (out_total == NULL || out_true == NULL) {
        return HAL_EINVAL;
    }
    jb_json_node_t root;
    jb_json_node_t flags = {NULL, 0}, item = {NULL, 0};
    const jb_json_span_t key = {"flags", 5};
    hal_status_t status = jb_json_root(doc, &root);
    if (status == HAL_OK) {
        status = jb_json_find(root, key, &flags);
    }
    if (status != HAL_OK) {
        return status;
    }
    jb_json_type_t type;
    status = jb_json_type(flags, &type);
    if (status != HAL_OK) {
        return status;
    }
    if (type != JB_JSON_ARRAY) {
        return HAL_EINVAL;
    }

    size_t total, true_count = 0;
    status = jb_json_count(flags, &total);
    if (status != HAL_OK) {
        return status;
    }
    status = jb_json_first(flags, &item);
    while (status == HAL_OK) {
        bool value;
        status = jb_json_get_bool(item, &value);
        if (status != HAL_OK) {
            return status;
        }
        if (value) {
            ++true_count;
        }
        status = jb_json_next(item, &item);
    }
    if (status != HAL_ENOENT) {
        return status;
    }
    *out_total = total;
    *out_true = true_count;
    return HAL_OK;
}
```

### `jb_json_at`

```c
hal_status_t jb_json_at(jb_json_node_t array, size_t position,
                       jb_json_node_t *out);
```

Returns an array item by zero-based position. `out` is required.
An out-of-range position gives `HAL_ENOENT` and a non-array gives `HAL_ETYPE`.
The function scans earlier siblings, so it is linear in position.
Use first/next for a full traversal instead of repeated at calls.

**Example: select one boolean**

For `{"flags": [true, false]}`, position 1 returns `false` and position 2
returns `HAL_ENOENT`.

```c
#include <jb/json.h>

/** Read one zero-based element of the "flags" array. */
hal_status_t example_flag_at(const jb_json_doc_t *doc, size_t position,
                            bool *out_value) {
    jb_json_node_t root;
    jb_json_node_t flags = {NULL, 0}, item = {NULL, 0};
    const jb_json_span_t key = {"flags", 5};
    hal_status_t status = jb_json_root(doc, &root);
    if (status == HAL_OK) {
        status = jb_json_find(root, key, &flags);
    }
    if (status == HAL_OK) {
        status = jb_json_at(flags, position, &item);
    }
    if (status == HAL_OK) {
        status = jb_json_get_bool(item, out_value);
    }
    return status;
}
```

### `jb_json_raw`

```c
hal_status_t jb_json_raw(jb_json_node_t node, jb_json_span_t *out);
```

Borrows the original JSON bytes of any value, without surrounding whitespace.
`out` is required. A string span includes quotes and escapes; a container
span includes all of its contents and delimiters. This does not allocate,
decode or append a terminator. The span shares the input's lifetime.

### `jb_json_number_token`

```c
hal_status_t jb_json_number_token(jb_json_node_t node, jb_json_span_t *out);
```

Borrows the original spelling of a number. `out` is required. A non-number,
including a boolean or numeric string, gives `HAL_ETYPE`.

There is no integer or floating-point conversion. Tokens such as
18446744073709551615, 9007199254740993.0 and 1e400 remain exact original
bytes if they fit the configured input limits. The function does not decide
whether a token fits a particular C numeric type.

**Example: preserve a numeric identifier**

For `{"id": 1.8446744073709551615e19}`, this helper copies that exact spelling.
The output is length-based and has no terminator. The borrowed span is used
only while doc and its input remain alive.

```c
#include <jb/json.h>
#include <string.h>

/** Copy the original numeric spelling of "id", without a terminator. */
hal_status_t example_number_text(const jb_json_doc_t *doc, char *output,
                                size_t capacity, size_t *out_length) {
    if (output == NULL || out_length == NULL) {
        return HAL_EINVAL;
    }
    jb_json_node_t root;
    jb_json_node_t id = {NULL, 0};
    const jb_json_span_t key = {"id", 2};
    hal_status_t status = jb_json_root(doc, &root);
    if (status == HAL_OK) {
        status = jb_json_find(root, key, &id);
    }
    if (status != HAL_OK) {
        return status;
    }
    jb_json_span_t number;
    status = jb_json_number_token(id, &number);
    if (status != HAL_OK) {
        return status;
    }
    if (number.length > capacity) {
        return HAL_EOVERFLOW;
    }
    memcpy(output, number.data, number.length);
    *out_length = number.length;
    return HAL_OK;
}
```

### `jb_json_get_bool`

```c
hal_status_t jb_json_get_bool(jb_json_node_t node, bool *out);
```

Reads a JSON boolean into out, which is required. Only `true` and `false`
are accepted. Numbers, strings and `null` return `HAL_ETYPE`; no truth-value
coercion is performed. The parsing and array examples use this function.

### `jb_json_string_copy`

```c
hal_status_t jb_json_string_copy(jb_json_node_t node, char *buffer,
                                size_t capacity, size_t *required);
```

Decodes a string value into UTF-8 bytes, without quotes or a terminator.
Escapes and surrogate pairs are decoded; `"\u0000"` produces an embedded
NUL. `required` is mandatory and excludes any terminator.

(`NULL`, 0) measures and returns `HAL_OK`. A real buffer that is too small
returns `HAL_EOVERFLOW`, receives no partial string, and required reports
the full size. A non-string gives `HAL_ETYPE`. A missing required pointer
or `NULL` buffer with nonzero capacity gives `HAL_EINVAL`.

Do not print the result with `%s`; the decoded bytes can contain NUL and have
no terminator. `jb_json_string_cstr` below produces a C string when the value
allows one. Empty strings need zero data bytes; the `buffer` is not
automatically cleared.

### `jb_json_key_copy`

```c
hal_status_t jb_json_key_copy(jb_json_node_t member, char *buffer,
                             size_t capacity, size_t *required);
```

Decodes a member name using the same capacity, measurement and NUL rules as
string_copy. `required` is mandatory. The member value can have any JSON
type; it only needs to have an object key. A root or array item has no key
and gives `HAL_EINVAL`. An empty member name is valid and requires zero bytes.

**Example: compare original JSON and decoded text**

For `{"label": "Hi\n"}`, raw includes the quotes and the two-byte escape,
while string_copy produces H, i and LF. The host-only example prints all
spans with explicit byte counts, so `"\u0000"` would not truncate the output.
Its local buffers bound decoded names to 32 bytes and values to 64 bytes;
larger results give `HAL_EOVERFLOW` before anything is printed.

```c
#include <jb/json.h>
#include <stdio.h>

/** Host-only display of the key, original JSON and decoded label bytes. */
hal_status_t example_print_label(const jb_json_doc_t *doc) {
    jb_json_node_t root;
    jb_json_node_t label = {NULL, 0};
    const jb_json_span_t key = {"label", 5};
    hal_status_t status = jb_json_root(doc, &root);
    if (status == HAL_OK) {
        status = jb_json_find(root, key, &label);
    }
    if (status != HAL_OK) {
        return status;
    }

    jb_json_span_t raw;
    char name[32], value[64];
    size_t name_bytes = 0, value_bytes = 0;
    status = jb_json_raw(label, &raw);
    if (status == HAL_OK) {
        status = jb_json_key_copy(label, name, sizeof(name), &name_bytes);
    }
    if (status == HAL_OK) {
        status = jb_json_string_copy(label, NULL, 0, &value_bytes);
    }
    if (status == HAL_OK) {
        status = jb_json_string_copy(label, value, sizeof(value), &value_bytes);
    }
    if (status != HAL_OK) {
        return status;
    }

    if (fwrite(name, 1, name_bytes, stdout) != name_bytes ||
        fputs(": ", stdout) == EOF ||
        fwrite(raw.data, 1, raw.length, stdout) != raw.length ||
        fputs(" -> ", stdout) == EOF ||
        fwrite(value, 1, value_bytes, stdout) != value_bytes) {
        return HAL_EIO;
    }
    return HAL_OK;
}
```

### `jb_json_string_equal`

```c
hal_status_t jb_json_string_equal(jb_json_node_t node, jb_json_span_t value,
                                 bool *out);
```

Compares a string value with plain UTF-8 bytes after decoding its escapes
and surrogate pairs, without copying, NFC or case folding. `value` is not
JSON syntax: a stored `"a"` equals the one-byte value `a`. It may
contain NUL, and (`NULL`, 0) is the empty string. `out` is required and
receives `true` only for identical decoded bytes.

A non-string gives `HAL_ETYPE`. Invalid UTF-8 anywhere in `value` gives
`HAL_EPROTO`, even after an earlier difference. A `NULL` `value.data` with
nonzero length or a missing `out` gives `HAL_EINVAL`. `out` is unchanged on
failure.

### `jb_json_string_cstr`

```c
hal_status_t jb_json_string_cstr(jb_json_node_t node, char *buffer,
                                size_t capacity, size_t *required);
```

Decodes a string value and appends a NUL terminator. `required` is mandatory
and includes the terminator, so an empty string requires one byte. (`NULL`, 0)
measures and returns `HAL_OK`. A real buffer that is too small gives
`HAL_EOVERFLOW`, receives nothing, and `required` reports the full size.

A value containing U+0000 cannot be a C string. It gives `HAL_ETYPE` and
leaves both outputs unchanged instead of truncating silently; read such data
with `jb_json_string_copy`. A non-string also gives `HAL_ETYPE`. A missing
`required` pointer or `NULL` `buffer` with nonzero capacity gives `HAL_EINVAL`.

## Results and ABI

### Operation results

| Status | Meaning |
| --- | --- |
| `HAL_OK` | Operation completed and its outputs can be used. |
| `HAL_EINVAL` | Invalid arguments. |
| `HAL_ETYPE` | A read of the wrong JSON type or a value without the requested exact representation. |
| `HAL_EPROTO` | Malformed JSON, invalid UTF-8/escapes, duplicate names or trailing data. |
| `HAL_EOVERFLOW` | Capacity, configured work/data limit or size arithmetic overflow. |
| `HAL_ESTATE` | Invalid document or node. |
| `HAL_ENOENT` | Missing member/item or end of iteration. |

All results use the existing `hal_status_t` from
[status.h](../../include/jb/status.h). `HAL_ETYPE` comes from that shared
header; the prototype defines no result codes of its own.
The host allocation example can also return `HAL_ENOMEM`; the display examples
can return `HAL_EIO`. Those are errors of the example's allocation/output
operations, not allocations or I/O performed by the portable parser.

### Parser diagnostics

`error` may be `NULL` when only the status is needed. Otherwise parse and
measure fill the following fields:

| Field | Meaning |
| --- | --- |
| `code` | One of the diagnostic reasons below; `JB_JSON_ERROR_NONE` on success or argument failure. |
| `offset` | Input byte offset. EOF uses `input.length`. A duplicate points to its later opening quote. |
| `related_offset` | Earlier key's opening quote for a duplicate; otherwise `SIZE_MAX`. |
| `required_capacity` | Known required token count or scratch bytes; zero when unknown. |

| Reason | Meaning |
| --- | --- |
| `JB_JSON_ERROR_NONE` | No parser diagnostic. |
| `JB_JSON_ERROR_SYNTAX` | Invalid JSON grammar, literal or number. |
| `JB_JSON_ERROR_UTF8` | Invalid UTF-8 within a string or name. |
| `JB_JSON_ERROR_ESCAPE` | Bad escape or surrogate sequence. |
| `JB_JSON_ERROR_DUPLICATE_KEY` | Two decoded names collide within one object. |
| `JB_JSON_ERROR_TRAILING_DATA` | A complete root is followed by non-whitespace data. |
| `JB_JSON_ERROR_LIMIT` | A configured data/work allowance was exceeded. |
| `JB_JSON_ERROR_TOKEN_CAPACITY` | The token array cannot hold the next value. |
| `JB_JSON_ERROR_WORKSPACE` | Insufficient scratch or overflow in its size calculation. |

`required_capacity` is not always the final document size. A token-capacity
error can report only the next required record count. Use measure to size
the whole token array and parse to accept the document.

### `jb_json_locate`

```c
hal_status_t jb_json_locate(jb_json_span_t input, size_t offset,
                           size_t *out_line, size_t *out_column);
```

Converts a byte offset, including EOF, to a one-based line and byte column.
Only LF starts a new line; CR counts as a byte. Columns are not display
positions or Unicode character counts. The operation scans the prefix
without requiring valid JSON or valid UTF-8.

Both outputs are required. `HAL_EINVAL` rejects an offset beyond input length,
`NULL` input with nonzero length or missing outputs. Counter overflow gives
`HAL_EOVERFLOW`. Failed calls leave both outputs unchanged.

**Example: report the location of malformed JSON**

The trailing comma below is followed by a closing brace at line 3, byte
column 1. The helper prints that location and returns the original
`HAL_EPROTO` status. For duplicate-key diagnostics, `related_offset` can be
passed to locate separately.

```c
#include <jb/json.h>
#include <jb/array.h>
#include <stdio.h>

/** Show a byte location for a trailing-comma error. */
hal_status_t example_json_error(void) {
    static const char text[] = "{\n  \"x\": 1,\n}";
    static jb_json_token_t tokens[8];
    static unsigned char scratch[1024];
    const jb_json_limits_t limits = {
        sizeof(text) - 1, COUNTOF(tokens), 2, 16, 16, 4, 64
    };
    size_t needed;
    hal_status_t status = jb_json_parse_workspace_size(&limits, &needed);
    if (status != HAL_OK) {
        return status;
    }
    if (needed > sizeof(scratch)) {
        return HAL_EOVERFLOW;
    }

    const jb_json_span_t input = {text, sizeof(text) - 1};
    const jb_json_workspace_t work = {scratch, sizeof(scratch)};
    jb_json_doc_t doc;
    jb_json_error_t error;
    const hal_status_t parsed = jb_json_parse(
        input, &limits, tokens, COUNTOF(tokens), work, &doc, &error);
    if (parsed != HAL_OK && error.code != JB_JSON_ERROR_NONE) {
        size_t line, column;
        status = jb_json_locate(input, error.offset, &line, &column);
        if (status != HAL_OK) {
            return status;
        }
        if (printf("JSON error %u at %zu:%zu\n",
                   (unsigned)error.code, line, column) < 0) {
            return HAL_EIO;
        }
    }
    return parsed;
}
```

### Layout and build profile

The prototype uses natural layouts with 32/64-bit pointers and `size_t`,
one-byte `bool` and a 32-bit status enum. `jb/status.h` keeps the status at 32
bits on every compiler, also under `-fshort-enums`, without changing other
enum types or the toolchain's global ABI attributes.

Header assertions reject incompatible packing, including packing opened
before the first include. Tokens occupy 40 bytes on the 32-bit profile and
80 on the 64-bit profile. These structs are not disk or wire formats.

Host modules may use C++17 and heap behind C APIs; every exception must be
translated to a HAL status before crossing a C boundary. The current portable
UTF-8 and JSON modules allocate no memory.
