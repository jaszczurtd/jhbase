#include "jb/json_pointer.h"
#include "json_internal.h"

/* Syntax is independent of the document, even after a missing reference. */
static hal_status_t pointer_syntax(jb_json_span_t pointer) {
  if (pointer.length != 0 && pointer.data[0] != '/') {
    return HAL_EPROTO;
  }
  size_t at = 0;
  while (at < pointer.length) {
    uint32_t scalar;
    const hal_status_t status = jb_json_text_scalar(pointer, &at, JB_JSON_TEXT_POINTER, &scalar);
    if (status != HAL_OK) {
      return status;
    }
  }
  return HAL_OK;
}

static hal_status_t array_reference(jb_json_node_t node, jb_json_span_t part, jb_json_node_t *out) {
  if (part.length == 1 && part.data[0] == '-') {
    return HAL_ENOENT;
  }
  if (part.length == 0 || (part.length > 1 && part.data[0] == '0')) {
    return HAL_EPROTO;
  }
  const size_t count = node.doc->tokens[node.index].child_count;
  size_t position = 0;
  bool missing = false;
  for (size_t i = 0; i < part.length; ++i) {
    const unsigned char c = (unsigned char)part.data[i];
    if (c < '0' || c > '9') {
      return HAL_EPROTO;
    }
    const size_t digit = c - '0';
    if (!missing) {
      missing = position > count / 10 || (position == count / 10 && digit > count % 10);
      if (!missing) {
        position = position * 10 + digit;
      }
    }
  }
  return missing ? HAL_ENOENT : jb_json_at(node, position, out);
}

hal_status_t jb_json_pointer_get(const jb_json_doc_t *doc, jb_json_span_t pointer,
                                 jb_json_node_t *out) {
  if (doc == NULL || out == NULL || (pointer.data == NULL && pointer.length != 0)) {
    return HAL_EINVAL;
  }
  jb_json_node_t node;
  hal_status_t status = jb_json_root(doc, &node);
  if (status == HAL_OK) {
    status = pointer_syntax(pointer);
  }
  if (status != HAL_OK) {
    return status;
  }
  size_t at = 0;
  while (at < pointer.length) {
    const size_t start = ++at;
    while (at < pointer.length && pointer.data[at] != '/') {
      ++at;
    }
    const jb_json_span_t part = {pointer.data + start, at - start};
    const jb_json_type_t type = doc->tokens[node.index].type;
    if (type == JB_JSON_OBJECT) {
      status = jb_json_find_encoded(node, part, JB_JSON_TEXT_POINTER, &node);
    } else if (type == JB_JSON_ARRAY) {
      status = array_reference(node, part, &node);
    } else {
      return HAL_ETYPE;
    }
    if (status != HAL_OK) {
      return status;
    }
  }
  *out = node;
  return HAL_OK;
}

typedef struct {
  jb_json_span_t key;
  size_t size, position;
  bool member;
} pointer_part_t;

static hal_status_t key_size(jb_json_span_t key, size_t *out) {
  size_t at = 0, size = 1; /* The separating slash also has to fit. */
  while (at < key.length) {
    uint32_t scalar;
    size_t width;
    const hal_status_t status = jb_json_scalar(key, &at, &scalar, NULL);
    if (status != HAL_OK) {
      return status;
    }
    (void)jb_utf8_encode(scalar, NULL, 0, &width);
    if (scalar == '~' || scalar == '/') {
      width = 2;
    }
    if (width > SIZE_MAX - size) {
      return HAL_EOVERFLOW;
    }
    size += width;
  }
  *out = size;
  return HAL_OK;
}

static hal_status_t pointer_part(jb_json_node_t node, pointer_part_t *out) {
  const jb_json_token_t *token;
  hal_status_t status = jb_json_record(node, &token);
  if (status != HAL_OK) {
    return status;
  }
  if (token->parent >= node.index) {
    return HAL_ESTATE;
  }
  const jb_json_token_t *parent = &node.doc->tokens[token->parent];
  *out = (pointer_part_t){0};
  if (parent->type == JB_JSON_OBJECT && token->has_key) {
    out->member = true;
    out->key = (jb_json_span_t){node.doc->input.data + token->key_offset, token->key_length};
    return key_size(out->key, &out->size);
  }
  if (parent->type != JB_JSON_ARRAY) {
    return HAL_ESTATE;
  }
  size_t at = parent->first_child;
  while (at != node.index) {
    if (at >= node.doc->token_count || out->position >= parent->child_count) {
      return HAL_ESTATE;
    }
    at = node.doc->tokens[at].next_sibling;
    ++out->position;
  }
  out->size = 2; /* Slash and at least one digit. */
  for (size_t value = out->position; value >= 10; value /= 10) {
    ++out->size;
  }
  return HAL_OK;
}

static hal_status_t write_part(const pointer_part_t *part, char *buffer) {
  buffer[0] = '/';
  if (!part->member) {
    size_t at = part->size, value = part->position;
    do {
      buffer[--at] = (char)('0' + value % 10);
      value /= 10;
    } while (value != 0);
    return HAL_OK;
  }
  size_t at = 0, written = 1;
  while (at < part->key.length) {
    uint32_t scalar;
    hal_status_t status = jb_json_scalar(part->key, &at, &scalar, NULL);
    if (status != HAL_OK) {
      return status;
    }
    size_t width;
    if (scalar == '~' || scalar == '/') {
      buffer[written] = '~';
      buffer[written + 1] = scalar == '~' ? '0' : '1';
      width = 2;
    } else {
      status = jb_utf8_encode(scalar, buffer + written, part->size - written, &width);
      if (status != HAL_OK) {
        return status;
      }
    }
    written += width;
  }
  return HAL_OK;
}

hal_status_t jb_json_pointer_copy(jb_json_node_t node, char *buffer, size_t capacity,
                                  size_t *required) {
  if (required == NULL || (buffer == NULL && capacity != 0)) {
    return HAL_EINVAL;
  }
  const jb_json_token_t *token;
  hal_status_t status = jb_json_record(node, &token);
  if (status != HAL_OK) {
    return status;
  }
  size_t size = 0;
  jb_json_node_t ancestor = node;
  while (ancestor.index != 0) {
    pointer_part_t part;
    status = pointer_part(ancestor, &part);
    if (status != HAL_OK) {
      return status;
    }
    if (part.size > SIZE_MAX - size) {
      return HAL_EOVERFLOW;
    }
    size += part.size;
    ancestor.index = ancestor.doc->tokens[ancestor.index].parent;
  }
  *required = size;
  if (buffer == NULL) {
    return HAL_OK;
  }
  if (size > capacity) {
    return HAL_EOVERFLOW;
  }
  while (node.index != 0) {
    pointer_part_t part;
    status = pointer_part(node, &part);
    if (status != HAL_OK) {
      return status;
    }
    size -= part.size;
    status = write_part(&part, buffer + size);
    if (status != HAL_OK) {
      return status;
    }
    node.index = node.doc->tokens[node.index].parent;
  }
  return HAL_OK;
}
