#include "jb/map.h"
#include "container_internal.h"
#include <string.h>

struct jb_map_node {
  struct jb_map_node *left, *right, *parent;
  int height;
};

static bool ready(const jb_map_t *map) {
  return map != NULL && map->compare != NULL && jb_container_allocator_valid(&map->allocator);
}

static const void *node_key(const jb_map_t *map, const struct jb_map_node *node) {
  return (const unsigned char *)node + map->key_offset;
}

static void *node_value(const jb_map_t *map, struct jb_map_node *node) {
  return (unsigned char *)node + map->value_offset;
}

static const void *node_value_const(const jb_map_t *map, const struct jb_map_node *node) {
  return (const unsigned char *)node + map->value_offset;
}

static bool aligned_size(size_t size, size_t *result) {
  const size_t alignment = JB_ALIGNOF(jb_max_align_t);
  const size_t padding = (alignment - size % alignment) % alignment;
  if (size > SIZE_MAX - padding)
    return false;
  *result = size + padding;
  return true;
}

hal_status_t jb_map_init(jb_map_t *map, size_t key_size, size_t value_size,
                         jb_map_compare_fn compare, void *context,
                         const jb_allocator_t *allocator) {
  if (map == NULL || key_size == 0 || value_size == 0 || compare == NULL ||
      !jb_container_allocator_valid(allocator))
    return HAL_EINVAL;
  if (map->root != NULL || map->size != 0 || map->compare != NULL || map->key_size != 0 ||
      map->value_size != 0 || map->key_offset != 0 || map->value_offset != 0 ||
      map->node_bytes != 0 || map->compare_context != NULL ||
      !jb_container_allocator_zero(&map->allocator))
    return HAL_ESTATE;
  size_t key_offset, key_bytes;
  if (!aligned_size(sizeof(struct jb_map_node), &key_offset) ||
      !aligned_size(key_size, &key_bytes) || key_bytes > SIZE_MAX - key_offset)
    return HAL_EOVERFLOW;
  const size_t value_offset = key_offset + key_bytes;
  if (value_size > SIZE_MAX - value_offset)
    return HAL_EOVERFLOW;
  const jb_map_t result = {
      NULL,    0,       key_size,  value_size, key_offset, value_offset, value_offset + value_size,
      compare, context, *allocator};
  *map = result;
  return HAL_OK;
}

static struct jb_map_node *search(const jb_map_t *map, const void *key, struct jb_map_node **parent,
                                  int *order) {
  struct jb_map_node *node = map->root;
  *parent = NULL;
  *order = 0;
  while (node != NULL) {
    *order = map->compare(key, node_key(map, node), map->compare_context);
    if (*order == 0)
      return node;
    *parent = node;
    node = *order < 0 ? node->left : node->right;
  }
  return NULL;
}

static int height(const struct jb_map_node *node) { return node == NULL ? 0 : node->height; }

static void update_height(struct jb_map_node *node) {
  const int left = height(node->left), right = height(node->right);
  node->height = 1 + (left > right ? left : right);
}

static int balance(const struct jb_map_node *node) {
  return height(node->left) - height(node->right);
}

static void replace(jb_map_t *map, struct jb_map_node *old, struct jb_map_node *replacement) {
  if (old->parent == NULL)
    map->root = replacement;
  else if (old->parent->left == old)
    old->parent->left = replacement;
  else
    old->parent->right = replacement;
  if (replacement != NULL)
    replacement->parent = old->parent;
}

static struct jb_map_node *rotate_left(jb_map_t *map, struct jb_map_node *node) {
  struct jb_map_node *top = node->right;
  replace(map, node, top);
  node->right = top->left;
  if (node->right != NULL)
    node->right->parent = node;
  top->left = node;
  node->parent = top;
  update_height(node);
  update_height(top);
  return top;
}

static struct jb_map_node *rotate_right(jb_map_t *map, struct jb_map_node *node) {
  struct jb_map_node *top = node->left;
  replace(map, node, top);
  node->left = top->right;
  if (node->left != NULL)
    node->left->parent = node;
  top->right = node;
  node->parent = top;
  update_height(node);
  update_height(top);
  return top;
}

static void rebalance(jb_map_t *map, struct jb_map_node *node) {
  while (node != NULL) {
    update_height(node);
    const int difference = balance(node);
    if (difference > 1) {
      if (balance(node->left) < 0)
        (void)rotate_left(map, node->left);
      node = rotate_right(map, node);
    } else if (difference < -1) {
      if (balance(node->right) > 0)
        (void)rotate_right(map, node->right);
      node = rotate_left(map, node);
    }
    node = node->parent;
  }
}

static hal_status_t store(jb_map_t *map, const void *key, const void *value, bool overwrite) {
  if (map == NULL || key == NULL || value == NULL)
    return HAL_EINVAL;
  if (!ready(map))
    return HAL_ESTATE;
  struct jb_map_node *parent;
  int order;
  struct jb_map_node *node = search(map, key, &parent, &order);
  if (node != NULL) {
    if (!overwrite)
      return HAL_EEXIST;
    memmove(node_value(map, node), value, map->value_size);
    return HAL_OK;
  }
  if (map->size == SIZE_MAX)
    return HAL_EOVERFLOW;
  node = map->allocator.allocate(map->allocator.context, map->node_bytes);
  if (node == NULL)
    return HAL_ENOMEM;
  *node = (struct jb_map_node){NULL, NULL, parent, 1};
  memcpy((unsigned char *)node + map->key_offset, key, map->key_size);
  memcpy(node_value(map, node), value, map->value_size);
  if (parent == NULL)
    map->root = node;
  else if (order < 0)
    parent->left = node;
  else
    parent->right = node;
  ++map->size;
  rebalance(map, parent);
  return HAL_OK;
}

hal_status_t jb_map_insert(jb_map_t *map, const void *key, const void *value) {
  return store(map, key, value, false);
}

hal_status_t jb_map_upsert(jb_map_t *map, const void *key, const void *value) {
  return store(map, key, value, true);
}

hal_status_t jb_map_find_const(const jb_map_t *map, const void *key, const void **value) {
  if (map == NULL || key == NULL || value == NULL)
    return HAL_EINVAL;
  if (!ready(map))
    return HAL_ESTATE;
  struct jb_map_node *parent;
  int order;
  const struct jb_map_node *node = search(map, key, &parent, &order);
  if (node == NULL)
    return HAL_ENOENT;
  *value = node_value_const(map, node);
  return HAL_OK;
}

hal_status_t jb_map_find(const jb_map_t *map, const void *key, void **value) {
  if (value == NULL)
    return HAL_EINVAL;
  const void *found;
  const hal_status_t status = jb_map_find_const(map, key, &found);
  if (status == HAL_OK)
    *value = (void *)found;
  return status;
}

static struct jb_map_node *first_node(struct jb_map_node *node) {
  while (node != NULL && node->left != NULL)
    node = node->left;
  return node;
}

hal_status_t jb_map_erase(jb_map_t *map, const void *key) {
  if (map == NULL || key == NULL)
    return HAL_EINVAL;
  if (!ready(map))
    return HAL_ESTATE;
  struct jb_map_node *parent;
  int order;
  struct jb_map_node *node = search(map, key, &parent, &order);
  if (node == NULL)
    return HAL_ENOENT;
  struct jb_map_node *start = node->parent;
  if (node->left == NULL || node->right == NULL) {
    replace(map, node, node->left != NULL ? node->left : node->right);
  } else {
    struct jb_map_node *successor = first_node(node->right);
    if (successor->parent != node) {
      start = successor->parent;
      replace(map, successor, successor->right);
      successor->right = node->right;
      successor->right->parent = successor;
    } else {
      start = successor;
    }
    replace(map, node, successor);
    successor->left = node->left;
    successor->left->parent = successor;
    successor->height = node->height;
  }
  --map->size;
  map->allocator.release(map->allocator.context, node);
  rebalance(map, start);
  return HAL_OK;
}

size_t jb_map_size(const jb_map_t *map) { return map == NULL ? 0 : map->size; }

void jb_map_clear(jb_map_t *map) {
  if (!ready(map))
    return;
  struct jb_map_node *node = map->root;
  while (node != NULL) {
    if (node->left != NULL) {
      node = node->left;
    } else if (node->right != NULL) {
      node = node->right;
    } else {
      struct jb_map_node *parent = node->parent;
      replace(map, node, NULL);
      map->allocator.release(map->allocator.context, node);
      node = parent;
    }
  }
  map->size = 0;
}

void jb_map_destroy(jb_map_t *map) {
  if (map != NULL) {
    jb_map_clear(map);
    *map = (jb_map_t){0};
  }
}

hal_status_t jb_map_first(const jb_map_t *map, jb_map_iter_t *iterator) {
  if (map == NULL || iterator == NULL)
    return HAL_EINVAL;
  if (!ready(map))
    return HAL_ESTATE;
  const struct jb_map_node *node = first_node(map->root);
  if (node == NULL)
    return HAL_ENOENT;
  *iterator = (jb_map_iter_t){map, node};
  return HAL_OK;
}

hal_status_t jb_map_next(jb_map_iter_t *iterator) {
  if (iterator == NULL)
    return HAL_EINVAL;
  if (!ready(iterator->owner))
    return HAL_ESTATE;
  const struct jb_map_node *node = iterator->node;
  if (node == NULL)
    return HAL_ENOENT;
  if (node->right != NULL) {
    iterator->node = first_node(node->right);
  } else {
    const struct jb_map_node *parent = node->parent;
    while (parent != NULL && node == parent->right) {
      node = parent;
      parent = parent->parent;
    }
    iterator->node = parent;
  }
  return iterator->node == NULL ? HAL_ENOENT : HAL_OK;
}

hal_status_t jb_map_iter_get(const jb_map_iter_t *iterator, const void **key, const void **value) {
  if (iterator == NULL || key == NULL || value == NULL)
    return HAL_EINVAL;
  if (!ready(iterator->owner))
    return HAL_ESTATE;
  if (iterator->node == NULL)
    return HAL_ENOENT;
  *key = node_key(iterator->owner, iterator->node);
  *value = node_value_const(iterator->owner, iterator->node);
  return HAL_OK;
}
