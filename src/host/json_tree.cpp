#include "jb/json_tree.h"
#include "support.h"
#include <algorithm>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

struct tree_node {
  jb_json_type_t type = JB_JSON_NULL;
  bool boolean = false, has_key = false;
  std::string key, text;
  std::vector<size_t> children;
};

struct jb_json_tree {
  jb_json_tree_limits_t limits{};
  std::vector<tree_node> nodes;
};

namespace {
using jb_host::boundary;
using jb_host::failure;
using view = jb_json_tree_view_t;

void require(hal_status_t status) {
  if (status != HAL_OK)
    throw failure{status};
}

void charge(size_t amount, size_t &used, size_t limit) {
  if (amount > limit - used)
    throw failure{HAL_EOVERFLOW};
  used += amount;
}

const tree_node &node(view value) {
  if (value.tree == nullptr || value.index >= value.tree->nodes.size())
    throw failure{HAL_ESTATE};
  return value.tree->nodes[value.index];
}

view root(const jb_json_tree_t *tree) {
  const view value{tree, 0};
  (void)node(value);
  return value;
}

void key_valid(jb_json_span_t key) {
  size_t bad;
  require(jb_utf8_validate(key.data, key.length, &bad));
}

bool equal_key(const std::string &left, jb_json_span_t right, size_t &work, size_t limit) {
  charge(1, work, limit);
  charge(left.size(), work, limit);
  charge(right.length, work, limit);
  return left.size() == right.length &&
         (right.length == 0 || std::memcmp(left.data(), right.data, right.length) == 0);
}

jb_json_span_t span(const std::string &text) { return {text.data(), text.size()}; }

size_t find(view object, jb_json_span_t key, size_t &work, size_t limit) {
  const auto &value = node(object);
  if (value.type != JB_JSON_OBJECT)
    throw failure{HAL_ETYPE};
  const auto found = std::find_if(value.children.begin(), value.children.end(), [&](size_t child) {
    return equal_key(object.tree->nodes[child].key, key, work, limit);
  });
  return found == value.children.end() ? SIZE_MAX : *found;
}

struct builder {
  std::unique_ptr<jb_json_tree_t> tree;
  size_t text_bytes = 0, compared = 0;

  explicit builder(const jb_json_tree_limits_t &limits) : tree(new jb_json_tree_t) {
    tree->limits = limits;
  }

  size_t add(const tree_node &source, const std::string &key, size_t parent, size_t depth) {
    if (tree->nodes.size() == tree->limits.max_nodes)
      throw failure{HAL_EOVERFLOW};
    const bool container = source.type == JB_JSON_ARRAY || source.type == JB_JSON_OBJECT;
    if (container && depth >= tree->limits.max_depth)
      throw failure{HAL_EOVERFLOW};
    charge(key.size(), text_bytes, tree->limits.max_text_bytes);
    charge(source.text.size(), text_bytes, tree->limits.max_text_bytes);
    tree_node copy;
    copy.type = source.type;
    copy.boolean = source.boolean;
    copy.has_key = parent != SIZE_MAX && tree->nodes[parent].type == JB_JSON_OBJECT;
    copy.key = key;
    copy.text = source.text;
    const size_t index = tree->nodes.size();
    tree->nodes.push_back(std::move(copy));
    if (parent != SIZE_MAX)
      tree->nodes[parent].children.push_back(index);
    return index;
  }

  void pending(size_t count, size_t queued) const {
    const size_t available = tree->limits.max_nodes - tree->nodes.size();
    if (queued > available || count > available - queued)
      throw failure{HAL_EOVERFLOW};
  }

  struct job {
    view left{}, right{};
    const std::string *key = nullptr;
    size_t parent = SIZE_MAX, depth = 0;
  };

  void build(std::vector<job> jobs) {
    static const std::string empty;
    while (!jobs.empty()) {
      const job next = jobs.back();
      jobs.pop_back();
      view selected = next.right.tree != nullptr ? next.right : next.left;
      const bool merge = next.left.tree != nullptr && next.right.tree != nullptr &&
                         node(next.left).type == JB_JSON_OBJECT &&
                         node(next.right).type == JB_JSON_OBJECT;
      const auto &source = node(selected);
      const size_t index =
          add(source, next.key == nullptr ? empty : *next.key, next.parent, next.depth);
      const size_t depth =
          next.depth + (source.type == JB_JSON_ARRAY || source.type == JB_JSON_OBJECT ? 1u : 0u);
      std::vector<job> children;
      if (merge) {
        const auto &base = node(next.left);
        for (size_t child : base.children) {
          const auto &key = next.left.tree->nodes[child].key;
          const size_t overlay =
              find(next.right, span(key), compared, tree->limits.max_key_compare_bytes);
          pending(children.size() + 1, jobs.size());
          children.push_back({{next.left.tree, child},
                              overlay == SIZE_MAX ? view{} : view{next.right.tree, overlay},
                              &key,
                              index,
                              depth});
        }
        for (size_t child : node(next.right).children) {
          const auto &key = next.right.tree->nodes[child].key;
          if (find(next.left, span(key), compared, tree->limits.max_key_compare_bytes) ==
              SIZE_MAX) {
            pending(children.size() + 1, jobs.size());
            children.push_back({{next.right.tree, child}, {}, &key, index, depth});
          }
        }
      } else {
        pending(source.children.size(), jobs.size());
        for (size_t child : source.children)
          children.push_back(
              {{selected.tree, child},
               {},
               source.type == JB_JSON_OBJECT ? &selected.tree->nodes[child].key : nullptr,
               index,
               depth});
      }
      pending(children.size(), jobs.size());
      jobs.insert(jobs.end(), children.rbegin(), children.rend());
    }
  }
};

std::string decoded(jb_json_node_t value, bool key, size_t limit) {
  const auto copy = key ? jb_json_key_copy : jb_json_string_copy;
  size_t length = 0;
  require(copy(value, nullptr, 0, &length));
  if (length > limit)
    throw failure{HAL_EOVERFLOW};
  std::string text(length, '\0');
  require(copy(value, text.data(), text.size(), &length));
  return text;
}

void import_core(builder &output, jb_json_node_t source) {
  struct job {
    jb_json_node_t source;
    size_t parent, depth;
    bool key;
  };
  std::vector<job> jobs{{source, SIZE_MAX, 0, false}};
  while (!jobs.empty()) {
    const job next = jobs.back();
    jobs.pop_back();
    tree_node value;
    require(jb_json_type(next.source, &value.type));
    const size_t limit = output.tree->limits.max_text_bytes - output.text_bytes;
    if (next.key)
      value.key = decoded(next.source, true, limit);
    if (value.type == JB_JSON_STRING) {
      value.text = decoded(next.source, false, limit - value.key.size());
    } else if (value.type == JB_JSON_NUMBER) {
      jb_json_span_t token{};
      require(jb_json_number_token(next.source, &token));
      if (token.length > limit - value.key.size())
        throw failure{HAL_EOVERFLOW};
      value.text.assign(token.data, token.length);
    } else if (value.type == JB_JSON_BOOL) {
      require(jb_json_get_bool(next.source, &value.boolean));
    }
    const size_t index = output.add(value, value.key, next.parent, next.depth);
    if (value.type != JB_JSON_ARRAY && value.type != JB_JSON_OBJECT)
      continue;
    size_t count = 0;
    require(jb_json_count(next.source, &count));
    output.pending(count, jobs.size());
    std::vector<job> children;
    jb_json_node_t child{};
    if (count != 0)
      require(jb_json_first(next.source, &child));
    for (size_t i = 0; i < count; ++i) {
      children.push_back({child, index, next.depth + 1, value.type == JB_JSON_OBJECT});
      if (i + 1 < count)
        require(jb_json_next(child, &child));
    }
    jobs.insert(jobs.end(), children.rbegin(), children.rend());
  }
}

void output_owner(jb_json_tree_t **out, const jb_json_tree_limits_t *limits) {
  if (out == nullptr || limits == nullptr)
    throw failure{HAL_EINVAL};
  if (*out != nullptr)
    throw failure{HAL_ESTATE};
}

enum class mutation { set, erase, append };

void modify(jb_json_tree_t *tree, mutation operation, jb_json_span_t key, view replacement) {
  const view source = root(tree);
  const auto &original = node(source);
  const jb_json_type_t expected = operation == mutation::append ? JB_JSON_ARRAY : JB_JSON_OBJECT;
  if (original.type != expected)
    throw failure{HAL_ETYPE};
  if (operation != mutation::erase)
    (void)node(replacement);
  if (operation != mutation::append)
    key_valid(key);
  builder result(tree->limits);
  const size_t target = operation == mutation::append ? SIZE_MAX
                                                      : find(source, key, result.compared,
                                                             tree->limits.max_key_compare_bytes);
  if (operation == mutation::erase && target == SIZE_MAX)
    throw failure{HAL_ENOENT};
  if (key.length > tree->limits.max_text_bytes)
    throw failure{HAL_EOVERFLOW};
  const std::string new_key = key.length == 0 ? std::string{} : std::string(key.data, key.length);
  const size_t parent = result.add(original, {}, SIZE_MAX, 0);
  std::vector<builder::job> jobs;
  for (size_t child : original.children) {
    if (child == target && operation == mutation::erase)
      continue;
    result.pending(jobs.size() + 1, 0);
    jobs.push_back({child == target ? replacement : view{tree, child},
                    {},
                    original.type == JB_JSON_OBJECT ? &tree->nodes[child].key : nullptr,
                    parent,
                    1});
  }
  if (operation == mutation::append || (operation == mutation::set && target == SIZE_MAX)) {
    result.pending(jobs.size() + 1, 0);
    jobs.push_back({replacement, {}, operation == mutation::set ? &new_key : nullptr, parent, 1});
  }
  result.pending(jobs.size(), 0);
  std::reverse(jobs.begin(), jobs.end());
  result.build(std::move(jobs));
  tree->nodes.swap(result.tree->nodes);
}

struct formatter {
  const jb_json_tree_format_t &options;
  jb_json_sink_fn sink;
  void *user;
  size_t &written;
  size_t compared = 0;
  std::vector<unsigned char> scratch;

  void emit(const char *data, size_t length) {
    if (length > options.max_output_bytes - written)
      throw failure{HAL_EOVERFLOW};
    if (length != 0) {
      const auto status = sink(user, data, length);
      if (status != HAL_OK)
        throw failure{status <= HAL_EINVAL && status >= HAL_ETYPE ? status : HAL_EINTERNAL};
      written += length;
    }
  }
  void emit(const std::string &text) { emit(text.data(), text.size()); }

  void newline(size_t depth) {
    if (options.indent == 0)
      return;
    emit("\n", 1);
    if (depth > (options.max_output_bytes - written) / options.indent)
      throw failure{HAL_EOVERFLOW};
    emit(std::string(depth * options.indent, ' '));
  }

  static hal_status_t collect(void *context, const char *data, size_t length) {
    return boundary([&] { static_cast<std::string *>(context)->append(data, length); });
  }

  void hex(uint32_t unit) {
    static const char digits[] = "0123456789abcdef";
    const char escaped[] = {'\\',
                            'u',
                            digits[(unit >> 12) & 15],
                            digits[(unit >> 8) & 15],
                            digits[(unit >> 4) & 15],
                            digits[unit & 15]};
    emit(escaped, sizeof(escaped));
  }

  void scalar(const tree_node &value) {
    if (value.type == JB_JSON_NULL) {
      emit("null", 4);
      return;
    }
    if (value.type == JB_JSON_BOOL) {
      emit(value.boolean ? "true" : "false", value.boolean ? 4u : 5u);
      return;
    }
    if (value.type == JB_JSON_NUMBER) {
      emit(value.text);
      return;
    }
    quoted(value.text);
  }

  void quoted(const std::string &text) {
    if (text.size() > (SIZE_MAX - 2) / 6)
      throw failure{HAL_EOVERFLOW};
    const jb_json_writer_limits_t limits{text.size() * 6 + 2, 0, 0, 0, 0};
    size_t needed = 0;
    require(jb_json_writer_workspace_size(&limits, &needed));
    scratch.resize(needed);
    jb_json_writer_t writer{};
    std::string encoded;
    require(
        jb_json_writer_init(&writer, &limits, {scratch.data(), scratch.size()}, collect, &encoded));
    require(jb_json_writer_string(&writer, span(text)));
    require(jb_json_writer_finish(&writer, &needed));
    for (size_t position = 0; position < encoded.size();) {
      const unsigned char byte = static_cast<unsigned char>(encoded[position]);
      if (byte == '\\') {
        if (position + 5 < encoded.size() && encoded.compare(position, 4, "\\u00") == 0) {
          const std::string code = encoded.substr(position + 4, 2);
          const char *short_escape = code == "08"   ? "\\b"
                                     : code == "0c" ? "\\f"
                                     : code == "0a" ? "\\n"
                                     : code == "0d" ? "\\r"
                                     : code == "09" ? "\\t"
                                                    : nullptr;
          if (short_escape != nullptr) {
            emit(short_escape, 2);
            position += 6;
            continue;
          }
        }
        emit(encoded.data() + position, 2);
        position += 2;
      } else if (options.ensure_ascii && byte >= 0x7f) {
        uint32_t codepoint = 0;
        size_t consumed = 0;
        require(jb_utf8_decode(encoded.data() + position, encoded.size() - position, &codepoint,
                               &consumed));
        if (codepoint <= 0xffff) {
          hex(codepoint);
        } else {
          const uint32_t pair = codepoint - 0x10000;
          hex(0xd800 + (pair >> 10));
          hex(0xdc00 + (pair & 0x3ff));
        }
        position += consumed;
      } else {
        emit(encoded.data() + position, 1);
        ++position;
      }
    }
  }

  void write(view value) {
    struct frame {
      view value;
      size_t depth = 0, next = 0;
      bool started = false;
      std::vector<size_t> order;
    };
    std::vector<frame> stack{{value, 0, 0, false, {}}};
    while (!stack.empty()) {
      auto &top = stack.back();
      const auto &current = node(top.value);
      const bool object = current.type == JB_JSON_OBJECT;
      if (!object && current.type != JB_JSON_ARRAY) {
        scalar(current);
        stack.pop_back();
        continue;
      }
      if (!top.started) {
        top.order = current.children;
        if (object && options.sort_keys) {
          std::stable_sort(top.order.begin(), top.order.end(), [&](size_t left, size_t right) {
            const auto &a = top.value.tree->nodes[left].key;
            const auto &b = top.value.tree->nodes[right].key;
            const size_t limit = value.tree->limits.max_key_compare_bytes;
            charge(1, compared, limit);
            charge(a.size(), compared, limit);
            charge(b.size(), compared, limit);
            return a < b;
          });
        }
        emit(object ? "{" : "[", 1);
        top.started = true;
      }
      if (top.next == top.order.size()) {
        if (!top.order.empty())
          newline(top.depth);
        emit(object ? "}" : "]", 1);
        stack.pop_back();
        continue;
      }
      if (top.next != 0)
        emit(",", 1);
      newline(top.depth + 1);
      const size_t child = top.order[top.next++];
      if (object) {
        quoted(top.value.tree->nodes[child].key);
        emit(options.indent == 0 ? ":" : ": ", options.indent == 0 ? 1u : 2u);
      }
      stack.push_back({{top.value.tree, child}, top.depth + 1, 0, false, {}});
    }
  }
};
} // namespace

extern "C" {
hal_status_t jb_json_tree_create(jb_json_node_t source, const jb_json_tree_limits_t *limits,
                                 jb_json_tree_t **out) {
  return boundary([&] {
    output_owner(out, limits);
    builder result(*limits);
    import_core(result, source);
    *out = result.tree.release();
  });
}

hal_status_t jb_json_tree_copy(view source, const jb_json_tree_limits_t *limits,
                               jb_json_tree_t **out) {
  return boundary([&] {
    output_owner(out, limits);
    (void)node(source);
    builder result(*limits);
    result.build({{source, {}, nullptr, SIZE_MAX, 0}});
    *out = result.tree.release();
  });
}

void jb_json_tree_destroy(jb_json_tree_t *tree) { delete tree; }

hal_status_t jb_json_tree_root(const jb_json_tree_t *tree, view *out) {
  if (tree == nullptr || out == nullptr)
    return HAL_EINVAL;
  return boundary([&] { *out = root(tree); });
}

hal_status_t jb_json_tree_type(view value, jb_json_type_t *out) {
  if (out == nullptr)
    return HAL_EINVAL;
  return boundary([&] { *out = node(value).type; });
}

hal_status_t jb_json_tree_count(view value, size_t *out) {
  if (out == nullptr)
    return HAL_EINVAL;
  return boundary([&] {
    const auto &entry = node(value);
    if (entry.type != JB_JSON_ARRAY && entry.type != JB_JSON_OBJECT)
      throw failure{HAL_ETYPE};
    *out = entry.children.size();
  });
}

hal_status_t jb_json_tree_find(view object, jb_json_span_t key, view *out) {
  if (out == nullptr)
    return HAL_EINVAL;
  return boundary([&] {
    (void)node(object);
    key_valid(key);
    size_t work = 0;
    const size_t index = find(object, key, work, object.tree->limits.max_key_compare_bytes);
    if (index == SIZE_MAX)
      throw failure{HAL_ENOENT};
    *out = {object.tree, index};
  });
}

hal_status_t jb_json_tree_at(view array, size_t index, view *out) {
  if (out == nullptr)
    return HAL_EINVAL;
  return boundary([&] {
    const auto &entry = node(array);
    if (entry.type != JB_JSON_ARRAY && entry.type != JB_JSON_OBJECT)
      throw failure{HAL_ETYPE};
    if (index >= entry.children.size())
      throw failure{HAL_ENOENT};
    *out = {array.tree, entry.children[index]};
  });
}

hal_status_t jb_json_tree_key(view member, jb_json_span_t *out) {
  if (out == nullptr)
    return HAL_EINVAL;
  return boundary([&] {
    const auto &entry = node(member);
    if (!entry.has_key)
      throw failure{HAL_EINVAL};
    *out = span(entry.key);
  });
}

hal_status_t jb_json_tree_text(view value, jb_json_span_t *out) {
  if (out == nullptr)
    return HAL_EINVAL;
  return boundary([&] {
    const auto &entry = node(value);
    if (entry.type != JB_JSON_STRING && entry.type != JB_JSON_NUMBER)
      throw failure{HAL_ETYPE};
    *out = span(entry.text);
  });
}

hal_status_t jb_json_tree_get_bool(view value, bool *out) {
  if (out == nullptr)
    return HAL_EINVAL;
  return boundary([&] {
    const auto &entry = node(value);
    if (entry.type != JB_JSON_BOOL)
      throw failure{HAL_ETYPE};
    *out = entry.boolean;
  });
}

hal_status_t jb_json_tree_merge(jb_json_tree_t *tree, view overlay) {
  if (tree == nullptr)
    return HAL_EINVAL;
  return boundary([&] {
    (void)node(overlay);
    builder result(tree->limits);
    result.build({{root(tree), overlay, nullptr, SIZE_MAX, 0}});
    tree->nodes.swap(result.tree->nodes);
  });
}

hal_status_t jb_json_tree_set(jb_json_tree_t *tree, jb_json_span_t key, view value) {
  if (tree == nullptr)
    return HAL_EINVAL;
  return boundary([&] { modify(tree, mutation::set, key, value); });
}

hal_status_t jb_json_tree_erase(jb_json_tree_t *tree, jb_json_span_t key) {
  if (tree == nullptr)
    return HAL_EINVAL;
  return boundary([&] { modify(tree, mutation::erase, key, {}); });
}

hal_status_t jb_json_tree_append(jb_json_tree_t *tree, view value) {
  if (tree == nullptr)
    return HAL_EINVAL;
  return boundary([&] { modify(tree, mutation::append, {}, value); });
}

hal_status_t jb_json_tree_format(view value, const jb_json_tree_format_t *options,
                                 jb_json_sink_fn sink, void *user, size_t *written) {
  if (options == nullptr || sink == nullptr || written == nullptr ||
      (options->indent != 0 && options->indent != 2 && options->indent != 4))
    return HAL_EINVAL;
  *written = 0;
  return boundary([&] {
    (void)node(value);
    formatter output{*options, sink, user, *written, 0, {}};
    output.write(value);
  });
}
} // extern "C"
