/* SHA-256 and HMAC-SHA256 against answers from Python's hashlib and hmac
 * and the published examples of FIPS 180-2 and RFC 4231: every input length
 * where padding changes, the input in pieces of every size, keys around the
 * block size, and the contracts. */
#include "badger/assert.h"
#include "badger/sha256.h"
#include "tb_test.h"

#include "sha256_vectors.h"

#include <string.h>

static void pattern(unsigned char *bytes, size_t length)
{
    for (size_t index = 0; index < length; index++)
    {
        bytes[index] = (unsigned char)((index * 31 + 7) & 0xFFu);
    }
}

TB_TEST(codec_sha256, every_length_through_padding_hashes_as_python_does)
{
    static unsigned char input[301];
    pattern(input, sizeof(input));
    for (size_t index = 0; index < TB_TEST_COUNT_OF(sha256_pattern_vectors); index++)
    {
        const sha256_pattern_vector *const vector = &sha256_pattern_vectors[index];
        uint8_t digest[TB_SHA256_SIZE] = {0};
        TB_TEST_CONTEXT("length %zu", vector->length);
        tb_sha256(vector->length == 0 ? NULL : input, vector->length, digest);
        TB_TEST_EXPECT_BYTES(digest, vector->digest, TB_SHA256_SIZE);
    }
}

TB_TEST(codec_sha256, the_examples_of_fips_180_2)
{
    static unsigned char million[1000000];
    uint8_t digest[TB_SHA256_SIZE] = {0};
    static const char two_blocks[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    tb_sha256("abc", 3, digest);
    TB_TEST_EXPECT_BYTES(digest, sha256_abc, TB_SHA256_SIZE);
    tb_sha256(two_blocks, sizeof(two_blocks) - 1, digest);
    TB_TEST_EXPECT_BYTES(digest, sha256_two_blocks, TB_SHA256_SIZE);
    memset(million, 'a', sizeof(million));
    tb_sha256(million, sizeof(million), digest);
    TB_TEST_EXPECT_BYTES(digest, sha256_million_a, TB_SHA256_SIZE);
}

/* Every split of 300 bytes into two pieces, and pieces of every size from 1
 * to 130, give the one-call digest. */
TB_TEST(codec_sha256, pieces_of_any_size_hash_as_the_whole)
{
    static unsigned char input[300];
    const uint8_t *const expected = sha256_pattern_vectors[300].digest;
    pattern(input, sizeof(input));
    for (size_t split = 0; split <= sizeof(input); split++)
    {
        tb_sha256_state hash = {0};
        uint8_t digest[TB_SHA256_SIZE] = {0};
        TB_TEST_CONTEXT("split at %zu", split);
        tb_sha256_init(&hash);
        tb_sha256_update(&hash, input, split);
        tb_sha256_update(&hash, input + split, sizeof(input) - split);
        tb_sha256_final(&hash, digest);
        TB_TEST_EXPECT_BYTES(digest, expected, TB_SHA256_SIZE);
    }
    for (size_t step = 1; step <= 130; step++)
    {
        tb_sha256_state hash = {0};
        uint8_t digest[TB_SHA256_SIZE] = {0};
        TB_TEST_CONTEXT("pieces of %zu", step);
        tb_sha256_init(&hash);
        for (size_t at = 0; at < sizeof(input); at += step)
        {
            tb_sha256_update(&hash, input + at,
                             sizeof(input) - at < step ? sizeof(input) - at : step);
        }
        tb_sha256_final(&hash, digest);
        TB_TEST_EXPECT_BYTES(digest, expected, TB_SHA256_SIZE);
    }
}

TB_TEST(codec_sha256, a_finished_state_is_cleared_and_can_start_again)
{
    tb_sha256_state hash = {0};
    static const tb_sha256_state cleared = {0};
    uint8_t digest[TB_SHA256_SIZE] = {0};
    tb_sha256_init(&hash);
    tb_sha256_update(&hash, "secret", 6);
    tb_sha256_final(&hash, digest);
    TB_TEST_EXPECT_BYTES(&hash, &cleared, sizeof(hash));
    tb_sha256_init(&hash);
    tb_sha256_update(&hash, "abc", 3);
    tb_sha256_final(&hash, digest);
    TB_TEST_EXPECT_BYTES(digest, sha256_abc, TB_SHA256_SIZE);
}

TB_TEST(codec_hmac_sha256, rfc_4231_and_keys_around_the_block_size)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(hmac_vectors); index++)
    {
        const hmac_vector *const vector = &hmac_vectors[index];
        uint8_t mac[TB_SHA256_SIZE] = {0};
        tb_hmac_sha256_state state = {0};
        TB_TEST_CONTEXT("case %zu, key of %zu", index, vector->key_length);
        tb_hmac_sha256(vector->key, vector->key_length, vector->data, vector->data_length, mac);
        TB_TEST_EXPECT_BYTES(mac, vector->mac, vector->compared);
        /* In pieces of one byte, the same. */
        tb_hmac_sha256_init(&state, vector->key, vector->key_length);
        for (size_t at = 0; at < vector->data_length; at++)
        {
            tb_hmac_sha256_update(&state, vector->data + at, 1);
        }
        tb_hmac_sha256_final(&state, mac);
        TB_TEST_EXPECT_BYTES(mac, vector->mac, vector->compared);
    }
}

static void sha256_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    tb_sha256_state hash = {0};
    tb_hmac_sha256_state mac = {0};
    uint8_t digest[TB_SHA256_SIZE] = {0};
    switch (choice)
    {
    case 0:
        tb_sha256(NULL, 1, digest);
        break;
    case 1:
        tb_sha256_init(&hash);
        tb_sha256_update(&hash, NULL, 1);
        break;
    case 2:
        tb_sha256_init(NULL);
        break;
    case 3:
        tb_sha256_init(&hash);
        tb_sha256_final(&hash, NULL);
        break;
    default:
        tb_hmac_sha256_init(&mac, NULL, 1);
        break;
    }
}

TB_TEST(codec_sha256, contract_violations_are_assertions)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = 0; choice < 5; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(sha256_contract, &choice, "assertion failed");
    }
}
