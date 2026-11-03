# Strings, vectors and ordered maps in C

`jb/string.h`, `jb/vector.h` and `jb/map.h` provide owned containers through
C11 APIs. Link `jb::base`. They also work from C++17, but store bytes rather
than C++ objects with constructors or destructors.

## Memory and ownership

Initialize each owner with `{0}` in C or `{}` in C++, call its `init` function, and eventually call
`destroy`. Do not copy a live owner by assignment. `clear` retains options
and, for strings and vectors, reusable storage. A map releases its nodes.
`destroy` releases storage and resets the owner.
Separate owners can be used independently. Synchronize access to an owner
when any thread modifies it.

Initialization copies a required `jb_allocator_t`. Its callbacks and context
must outlive the owner. Allocations require alignment suitable for
`jb_max_align_t`; failures return `HAL_ENOMEM`. Size arithmetic is checked and
overflow returns `HAL_EOVERFLOW`. Failed mutations preserve existing contents.
Callbacks must not throw exceptions or reenter their owner.

The portable implementation never selects `malloc` implicitly. On Linux and
Windows, `jb_heap_allocator()` supplies `malloc/free` through `jb::filesystem`
or its consumer `jb::host`. An application may instead provide a bounded
allocator. This does not change the allocation-free UTF-8 and JSON parser APIs.

Containers copy element bytes. Pointers inside an element remain the caller's
responsibility; removing an element does not release the resources they name.
Use explicit ownership at that level, or store borrowed references whose
lifetime covers every use.

## Strings

`jb_string_t` keeps payload length and capacity separately from its trailing
NUL. Embedded NUL bytes are valid. Operations use byte offsets and do not
validate UTF-8 or count Unicode characters.

Use `assign`, `append`, `insert`, `erase`, `resize` and `reserve` to change the
contents. A source inside the same live string is supported, including its
terminator, but cannot extend into unused capacity. Internal insertion can
allocate even when capacity is sufficient. A successful mutation may
invalidate borrowed pointers.
Explicit `reserve` grows to the requested capacity without geometric rounding;
automatic growth during append/insert/resize can reserve extra space.

`jb_string_data` provides a terminated read-only view. `jb_string_mutable_data`
provides allocated storage for APIs that write into a previously resized
string. Preserve its length and terminating NUL. An empty string without an
allocation has no writable view. `jb_string_compare` compares bytes and length,
including embedded NULs.

## Vectors

`jb_vector_t` stores contiguous elements of one fixed size. Pass `sizeof(T)`
to `init`; `T` must be bytewise movable and require at most `jb_max_align_t`
alignment. Capacity and indexes count elements, not bytes.

The mutation operations follow the string model. `resize` zeroes newly added
bytes; the caller must establish a valid value if zero bytes do not represent
one for its element type. Mutable accessors keep the descriptor unchanged,
while allowing the pointed-to elements to change. Use the `const_` accessors
for read-only views.

`jb_vector_detach` transfers the allocation to the caller and leaves the
vector initialized but empty. Release that memory with its original allocator.
Skip the release callback for NULL. `jb_vector_shrink_to_fit` reduces capacity
to the live size, or releases an empty buffer. Allocation failure leaves the
vector unchanged. The file reader initially reserves the reported file size
exactly. If more bytes arrive, its buffer grows geometrically up to `max_bytes`.
Before returning, it removes unused capacity; that requires a copy when the
buffer is larger than the data. A stable file with an accurate size needs no
final copy.

## Ordered maps

`jb_map_t` copies fixed-size keys and values and orders keys with a comparator.
Lookup, insertion and erasure use O(log n) comparisons. Inserting an existing
key returns `HAL_EEXIST`; `upsert` replaces its value while retaining the
original key. Inserting or erasing other entries preserves borrowed entry
addresses. Erasing an entry invalidates its pointers and iterator.

Keys and any pointed-to data used by the comparator must remain unchanged
while stored. A map whose keys contain pointers to text borrows that text;
it does not copy the pointed-to characters. Iteration follows comparator
order, which differs from insertion order or hash-table order.

The [containers example](../../examples/containers.c) accumulates named samples
in a vector, aggregates them in a map and builds a sorted text report. The
example uses the host heap allocator and compiles as C11 or C++17.
