#include "jb/endian.h"
#include "jb/json.h"
#include "jb/sha256.h"
#include "jb/utf8.h"
#include <jb/array.h>
#include <type_traits>

static_assert(std::is_standard_layout<jb_json_token_t>::value, "C token layout");
static_assert(std::is_same<decltype(&jb_json_root),
                           hal_status_t (*)(const jb_json_doc_t *, jb_json_node_t *)>::value,
              "shared HAL status type");
static_assert(std::is_standard_layout<jb_sha256_context_t>::value, "C SHA-256 context layout");

int main() {
  jb_sha256_context_t sha = {};
  uint8_t digest[JB_SHA256_DIGEST_BYTES];
  jb_sha256_init(&sha);
  jb_sha256_update(&sha, reinterpret_cast<const uint8_t *>("abc"), 3u);
  jb_sha256_final(&sha, digest);
  if (jb_load_be32(digest) != 0xba7816bfu || sha.initialized) {
    return 2;
  }
  static jb_json_token_t tokens[4];
  static unsigned char scratch[1024];
  const jb_json_limits_t limits = {32, COUNTOF(tokens), 2, 16, 16, 4, 64};
  jb_json_doc_t doc = {};
  const auto status = jb_json_parse({"{\"x\":true}", 10}, &limits, tokens, COUNTOF(tokens),
                                    {scratch, sizeof(scratch)}, &doc, nullptr);
  if (status != HAL_OK) {
    return 1;
  }
  jb_json_node_t root, value;
  bool boolean = false;
  return jb_json_root(&doc, &root) == HAL_OK && jb_json_find(root, {"x", 1}, &value) == HAL_OK &&
                 jb_json_get_bool(value, &boolean) == HAL_OK && boolean
             ? 0
             : 1;
}
