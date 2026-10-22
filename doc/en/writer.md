# Write JSON sequentially

A writer emits one compact JSON value to a synchronous callback. Include
[json_writer.h](../../include/jb/json_writer.h). Its state, container stack
and copied keys live in caller memory. No heap or mutable DOM
is required. The complete [writer example](../../examples/json_writer.c) is
built and run as both C11 and C++17.

## Initialize and finish

`jb_json_writer_workspace_size(limits, out_bytes)` computes the required byte
scratch, including all simultaneously open containers and keys. It checks size
overflow; both pointers are required. Scratch can be deliberately unaligned.

`jb_json_writer_init(writer, limits, work, sink, user)` starts a new document.
writer, limits and sink are required; user is passed unchanged to the sink.
Invalid arguments give `HAL_EINVAL`; insufficient scratch gives `HAL_EOVERFLOW`.
Failed init leaves an inactive handle holding that error. Reinitialization is
also how an application starts again after an error.

| Limit | Meaning |
| --- | --- |
| `max_output_bytes` | Total output bytes, without a terminator. |
| `max_depth` | Simultaneously open arrays and objects. |
| `max_keys` | Total keys in all currently open objects. |
| `max_key_bytes` | Total copied decoded UTF-8 bytes of those keys. |
| `max_key_compare_bytes` | Document-wide comparison budget: one per comparison, then both bytes actually compared. |

Zero always allows zero. Closing a container releases its key records and
bytes while preserving those of its parents. The comparison budget is not
reset by container close.

```c
static unsigned char scratch[512];
static char output[128];
const jb_json_writer_limits_t limits = {sizeof(output), 1, 3, 32, 128};
const jb_json_workspace_t work = {scratch, sizeof(scratch)};
jb_json_buffer_t buffer = {output, sizeof(output), 0};
jb_json_writer_t writer;
size_t needed;
hal_status_t status = jb_json_writer_workspace_size(&limits, &needed);
if (status != HAL_OK) {
    return status;
}
if (needed > work.size) {
    return HAL_EOVERFLOW;
}
status = jb_json_writer_init(&writer, &limits, work, jb_json_buffer_sink, &buffer);
```

`jb_json_writer_finish(writer, out_bytes)` requires exactly one complete root
and no open containers. Both pointers are required. An incomplete document
gives `HAL_ESTATE`. On success it returns the accepted byte count; repeated
finish returns the same result without calling the sink. Writing after finish
gives `HAL_ESTATE`.

## Containers and scalar values

| Function | Behavior |
| --- | --- |
| `jb_json_writer_object_begin(writer)` | Opens an object in a value position. |
| `jb_json_writer_object_end(writer)` | Closes the innermost object; a pending key without a value is an error. |
| `jb_json_writer_array_begin(writer)` | Opens an array in a value position. |
| `jb_json_writer_array_end(writer)` | Closes the innermost array; an object there is an error. |
| `jb_json_writer_key(writer, key)` | Copies a decoded UTF-8 name, verifies uniqueness in the current object and opens its value position. |
| `jb_json_writer_string(writer, value)` | Writes UTF-8 bytes with controls, quotes and backslashes escaped. |
| `jb_json_writer_bool(writer, value)` | Writes `true` or `false`. |
| `jb_json_writer_null(writer)` | Writes `null`. |
| `jb_json_writer_u64(writer, value)` | Writes the exact unsigned integer. |
| `jb_json_writer_i64(writer, value)` | Writes the exact signed integer, including `INT64_MIN`. |
| `jb_json_writer_number(writer, token)` | Validates one complete JSON number token and preserves its bytes. |
| `jb_json_writer_f32(writer, value)` | Writes a finite native binary32 using the shortest round-trip decimal. |
| `jb_json_writer_f64(writer, value)` | Writes a finite native binary64 using the same policy. |
| `jb_json_writer_node(writer, node)` | Iteratively writes a parsed subtree, applying the writer's limits. |

A value position is the root, an array item or the value of a preceding object
key. The writer inserts commas and colons. A second root, misplaced value/key
or mismatched end gives `HAL_ESTATE`. Limit exhaustion gives `HAL_EOVERFLOW`.

Keys and strings use spans, not terminated strings. `(NULL, 0)` means empty;
NUL within a span remains data. Invalid UTF-8 gives `HAL_EPROTO`. Keys are
compared by decoded bytes without NFC or case folding; duplicates give
`HAL_EEXIST`. A key's source buffer can be reused immediately after the call.

```c
const jb_json_span_t key = {"message", 7};
const jb_json_span_t value = {"a\0b", 3};
(void)jb_json_writer_object_begin(&writer);
(void)jb_json_writer_key(&writer, key);
(void)jb_json_writer_string(&writer, value);
(void)jb_json_writer_object_end(&writer);
size_t length;
status = jb_json_writer_finish(&writer, &length);
```

The output is `{"message":"a\u0000b"}`. Successful finish is mandatory even
after a successful last end operation. Checking only finish is valid because
each operation caches its first error; checking each operation is also valid.

Number-token input has no surrounding whitespace and no embedded NUL. Values
such as `1e400` are valid tokens even though a native floating conversion
would overflow. Malformed tokens give `HAL_EPROTO`. Floating writes reject
NaN/Infinity with `HAL_EINVAL` and preserve negative zero as `-0`. Their
round-trip output differs from the fixed-precision [C literal format](numbers.md).
The buffered Ryu backend uses fixed local storage and its size-optimized tables;
it needs no additional writer scratch. See [dependencies](dependencies.md).

For `writer_node`, keys and strings are decoded and escaped again, while raw
number tokens are preserved. Whitespace and original string escape spellings
are not preserved. The node's own member name is omitted; the caller supplies
an outer object key when needed. Depth limits include both the existing writer
context and the appended subtree. The document and its buffers remain alive
and unchanged throughout the call.

## Sinks and errors

`jb_json_sink_fn(user, data, length)` must synchronously accept the entire chunk
or return a defined negative HAL status. It must not retain data, reenter the
same writer or modify its scratch. `HAL_NONE` and unknown codes become
`HAL_EINTERNAL`. A defined error, including `HAL_EAGAIN`, is preserved.

| Sink | Behavior |
| --- | --- |
| `jb_json_buffer_sink` | Uses `jb_json_buffer_t` user. A chunk fits entirely or no part of it is copied; `used` counts accepted bytes. |
| `jb_json_count_sink` | Uses `size_t *` user. Counts bytes with overflow checking, without storing output. |

The first error is sticky: later operations return it without calling the sink.
The output may already contain a prefix from earlier accepted chunks. No write
is retried and there is no transport rollback. Build into a buffer and check
finish before sending when the application requires prior JSON validation.

A count pass validates the same operations and still needs scratch. A second
pass must use the same values, limits and order and check its own result;
successful counting does not guarantee successful transport.

Scratch, output, current input and any document being written must not overlap.
Do not copy an active handle: the copy would share mutable scratch. Independent
writers are safe with separate buffers and an appropriately synchronized sink.
