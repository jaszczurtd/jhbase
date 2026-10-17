#include "jb/json_writer.h"
#include "json_internal.h"
#include "number_internal.h"
#include <string.h>

typedef struct {
  size_t count, keys, bytes;
  bool object, pending;
} writer_frame_t;
typedef struct {
  size_t offset, length;
} writer_key_t;
typedef struct {
  jb_json_writer_limits_t limits;
  jb_json_sink_fn sink;
  void *user;
  unsigned char *frames, *records, *bytes;
  size_t depth, keys, key_bytes, compared, written;
  bool root, finished;
} writer_state_t;

static hal_status_t cache(jb_json_writer_t *w, hal_status_t status) {
  if (w != NULL && status != HAL_OK) {
    w->private_status = status;
  }
  return status;
}
static hal_status_t load(jb_json_writer_t *w, writer_state_t *s) {
  if (w == NULL) {
    return HAL_EINVAL;
  }
  if (w->private_status != HAL_OK) {
    return w->private_status == HAL_NONE ? cache(w, HAL_ESTATE) : w->private_status;
  }
  if (w->state == NULL) {
    return cache(w, HAL_ESTATE);
  }
  memcpy(s, w->state, sizeof(*s));
  return HAL_OK;
}
static hal_status_t writable(jb_json_writer_t *w, writer_state_t *s) {
  const hal_status_t status = load(w, s);
  return status == HAL_OK && s->finished ? cache(w, HAL_ESTATE) : status;
}
static hal_status_t save(jb_json_writer_t *w, const writer_state_t *s, hal_status_t status) {
  if (status == HAL_OK) {
    memcpy(w->state, s, sizeof(*s));
  }
  return cache(w, status);
}
static void read_frame(const writer_state_t *s, writer_frame_t *f) {
  memcpy(f, s->frames + (s->depth - 1) * sizeof(*f), sizeof(*f));
}
static void write_frame(const writer_state_t *s, const writer_frame_t *f) {
  memcpy(s->frames + (s->depth - 1) * sizeof(*f), f, sizeof(*f));
}
static bool size_add(size_t *size, size_t count, size_t width) {
  if (count > (SIZE_MAX - *size) / width) {
    return false;
  }
  *size += count * width;
  return true;
}
hal_status_t jb_json_writer_workspace_size(const jb_json_writer_limits_t *limits, size_t *out) {
  if (limits == NULL || out == NULL) {
    return HAL_EINVAL;
  }
  size_t size = sizeof(writer_state_t);
  if (!size_add(&size, limits->max_depth, sizeof(writer_frame_t)) ||
      !size_add(&size, limits->max_keys, sizeof(writer_key_t)) ||
      !size_add(&size, limits->max_key_bytes, 1)) {
    return HAL_EOVERFLOW;
  }
  *out = size;
  return HAL_OK;
}
hal_status_t jb_json_writer_init(jb_json_writer_t *w, const jb_json_writer_limits_t *limits,
                                 jb_json_workspace_t work, jb_json_sink_fn sink, void *user) {
  if (w == NULL) {
    return HAL_EINVAL;
  }
  *w = (jb_json_writer_t){NULL, HAL_EUNINIT};
  if (limits == NULL || sink == NULL || (work.data == NULL && work.size != 0)) {
    return cache(w, HAL_EINVAL);
  }
  size_t needed;
  hal_status_t status = jb_json_writer_workspace_size(limits, &needed);
  if (status != HAL_OK) {
    return cache(w, status);
  }
  if (work.size < needed) {
    return cache(w, HAL_EOVERFLOW);
  }
  writer_state_t s = {0};
  s.limits = *limits;
  s.sink = sink;
  s.user = user;
  s.frames = (unsigned char *)work.data + sizeof(s);
  s.records = s.frames + limits->max_depth * sizeof(writer_frame_t);
  s.bytes = s.records + limits->max_keys * sizeof(writer_key_t);
  memcpy(work.data, &s, sizeof(s));
  w->state = work.data;
  w->private_status = HAL_OK;
  return HAL_OK;
}
hal_status_t jb_json_buffer_sink(void *user, const char *data, size_t length) {
  jb_json_buffer_t *buffer = user;
  if (buffer == NULL || (data == NULL && length != 0) || buffer->used > buffer->capacity ||
      (buffer->data == NULL && buffer->capacity != 0)) {
    return HAL_EINVAL;
  }
  if (length > buffer->capacity - buffer->used) {
    return HAL_EOVERFLOW;
  }
  if (length != 0) {
    memcpy(buffer->data + buffer->used, data, length);
  }
  buffer->used += length;
  return HAL_OK;
}
hal_status_t jb_json_count_sink(void *user, const char *data, size_t length) {
  if (user == NULL || (data == NULL && length != 0)) {
    return HAL_EINVAL;
  }
  size_t *count = user;
  if (length > SIZE_MAX - *count) {
    return HAL_EOVERFLOW;
  }
  *count += length;
  return HAL_OK;
}
static hal_status_t emit(writer_state_t *s, const char *data, size_t length) {
  if (length > s->limits.max_output_bytes - s->written) {
    return HAL_EOVERFLOW;
  }
  if (length == 0) {
    return HAL_OK;
  }
  const hal_status_t status = s->sink(s->user, data, length);
  if (status != HAL_OK) {
    return status <= HAL_EINVAL && status >= HAL_ETYPE ? status : HAL_EINTERNAL;
  }
  s->written += length;
  return HAL_OK;
}
static hal_status_t value_prefix(writer_state_t *s) {
  if (s->finished) {
    return HAL_ESTATE;
  }
  if (s->depth == 0) {
    if (s->root) {
      return HAL_ESTATE;
    }
    s->root = true;
    return HAL_OK;
  }
  writer_frame_t f;
  read_frame(s, &f);
  if (f.object && !f.pending) {
    return HAL_ESTATE;
  }
  if (f.count == SIZE_MAX) {
    return HAL_EOVERFLOW;
  }
  hal_status_t status = HAL_OK;
  if (!f.object && f.count != 0) {
    status = emit(s, ",", 1);
  }
  f.pending = false;
  ++f.count;
  write_frame(s, &f);
  return status;
}
static hal_status_t container(jb_json_writer_t *w, bool object, bool opening) {
  writer_state_t s;
  hal_status_t status = writable(w, &s);
  if (status != HAL_OK) {
    return status;
  }
  if (opening) {
    if (s.depth == s.limits.max_depth) {
      return cache(w, HAL_EOVERFLOW);
    }
    status = value_prefix(&s);
    if (status == HAL_OK) {
      status = emit(&s, object ? "{" : "[", 1);
    }
    if (status == HAL_OK) {
      const writer_frame_t f = {0, s.keys, s.key_bytes, object, false};
      ++s.depth;
      write_frame(&s, &f);
    }
  } else {
    if (s.finished || s.depth == 0) {
      return cache(w, HAL_ESTATE);
    }
    writer_frame_t f;
    read_frame(&s, &f);
    if (f.object != object || f.pending) {
      return cache(w, HAL_ESTATE);
    }
    status = emit(&s, object ? "}" : "]", 1);
    if (status == HAL_OK) {
      --s.depth;
      s.keys = f.keys;
      s.key_bytes = f.bytes;
    }
  }
  return save(w, &s, status);
}
hal_status_t jb_json_writer_object_begin(jb_json_writer_t *w) { return container(w, true, true); }
hal_status_t jb_json_writer_object_end(jb_json_writer_t *w) { return container(w, true, false); }
hal_status_t jb_json_writer_array_begin(jb_json_writer_t *w) { return container(w, false, true); }
hal_status_t jb_json_writer_array_end(jb_json_writer_t *w) { return container(w, false, false); }

static hal_status_t escaped_byte(writer_state_t *s, unsigned char c) {
  if (c == '"' || c == '\\') {
    const char escape[2] = {'\\', (char)c};
    return emit(s, escape, 2);
  }
  if (c < 0x20) {
    static const char hex[] = "0123456789abcdef";
    const char escape[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
    return emit(s, escape, 6);
  }
  const char byte = (char)c;
  return emit(s, &byte, 1);
}
static hal_status_t quoted(writer_state_t *s, jb_json_span_t value, bool escaped) {
  hal_status_t status = emit(s, "\"", 1);
  size_t position = 0;
  while (status == HAL_OK && position < value.length) {
    if (!escaped) {
      status = escaped_byte(s, (unsigned char)value.data[position++]);
    } else {
      uint32_t scalar;
      char bytes[4];
      size_t width = 0; /* Read only after a successful encode; MSVC cannot see that. */
      status = jb_json_scalar(value, &position, &scalar, NULL);
      if (status == HAL_OK) {
        status = jb_utf8_encode(scalar, bytes, sizeof(bytes), &width);
      }
      for (size_t i = 0; status == HAL_OK && i < width; ++i) {
        status = escaped_byte(s, (unsigned char)bytes[i]);
      }
    }
  }
  return status == HAL_OK ? emit(s, "\"", 1) : status;
}

static hal_status_t key(jb_json_writer_t *w, jb_json_span_t value, bool escaped) {
  writer_state_t s;
  hal_status_t status = writable(w, &s);
  if (status != HAL_OK) {
    return status;
  }
  if (s.finished || s.depth == 0) {
    return cache(w, HAL_ESTATE);
  }
  writer_frame_t f;
  read_frame(&s, &f);
  if (!f.object || f.pending) {
    return cache(w, HAL_ESTATE);
  }
  size_t length = value.length;
  if (escaped) {
    status = jb_json_copy_string(value, NULL, 0, &length, false);
    if (status == HAL_EOVERFLOW) {
      status = HAL_OK;
    }
  } else {
    status = jb_utf8_validate(value.data, value.length, NULL);
  }
  if (status != HAL_OK) {
    return cache(w, status);
  }
  if (s.keys == s.limits.max_keys || length > s.limits.max_key_bytes - s.key_bytes) {
    return cache(w, HAL_EOVERFLOW);
  }
  char *candidate = (char *)s.bytes + s.key_bytes;
  if (escaped) {
    status = jb_json_copy_string(value, candidate, length, &length, false);
  } else if (length != 0) {
    memcpy(candidate, value.data, length);
  }
  if (status != HAL_OK) {
    return cache(w, status);
  }
  for (size_t i = f.keys; i < s.keys; ++i) {
    writer_key_t old;
    memcpy(&old, s.records + i * sizeof(old), sizeof(old));
    if (s.compared == s.limits.max_key_compare_bytes) {
      return cache(w, HAL_EOVERFLOW);
    }
    ++s.compared;
    bool equal = old.length == length;
    if (equal) {
      for (size_t j = 0; j < length; ++j) {
        if (s.limits.max_key_compare_bytes - s.compared < 2) {
          return cache(w, HAL_EOVERFLOW);
        }
        s.compared += 2;
        if (candidate[j] != (char)s.bytes[old.offset + j]) {
          equal = false;
          break;
        }
      }
    }
    if (equal) {
      return cache(w, HAL_EEXIST);
    }
  }
  const writer_key_t record = {s.key_bytes, length};
  memcpy(s.records + s.keys * sizeof(record), &record, sizeof(record));
  ++s.keys;
  s.key_bytes += length;
  if (f.count != 0) {
    status = emit(&s, ",", 1);
  }
  if (status == HAL_OK) {
    status = quoted(&s, (jb_json_span_t){candidate, length}, false);
  }
  if (status == HAL_OK) {
    status = emit(&s, ":", 1);
  }
  f.pending = true;
  write_frame(&s, &f);
  return save(w, &s, status);
}
hal_status_t jb_json_writer_key(jb_json_writer_t *w, jb_json_span_t value) {
  return key(w, value, false);
}
static hal_status_t string_value(jb_json_writer_t *w, jb_json_span_t value, bool escaped) {
  writer_state_t s;
  hal_status_t status = writable(w, &s);
  if (status != HAL_OK) {
    return status;
  }
  if (!escaped) {
    status = jb_utf8_validate(value.data, value.length, NULL);
  }
  if (status == HAL_OK) {
    status = value_prefix(&s);
  }
  if (status == HAL_OK) {
    status = quoted(&s, value, escaped);
  }
  return save(w, &s, status);
}
hal_status_t jb_json_writer_string(jb_json_writer_t *w, jb_json_span_t value) {
  return string_value(w, value, false);
}
static hal_status_t raw_value(jb_json_writer_t *w, jb_json_span_t value, bool validate) {
  writer_state_t s;
  hal_status_t status = writable(w, &s);
  if (status != HAL_OK) {
    return status;
  }
  if (value.length > s.limits.max_output_bytes - s.written) {
    return cache(w, HAL_EOVERFLOW);
  }
  if (validate) {
    status = jb_number_validate(value);
  }
  if (status == HAL_OK) {
    status = value_prefix(&s);
  }
  if (status == HAL_OK) {
    status = emit(&s, value.data, value.length);
  }
  return save(w, &s, status);
}
hal_status_t jb_json_writer_bool(jb_json_writer_t *w, bool value) {
  return raw_value(w, (jb_json_span_t){value ? "true" : "false", value ? 4u : 5u}, false);
}
hal_status_t jb_json_writer_null(jb_json_writer_t *w) {
  return raw_value(w, (jb_json_span_t){"null", 4}, false);
}
hal_status_t jb_json_writer_u64(jb_json_writer_t *w, uint64_t value) {
  char text[20];
  const size_t length = jb_u64_digits(value, text);
  return raw_value(w, (jb_json_span_t){text, length}, false);
}
hal_status_t jb_json_writer_i64(jb_json_writer_t *w, int64_t value) {
  char text[21];
  size_t length = 0;
  if (value < 0) {
    text[length++] = '-';
  }
  length +=
      jb_u64_digits(value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value, text + length);
  return raw_value(w, (jb_json_span_t){text, length}, false);
}
hal_status_t jb_json_writer_number(jb_json_writer_t *w, jb_json_span_t token) {
  return raw_value(w, token, true);
}
static hal_status_t float_value(jb_json_writer_t *w, uint64_t bits, unsigned width) {
  writer_state_t s;
  hal_status_t status = writable(w, &s);
  if (status != HAL_OK) {
    return status;
  }
  char text[40];
  size_t length;
  status = jb_float_shortest(bits, width, text, &length);
  if (status != HAL_OK) {
    return cache(w, status);
  }
  return raw_value(w, (jb_json_span_t){text, length}, false);
}
hal_status_t jb_json_writer_f32(jb_json_writer_t *w, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return float_value(w, bits, 32);
}
hal_status_t jb_json_writer_f64(jb_json_writer_t *w, double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return float_value(w, bits, 64);
}

hal_status_t jb_json_writer_node(jb_json_writer_t *w, jb_json_node_t node) {
  writer_state_t s;
  hal_status_t status = writable(w, &s);
  if (status != HAL_OK) {
    return status;
  }
  jb_json_type_t type;
  status = jb_json_type(node, &type);
  if (status != HAL_OK) {
    return cache(w, status);
  }
  size_t at = node.index;
  for (;;) {
    const jb_json_token_t *t = &node.doc->tokens[at];
    if (at != node.index && t->has_key) {
      status = key(w, (jb_json_span_t){node.doc->input.data + t->key_offset, t->key_length}, true);
      if (status != HAL_OK) {
        return status;
      }
    }
    const jb_json_span_t raw = {node.doc->input.data + t->offset, t->length};
    if (t->type == JB_JSON_ARRAY || t->type == JB_JSON_OBJECT) {
      status = container(w, t->type == JB_JSON_OBJECT, true);
      if (status != HAL_OK) {
        return status;
      }
      if (t->first_child != JB_JSON_NODE_NONE) {
        at = t->first_child;
        continue;
      }
      status = container(w, t->type == JB_JSON_OBJECT, false);
    } else if (t->type == JB_JSON_STRING) {
      status = string_value(w, (jb_json_span_t){raw.data + 1, raw.length - 2}, true);
    } else {
      status = raw_value(w, raw, false);
    }
    if (status != HAL_OK) {
      return status;
    }
    for (;;) {
      if (at == node.index) {
        return HAL_OK;
      }
      t = &node.doc->tokens[at];
      if (t->next_sibling != JB_JSON_NODE_NONE) {
        at = t->next_sibling;
        break;
      }
      at = t->parent;
      t = &node.doc->tokens[at];
      status = container(w, t->type == JB_JSON_OBJECT, false);
      if (status != HAL_OK) {
        return status;
      }
    }
  }
}
hal_status_t jb_json_writer_finish(jb_json_writer_t *w, size_t *out_bytes) {
  writer_state_t s;
  hal_status_t status = load(w, &s);
  if (status != HAL_OK) {
    return status;
  }
  if (out_bytes == NULL) {
    return cache(w, HAL_EINVAL);
  }
  if (!s.root || s.depth != 0) {
    return cache(w, HAL_ESTATE);
  }
  s.finished = true;
  *out_bytes = s.written;
  return save(w, &s, HAL_OK);
}
