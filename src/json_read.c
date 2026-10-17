#include "json_internal.h"

static hal_status_t record(jb_json_node_t node, const jb_json_token_t **out) {
  if (node.doc == NULL || !node.doc->valid || node.doc->tokens == NULL ||
      node.index >= node.doc->token_count) {
    return HAL_ESTATE;
  }
  *out = &node.doc->tokens[node.index];
  return HAL_OK;
}

hal_status_t jb_json_root(const jb_json_doc_t *doc, jb_json_node_t *out) {
  if (doc == NULL || out == NULL) {
    return HAL_EINVAL;
  }
  const jb_json_node_t root = {doc, 0};
  const jb_json_token_t *token;
  const hal_status_t status = record(root, &token);
  if (status == HAL_OK) {
    *out = root;
  }
  return status;
}

hal_status_t jb_json_type(jb_json_node_t node, jb_json_type_t *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  const jb_json_token_t *token;
  const hal_status_t status = record(node, &token);
  if (status == HAL_OK) {
    *out = token->type;
  }
  return status;
}

hal_status_t jb_json_count(jb_json_node_t node, size_t *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  const jb_json_token_t *token;
  const hal_status_t status = record(node, &token);
  if (status != HAL_OK) {
    return status;
  }
  if (token->type != JB_JSON_ARRAY && token->type != JB_JSON_OBJECT) {
    return HAL_ETYPE;
  }
  *out = token->child_count;
  return HAL_OK;
}

static hal_status_t related(jb_json_node_t node, size_t index, jb_json_node_t *out) {
  if (index == JB_JSON_NODE_NONE) {
    return HAL_ENOENT;
  }
  *out = (jb_json_node_t){node.doc, index};
  return HAL_OK;
}

hal_status_t jb_json_first(jb_json_node_t container, jb_json_node_t *out) {
  size_t count;
  if (out == NULL) {
    return HAL_EINVAL;
  }
  const hal_status_t status = jb_json_count(container, &count);
  if (status != HAL_OK) {
    return status;
  }
  return related(container, container.doc->tokens[container.index].first_child, out);
}

hal_status_t jb_json_next(jb_json_node_t node, jb_json_node_t *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  const jb_json_token_t *token;
  const hal_status_t status = record(node, &token);
  return status == HAL_OK ? related(node, token->next_sibling, out) : status;
}

hal_status_t jb_json_find(jb_json_node_t object, jb_json_span_t key, jb_json_node_t *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  const jb_json_token_t *token;
  hal_status_t status = record(object, &token);
  if (status != HAL_OK) {
    return status;
  }
  if (token->type != JB_JSON_OBJECT) {
    return HAL_ETYPE;
  }
  status = jb_utf8_validate(key.data, key.length, NULL);
  if (status != HAL_OK) {
    return status;
  }
  size_t at = token->first_child;
  while (at != JB_JSON_NODE_NONE) {
    const jb_json_token_t *member = &object.doc->tokens[at];
    const jb_json_span_t raw = {object.doc->input.data + member->key_offset, member->key_length};
    bool equal;
    size_t used = 0;
    status = jb_json_strings_equal(raw, key, false, SIZE_MAX, &used, &equal);
    if (status != HAL_OK) {
      return status;
    }
    if (equal) {
      return related(object, at, out);
    }
    at = member->next_sibling;
  }
  return HAL_ENOENT;
}

hal_status_t jb_json_at(jb_json_node_t array, size_t position, jb_json_node_t *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  const jb_json_token_t *token;
  const hal_status_t status = record(array, &token);
  if (status != HAL_OK) {
    return status;
  }
  if (token->type != JB_JSON_ARRAY) {
    return HAL_ETYPE;
  }
  if (position >= token->child_count) {
    return HAL_ENOENT;
  }
  size_t at = token->first_child;
  for (size_t i = 0; i < position; ++i) {
    at = array.doc->tokens[at].next_sibling;
  }
  return related(array, at, out);
}

hal_status_t jb_json_raw(jb_json_node_t node, jb_json_span_t *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  const jb_json_token_t *token;
  const hal_status_t status = record(node, &token);
  if (status == HAL_OK) {
    *out = (jb_json_span_t){node.doc->input.data + token->offset, token->length};
  }
  return status;
}

hal_status_t jb_json_number_token(jb_json_node_t node, jb_json_span_t *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  const jb_json_token_t *token;
  const hal_status_t status = record(node, &token);
  if (status != HAL_OK) {
    return status;
  }
  return token->type == JB_JSON_NUMBER ? jb_json_raw(node, out) : HAL_ETYPE;
}

hal_status_t jb_json_get_bool(jb_json_node_t node, bool *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  const jb_json_token_t *token;
  const hal_status_t status = record(node, &token);
  if (status != HAL_OK) {
    return status;
  }
  if (token->type != JB_JSON_BOOL) {
    return HAL_ETYPE;
  }
  *out = node.doc->input.data[token->offset] == 't';
  return HAL_OK;
}

/* Escaped bytes of a member name or of a string value without its quotes. */
static jb_json_span_t encoded_text(const jb_json_doc_t *doc, const jb_json_token_t *token,
                                   bool key) {
  return (jb_json_span_t){doc->input.data + (key ? token->key_offset : token->offset + 1),
                          key ? token->key_length : token->length - 2};
}

static hal_status_t copy_value(jb_json_node_t node, bool key, bool terminate, char *buffer,
                               size_t capacity, size_t *required) {
  if (required == NULL || (buffer == NULL && capacity != 0)) {
    return HAL_EINVAL;
  }
  const jb_json_token_t *token;
  const hal_status_t status = record(node, &token);
  if (status != HAL_OK) {
    return status;
  }
  if (key ? !token->has_key : token->type != JB_JSON_STRING) {
    return key ? HAL_EINVAL : HAL_ETYPE;
  }
  return jb_json_copy_string(encoded_text(node.doc, token, key), buffer, capacity, required,
                             terminate);
}

hal_status_t jb_json_string_copy(jb_json_node_t node, char *buffer, size_t capacity,
                                 size_t *required) {
  return copy_value(node, false, false, buffer, capacity, required);
}

hal_status_t jb_json_key_copy(jb_json_node_t member, char *buffer, size_t capacity,
                              size_t *required) {
  return copy_value(member, true, false, buffer, capacity, required);
}

hal_status_t jb_json_string_cstr(jb_json_node_t node, char *buffer, size_t capacity,
                                 size_t *required) {
  return copy_value(node, false, true, buffer, capacity, required);
}

hal_status_t jb_json_string_equal(jb_json_node_t node, jb_json_span_t value, bool *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  const jb_json_token_t *token;
  hal_status_t status = record(node, &token);
  if (status != HAL_OK) {
    return status;
  }
  if (token->type != JB_JSON_STRING) {
    return HAL_ETYPE;
  }
  status = jb_utf8_validate(value.data, value.length, NULL);
  if (status != HAL_OK) {
    return status;
  }
  size_t used = 0;
  return jb_json_strings_equal(encoded_text(node.doc, token, false), value, false, SIZE_MAX, &used,
                               out);
}
