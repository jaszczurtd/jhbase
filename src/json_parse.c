#include "json_internal.h"

#include <string.h>

enum { FRAME_FIRST, FRAME_REQUIRED, FRAME_AFTER, FRAME_COLON, FRAME_VALUE };

typedef struct {
  size_t node, children, first, last, key_offset, key_length;
  unsigned state;
  bool object;
} parse_frame_t;

typedef struct {
  jb_json_span_t input;
  const jb_json_limits_t *limits;
  jb_json_token_t *tokens;
  size_t capacity, count, position, depth, deepest, compared;
  jb_json_workspace_t work;
  jb_json_error_t *error;
  bool measure;
} parser_t;

static hal_status_t fail(parser_t *p, hal_status_t status, jb_json_error_code_t code, size_t offset,
                         size_t required) {
  if (p->error != NULL) {
    *p->error = (jb_json_error_t){code, offset, SIZE_MAX, required};
  }
  return status;
}

static hal_status_t syntax(parser_t *p) {
  return fail(p, HAL_EPROTO, JB_JSON_ERROR_SYNTAX, p->position, 0);
}

static hal_status_t limit(parser_t *p, size_t offset) {
  return fail(p, HAL_EOVERFLOW, JB_JSON_ERROR_LIMIT, offset, 0);
}

/* memcpy preserves effective type and supports deliberately unaligned scratch. */
static void frame_read(const parser_t *p, size_t index, parse_frame_t *out) {
  memcpy(out, (const unsigned char *)p->work.data + index * sizeof(*out), sizeof(*out));
}

static void frame_write(parser_t *p, size_t index, const parse_frame_t *frame) {
  memcpy((unsigned char *)p->work.data + index * sizeof(*frame), frame, sizeof(*frame));
}

static void whitespace(parser_t *p) {
  while (p->position < p->input.length) {
    const char c = p->input.data[p->position];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      break;
    }
    ++p->position;
  }
}

static hal_status_t scan_string(parser_t *p, size_t *raw_offset, size_t *raw_length) {
  ++p->position; /* Opening quote was checked by the caller. */
  const size_t start = p->position;
  size_t decoded = 0;
  while (p->position < p->input.length && p->input.data[p->position] != '"') {
    const size_t at = p->position;
    uint32_t scalar;
    size_t width;
    hal_status_t status = jb_json_scalar(p->input, &p->position, &scalar, p->error);
    if (status != HAL_OK) {
      return status;
    }
    (void)jb_utf8_encode(scalar, NULL, 0, &width);
    if (width > p->limits->max_string_bytes - decoded) {
      return limit(p, at);
    }
    decoded += width;
  }
  if (p->position == p->input.length) {
    return syntax(p);
  }
  *raw_offset = start;
  *raw_length = p->position - start;
  ++p->position;
  return HAL_OK;
}

static bool digit(const parser_t *p) {
  return p->position < p->input.length && p->input.data[p->position] >= '0' &&
         p->input.data[p->position] <= '9';
}

static hal_status_t digits(parser_t *p) {
  if (!digit(p)) {
    return syntax(p);
  }
  do {
    ++p->position;
  } while (digit(p));
  return HAL_OK;
}

static hal_status_t scan_number(parser_t *p) {
  const size_t start = p->position;
  if (p->input.data[p->position] == '-') {
    ++p->position;
  }
  if (!digit(p)) {
    return syntax(p);
  }
  if (p->input.data[p->position] == '0') {
    ++p->position;
    if (digit(p)) {
      return syntax(p);
    }
  } else {
    (void)digits(p);
  }
  if (p->position < p->input.length && p->input.data[p->position] == '.') {
    ++p->position;
    if (digits(p) != HAL_OK) {
      return HAL_EPROTO;
    }
  }
  if (p->position < p->input.length &&
      (p->input.data[p->position] == 'e' || p->input.data[p->position] == 'E')) {
    ++p->position;
    if (p->position < p->input.length &&
        (p->input.data[p->position] == '+' || p->input.data[p->position] == '-')) {
      ++p->position;
    }
    if (digits(p) != HAL_OK) {
      return HAL_EPROTO;
    }
  }
  if (p->position - start > p->limits->max_number_bytes) {
    return limit(p, start);
  }
  return HAL_OK;
}

static hal_status_t literal(parser_t *p, const char *word, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (p->position == p->input.length || p->input.data[p->position] != word[i]) {
      return syntax(p);
    }
    ++p->position;
  }
  return HAL_OK;
}

static hal_status_t unique_key(parser_t *p, const parse_frame_t *parent) {
  if (p->measure) {
    return HAL_OK;
  }
  const jb_json_span_t key = {p->input.data + parent->key_offset, parent->key_length};
  size_t at = parent->first;
  while (at != JB_JSON_NODE_NONE) {
    const jb_json_token_t *old = &p->tokens[at];
    const jb_json_span_t prior = {p->input.data + old->key_offset, old->key_length};
    bool equal;
    const hal_status_t status = jb_json_strings_equal(
        prior, key, JB_JSON_TEXT_JSON, p->limits->max_key_compare_bytes, &p->compared, &equal);
    if (status != HAL_OK) {
      return limit(p, parent->key_offset - 1);
    }
    if (equal) {
      (void)fail(p, HAL_EPROTO, JB_JSON_ERROR_DUPLICATE_KEY, parent->key_offset - 1, 0);
      if (p->error != NULL) {
        p->error->related_offset = old->key_offset - 1;
      }
      return HAL_EPROTO;
    }
    at = old->next_sibling;
  }
  return HAL_OK;
}

static hal_status_t value(parser_t *p, parse_frame_t *parent) {
  const size_t start = p->position;
  if (start == p->input.length) {
    return syntax(p);
  }
  if (p->count == p->limits->max_nodes ||
      (parent != NULL && parent->children == p->limits->max_members)) {
    return limit(p, start);
  }
  if (!p->measure && p->count == p->capacity) {
    return fail(p, HAL_EOVERFLOW, JB_JSON_ERROR_TOKEN_CAPACITY, start, p->count + 1);
  }
  const char c = p->input.data[start];
  const bool container = c == '[' || c == '{';
  if (container && p->depth == p->limits->max_depth) {
    return limit(p, start);
  }
  jb_json_type_t kind;
  hal_status_t status;
  if (container) {
    kind = c == '[' ? JB_JSON_ARRAY : JB_JSON_OBJECT;
    ++p->position;
    status = HAL_OK;
  } else if (c == '"') {
    kind = JB_JSON_STRING;
    size_t raw_offset, raw_length;
    status = scan_string(p, &raw_offset, &raw_length);
  } else if (c == '-' || (c >= '0' && c <= '9')) {
    kind = JB_JSON_NUMBER;
    status = scan_number(p);
  } else if (c == 'n') {
    kind = JB_JSON_NULL;
    status = literal(p, "null", 4);
  } else if (c == 't' || c == 'f') {
    kind = JB_JSON_BOOL;
    status = c == 't' ? literal(p, "true", 4) : literal(p, "false", 5);
  } else {
    return syntax(p);
  }
  if (status != HAL_OK) {
    return status;
  }
  const size_t index = p->count++;
  if (!p->measure) {
    const jb_json_token_t token = {kind,
                                   start,
                                   p->position - start,
                                   parent != NULL && parent->object ? parent->key_offset : 0,
                                   parent != NULL && parent->object ? parent->key_length : 0,
                                   parent != NULL ? parent->node : JB_JSON_NODE_NONE,
                                   JB_JSON_NODE_NONE,
                                   JB_JSON_NODE_NONE,
                                   0,
                                   parent != NULL && parent->object};
    p->tokens[index] = token;
    if (parent != NULL) {
      if (parent->last == JB_JSON_NODE_NONE) {
        p->tokens[parent->node].first_child = index;
      } else {
        p->tokens[parent->last].next_sibling = index;
      }
      ++p->tokens[parent->node].child_count;
    }
  }
  if (parent != NULL) {
    if (parent->children == 0) {
      parent->first = index;
    }
    parent->last = index;
    ++parent->children;
    parent->state = FRAME_AFTER;
    frame_write(p, p->depth - 1, parent);
  }
  if (container) {
    const parse_frame_t child = {index, 0, JB_JSON_NODE_NONE, JB_JSON_NODE_NONE,
                                 0,     0, FRAME_FIRST,       kind == JB_JSON_OBJECT};
    frame_write(p, p->depth, &child);
    ++p->depth;
    if (p->depth > p->deepest) {
      p->deepest = p->depth;
    }
  }
  return HAL_OK;
}

static hal_status_t run(parser_t *p) {
  whitespace(p);
  hal_status_t status = value(p, NULL);
  while (status == HAL_OK && p->depth != 0) {
    whitespace(p);
    if (p->position == p->input.length) {
      return syntax(p);
    }
    parse_frame_t frame;
    frame_read(p, p->depth - 1, &frame);
    const char c = p->input.data[p->position];
    if ((frame.state == FRAME_FIRST || frame.state == FRAME_AFTER) &&
        c == (frame.object ? '}' : ']')) {
      ++p->position;
      if (!p->measure) {
        p->tokens[frame.node].length = p->position - p->tokens[frame.node].offset;
      }
      --p->depth;
    } else if (frame.state == FRAME_AFTER) {
      if (c != ',') {
        return syntax(p);
      }
      ++p->position;
      frame.state = FRAME_REQUIRED;
      frame_write(p, p->depth - 1, &frame);
    } else if (frame.object && (frame.state == FRAME_FIRST || frame.state == FRAME_REQUIRED)) {
      if (c != '"') {
        return syntax(p);
      }
      status = scan_string(p, &frame.key_offset, &frame.key_length);
      if (status == HAL_OK) {
        status = unique_key(p, &frame);
      }
      frame.state = FRAME_COLON;
      frame_write(p, p->depth - 1, &frame);
    } else if (frame.state == FRAME_COLON) {
      if (c != ':') {
        return syntax(p);
      }
      ++p->position;
      frame.state = FRAME_VALUE;
      frame_write(p, p->depth - 1, &frame);
    } else {
      status = value(p, &frame);
    }
  }
  if (status != HAL_OK) {
    return status;
  }
  whitespace(p);
  if (p->position != p->input.length) {
    return fail(p, HAL_EPROTO, JB_JSON_ERROR_TRAILING_DATA, p->position, 0);
  }
  return HAL_OK;
}

hal_status_t jb_json_parse_workspace_size(const jb_json_limits_t *limits, size_t *out_bytes) {
  if (limits == NULL || out_bytes == NULL) {
    return HAL_EINVAL;
  }
  if (limits->max_depth > SIZE_MAX / sizeof(parse_frame_t)) {
    return HAL_EOVERFLOW;
  }
  *out_bytes = limits->max_depth * sizeof(parse_frame_t);
  return HAL_OK;
}

static void clear_error(jb_json_error_t *error) {
  if (error != NULL) {
    *error = (jb_json_error_t){JB_JSON_ERROR_NONE, 0, SIZE_MAX, 0};
  }
}

static hal_status_t prepare(parser_t *p) {
  clear_error(p->error);
  if (p->limits == NULL || (p->input.data == NULL && p->input.length != 0) ||
      (p->work.data == NULL && p->work.size != 0) ||
      (!p->measure && p->tokens == NULL && p->capacity != 0)) {
    return HAL_EINVAL;
  }
  if (p->input.length > p->limits->max_input_bytes) {
    return limit(p, 0);
  }
  size_t needed;
  const hal_status_t status = jb_json_parse_workspace_size(p->limits, &needed);
  if (status != HAL_OK) {
    return fail(p, status, JB_JSON_ERROR_WORKSPACE, 0, 0);
  }
  if (needed > p->work.size) {
    return fail(p, HAL_EOVERFLOW, JB_JSON_ERROR_WORKSPACE, 0, needed);
  }
  return HAL_OK;
}

hal_status_t jb_json_measure(jb_json_span_t input, const jb_json_limits_t *limits,
                             jb_json_workspace_t work, jb_json_requirements_t *out,
                             jb_json_error_t *error) {
  if (out == NULL) {
    clear_error(error);
    return HAL_EINVAL;
  }
  parser_t p = {0};
  p.input = input;
  p.limits = limits;
  p.work = work;
  p.error = error;
  p.measure = true;
  hal_status_t status = prepare(&p);
  if (status == HAL_OK) {
    status = run(&p);
  }
  if (status == HAL_OK) {
    *out = (jb_json_requirements_t){p.count, p.deepest};
  }
  return status;
}

hal_status_t jb_json_parse(jb_json_span_t input, const jb_json_limits_t *limits,
                           jb_json_token_t *tokens, size_t token_capacity, jb_json_workspace_t work,
                           jb_json_doc_t *out_doc, jb_json_error_t *error) {
  if (out_doc == NULL) {
    clear_error(error);
    return HAL_EINVAL;
  }
  *out_doc = (jb_json_doc_t){0};
  parser_t p = {0};
  p.input = input;
  p.limits = limits;
  p.tokens = tokens;
  p.capacity = token_capacity;
  p.work = work;
  p.error = error;
  hal_status_t status = prepare(&p);
  if (status == HAL_OK) {
    status = run(&p);
  }
  if (status == HAL_OK) {
    *out_doc = (jb_json_doc_t){input, tokens, p.count, true};
  }
  return status;
}

hal_status_t jb_json_locate(jb_json_span_t input, size_t offset, size_t *out_line,
                            size_t *out_column) {
  if ((input.data == NULL && input.length != 0) || offset > input.length || out_line == NULL ||
      out_column == NULL) {
    return HAL_EINVAL;
  }
  size_t line = 1, column = 1;
  for (size_t i = 0; i < offset; ++i) {
    if (input.data[i] == '\n') {
      if (line == SIZE_MAX) {
        return HAL_EOVERFLOW;
      }
      ++line;
      column = 1;
    } else {
      if (column == SIZE_MAX) {
        return HAL_EOVERFLOW;
      }
      ++column;
    }
  }
  *out_line = line;
  *out_column = column;
  return HAL_OK;
}
