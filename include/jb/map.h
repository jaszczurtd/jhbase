#ifndef JB_MAP_H
#define JB_MAP_H

/** @file map.h
 * Ordered C map with fixed-size keys and values, linked with jb::base.
 * Each entry owns byte copies of its key and value; pointers inside those
 * copies remain caller-owned. The map never calls element destructors.
 * Only types that can be copied as bytes and need at most jb_max_align_t
 * alignment are supported. C++ objects with constructors are not supported.
 *
 * Insert, lookup and erase use O(log n) comparisons. Entry addresses remain
 * stable while other entries are inserted or erased. Upsert overwrites the
 * existing value in place and preserves the original key. Erase invalidates
 * only the erased entry; clear/destroy invalidate every borrowed pointer and
 * iterator. The owner must not be copied or moved while in use. Separate maps
 * may be used concurrently; access to the same map requires caller locking.
 */
#include "jb/allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Compare two keys: negative, zero or positive for less, equal or greater.
 * The callback and context must remain valid until destruction, must define
 * a consistent ordering with zero for equivalent keys, and must not mutate/reenter the
 * map or throw a C++ exception. Fields and pointed-to data used for ordering
 * must remain unchanged while the key belongs to the map.
 */
typedef int (*jb_map_compare_fn)(const void *left, const void *right, void *context);

/** Private node type, exposed only to describe the owner and iterator. */
struct jb_map_node;

/** Single owner. Initialize with {0} in C or {} in C++, then call jb_map_init; fields are private.
 * The allocator is copied. Its callbacks and context must outlive the map.
 */
typedef struct {
  struct jb_map_node *root;
  size_t size, key_size, value_size;
  size_t key_offset, value_offset, node_bytes;
  jb_map_compare_fn compare;
  void *compare_context;
  jb_allocator_t allocator;
} jb_map_t;

/** Borrowed position, invalidated by erasing its entry or clearing the map.
 * Obtain it with jb_map_first. Do not alter its fields or use it after its
 * owner is destroyed. Iteration may observe entries inserted between steps.
 */
typedef struct {
  const jb_map_t *owner;
  const struct jb_map_node *node;
} jb_map_iter_t;

JB_STATIC_ASSERT(JB_ALIGNOF(jb_map_t) == sizeof(size_t) &&
                     offsetof(jb_map_t, size) == sizeof(void *) &&
                     offsetof(jb_map_t, allocator) % sizeof(void *) == 0,
                 "jb requires a naturally aligned map owner");
JB_STATIC_ASSERT(sizeof(jb_map_iter_t) == 2 * sizeof(void *) &&
                     JB_ALIGNOF(jb_map_iter_t) == sizeof(void *) &&
                     offsetof(jb_map_iter_t, node) == sizeof(void *),
                 "jb requires a naturally aligned map iterator");

/** Initialize an empty map without allocating.
 * @param map Required zero-initialized or previously destroyed owner.
 * @param key_size Size in bytes of one key; must be nonzero.
 * @param value_size Size in bytes of one value; must be nonzero.
 * @param compare Required comparison callback.
 * @param context Optional comparison context, retained until destruction.
 * @param allocator Required allocator with both callbacks set.
 * @return HAL_OK, HAL_EINVAL for bad arguments, HAL_ESTATE for an occupied
 * owner, or HAL_EOVERFLOW if entry sizes cannot be represented. On failure
 * the owner is unchanged. No default heap allocation is used.
 */
hal_status_t jb_map_init(jb_map_t *map, size_t key_size, size_t value_size,
                         jb_map_compare_fn compare, void *context, const jb_allocator_t *allocator);

/** Insert copied key/value bytes. Both inputs are required and may point
 * into this map. Returns HAL_EEXIST for an equivalent key, HAL_ENOMEM when
 * allocation fails, HAL_EOVERFLOW at the size limit, HAL_EINVAL for NULL
 * arguments, or HAL_ESTATE for an uninitialized map. Failure changes nothing.
 */
hal_status_t jb_map_insert(jb_map_t *map, const void *key, const void *value);

/** Insert or replace value bytes. An equivalent key retains its original
 * bytes and address. Replacement accepts overlapping input and allocates
 * nothing. Statuses and failure behavior otherwise match jb_map_insert.
 */
hal_status_t jb_map_upsert(jb_map_t *map, const void *key, const void *value);

/** Find a mutable borrowed value. map/key/value are required. Returns HAL_OK,
 * HAL_ENOENT, HAL_EINVAL or HAL_ESTATE; *value changes only on success.
 * The const owner describes a lookup; it does not make its elements read-only.
 * Call jb_map_find_const when the returned value must remain read-only.
 * Writing the value must not change any data used for key comparison.
 */
hal_status_t jb_map_find(const jb_map_t *map, const void *key, void **value);

/** Find a read-only borrowed value, with the same rules as jb_map_find. */
hal_status_t jb_map_find_const(const jb_map_t *map, const void *key, const void **value);

/** Erase one equivalent key, without freeing resources pointed to by its
 * copied bytes. Returns HAL_OK, HAL_ENOENT, HAL_EINVAL or HAL_ESTATE.
 * Key may point at the entry being erased. Other entry addresses stay valid.
 */
hal_status_t jb_map_erase(jb_map_t *map, const void *key);

/** Number of entries; NULL and an owner initialized to {0} return zero. */
size_t jb_map_size(const jb_map_t *map);

/** Release all entries, preserving initialized options for reuse.
 * NULL or an owner initialized to {0} is harmless. Does not free resources
 * pointed to by the copied key/value bytes.
 */
void jb_map_clear(jb_map_t *map);

/** Release all entries and reset the owner to {0}; NULL is harmless. */
void jb_map_destroy(jb_map_t *map);

/** Position at the smallest key. map/iterator are required. Returns HAL_OK,
 * HAL_ENOENT for an empty map, HAL_EINVAL or HAL_ESTATE. On failure the
 * supplied iterator is unchanged.
 */
hal_status_t jb_map_first(const jb_map_t *map, jb_map_iter_t *iterator);

/** Advance in key order. HAL_ENOENT sets node to NULL after the last entry
 * and also describes an already exhausted iterator. NULL gives HAL_EINVAL;
 * an uninitialized owner gives HAL_ESTATE. An erased position is invalid
 * and must not be passed here.
 */
hal_status_t jb_map_next(jb_map_iter_t *iterator);

/** Borrow the current key/value. iterator/key/value are required. Returns
 * HAL_OK, HAL_ENOENT at end, HAL_EINVAL or HAL_ESTATE. Outputs change only
 * on success; an erased position must not be passed here.
 */
hal_status_t jb_map_iter_get(const jb_map_iter_t *iterator, const void **key, const void **value);

#ifdef __cplusplus
}
#endif
#endif
