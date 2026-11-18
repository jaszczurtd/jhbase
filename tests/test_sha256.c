#include "jb/sha256.h"
#include "unity.h"
#include <jb/array.h>
#include <string.h>

/* Digests: FIPS 180-2 examples and NIST test vectors; the padding-boundary and
 * split cases were computed with Python hashlib over the byte pattern below. */
static const char empty_digest[] =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
static const char abc_digest[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
static const char pattern_1000_digest[] =
    "a8af099bf2e878609558dbf69d8f88f4a31040a8cf84b549a0cfa912f12ffc3f";

static uint8_t pattern[1000 + 8];

void setUp(void) {
  for (size_t i = 0u; i < sizeof(pattern); ++i) {
    pattern[i] = (uint8_t)i;
  }
}
void tearDown(void) {}

static void expect_digest(const char *hex, const uint8_t digest[JB_SHA256_DIGEST_BYTES]) {
  uint8_t expected[JB_SHA256_DIGEST_BYTES];
  TEST_ASSERT_EQUAL_size_t(2u * JB_SHA256_DIGEST_BYTES, strlen(hex));
  for (size_t i = 0u; i < JB_SHA256_DIGEST_BYTES; ++i) {
    unsigned value = 0u;
    for (size_t j = 0u; j < 2u; ++j) {
      const char c = hex[2u * i + j];
      value = value * 16u + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    expected[i] = (uint8_t)value;
  }
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, digest, JB_SHA256_DIGEST_BYTES);
}

static void digest_of(const uint8_t *data, size_t length, uint8_t digest[JB_SHA256_DIGEST_BYTES]) {
  jb_sha256_context_t context;
  jb_sha256_init(&context);
  jb_sha256_update(&context, data, length);
  jb_sha256_final(&context, digest);
}

static void test_fips_examples(void) {
  static const char two_blocks[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  static const char long_two_blocks[] =
      "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmno"
      "pqrsmnopqrstnopqrstu";
  uint8_t digest[JB_SHA256_DIGEST_BYTES];
  digest_of(NULL, 0u, digest);
  expect_digest(empty_digest, digest);
  digest_of((const uint8_t *)"abc", 3u, digest);
  expect_digest(abc_digest, digest);
  digest_of((const uint8_t *)two_blocks, sizeof(two_blocks) - 1u, digest);
  expect_digest("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", digest);
  digest_of((const uint8_t *)long_two_blocks, sizeof(long_two_blocks) - 1u, digest);
  expect_digest("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1", digest);
}

static void test_million_a_in_chunks(void) {
  static uint8_t chunk[1000];
  memset(chunk, 'a', sizeof(chunk));
  jb_sha256_context_t context;
  jb_sha256_init(&context);
  for (size_t i = 0u; i < 1000u; ++i) {
    jb_sha256_update(&context, chunk, sizeof(chunk));
  }
  uint8_t digest[JB_SHA256_DIGEST_BYTES];
  jb_sha256_final(&context, digest);
  expect_digest("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", digest);
}

static void test_padding_boundaries(void) {
  static const struct {
    size_t length;
    const char *digest;
  } cases[] = {
      {55u, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
      {56u, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
      {57u, "2fe741af801cc238602ac0ec6a7b0c3a8a87c7fc7d7f02a3fe03d1c12eac4d8f"},
      {63u, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
      {64u, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
      {65u, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
      {119u, "da18797ed7c3a777f0847f429724a2d8cd5138e6ed2895c3fa1a6d39d18f7ec6"},
      {120u, "f52b23db1fbb6ded89ef42a23ce0c8922c45f25c50b568a93bf1c075420bbb7c"},
  };
  for (size_t i = 0u; i < COUNTOF(cases); ++i) {
    uint8_t digest[JB_SHA256_DIGEST_BYTES];
    digest_of(pattern, cases[i].length, digest);
    expect_digest(cases[i].digest, digest);
  }
}

static void test_any_split_gives_the_same_digest(void) {
  uint8_t digest[JB_SHA256_DIGEST_BYTES];
  for (size_t split = 0u; split <= 1000u; ++split) {
    jb_sha256_context_t context;
    jb_sha256_init(&context);
    jb_sha256_update(&context, pattern, split);
    jb_sha256_update(&context, pattern + split, 1000u - split);
    jb_sha256_final(&context, digest);
    expect_digest(pattern_1000_digest, digest);
  }
  static const size_t chunks[] = {1u, 3u, 55u, 63u, 64u, 65u, 127u, 999u};
  for (size_t i = 0u; i < COUNTOF(chunks); ++i) {
    jb_sha256_context_t context;
    jb_sha256_init(&context);
    for (size_t done = 0u; done < 1000u;) {
      const size_t take = (1000u - done < chunks[i]) ? 1000u - done : chunks[i];
      jb_sha256_update(&context, pattern + done, take);
      done += take;
    }
    jb_sha256_final(&context, digest);
    expect_digest(pattern_1000_digest, digest);
  }
}

static void test_unaligned_input_and_digest(void) {
  static uint8_t shifted[1000 + 8];
  uint8_t output[JB_SHA256_DIGEST_BYTES + 8];
  for (size_t offset = 1u; offset < 8u; ++offset) {
    memcpy(shifted + offset, pattern, 1000u);
    memset(output, 0xA5, sizeof(output));
    digest_of(shifted + offset, 1000u, output + offset);
    expect_digest(pattern_1000_digest, output + offset);
    for (size_t i = 0u; i < offset; ++i) {
      TEST_ASSERT_EQUAL_UINT8(0xA5, output[i]);
    }
    for (size_t i = offset + JB_SHA256_DIGEST_BYTES; i < sizeof(output); ++i) {
      TEST_ASSERT_EQUAL_UINT8(0xA5, output[i]);
    }
  }
}

static void test_context_lifecycle(void) {
  jb_sha256_context_t context, before;
  memset(&context, 0, sizeof(context));
  jb_sha256_init(&context);
  TEST_ASSERT_TRUE(context.initialized);
  jb_sha256_update(&context, (const uint8_t *)"ab", 2u);
  memcpy(&before, &context, sizeof(context));
  jb_sha256_update(&context, NULL, 0u);
  TEST_ASSERT_EQUAL_MEMORY(&before, &context, sizeof(context));
  jb_sha256_update(&context, (const uint8_t *)"c", 1u);
  uint8_t digest[JB_SHA256_DIGEST_BYTES];
  jb_sha256_final(&context, digest);
  expect_digest(abc_digest, digest);
  TEST_ASSERT_FALSE(context.initialized);

  /* A finished context starts the next digest from scratch. */
  jb_sha256_init(&context);
  TEST_ASSERT_TRUE(context.initialized);
  jb_sha256_final(&context, digest);
  expect_digest(empty_digest, digest);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_fips_examples);
  RUN_TEST(test_million_a_in_chunks);
  RUN_TEST(test_padding_boundaries);
  RUN_TEST(test_any_split_gives_the_same_digest);
  RUN_TEST(test_unaligned_input_and_digest);
  RUN_TEST(test_context_lifecycle);
  return UNITY_END();
}
