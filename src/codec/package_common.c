/* What the package reader and writer share: keys, fields, and result names. */
#include "package_internal.h"

#include "badger/sha256.h"

#include <string.h>

const uint8_t tb_package_nonce[TB_CHACHA20_NONCE_SIZE] = {0};

void tb_package_wipe(void *memory, size_t length)
{
    /* Stores through a volatile pointer may not be removed as dead. (This is
     * not synchronization, for which volatile is never used.) */
    volatile unsigned char *bytes = memory;
    while (length > 0)
    {
        *bytes = 0;
        bytes++;
        length--;
    }
}

void tb_package_derive_key(const uint8_t game_key[TB_PACKAGE_KEY_SIZE],
                           const uint8_t salt[TB_PACKAGE_SALT_SIZE],
                           uint8_t package_key[TB_CHACHA20_KEY_SIZE],
                           uint8_t check[TB_PACKAGE_CHECK_SIZE])
{
    tb_hmac_sha256_state mac = {0};
    uint8_t full_check[TB_SHA256_SIZE] = {0};
    tb_hmac_sha256_init(&mac, game_key, TB_PACKAGE_KEY_SIZE);
    tb_hmac_sha256_update(&mac, TB_PACKAGE_KEY_LABEL, sizeof(TB_PACKAGE_KEY_LABEL) - 1);
    tb_hmac_sha256_update(&mac, salt, TB_PACKAGE_SALT_SIZE);
    tb_hmac_sha256_final(&mac, package_key);
    tb_hmac_sha256(package_key, TB_CHACHA20_KEY_SIZE, TB_PACKAGE_CHECK_LABEL,
                   sizeof(TB_PACKAGE_CHECK_LABEL) - 1, full_check);
    memcpy(check, full_check, TB_PACKAGE_CHECK_SIZE);
    tb_package_wipe(full_check, sizeof(full_check));
}

uint16_t tb_package_get16(const unsigned char *bytes)
{
    return (uint16_t)(bytes[0] | (unsigned)bytes[1] << 8);
}

uint32_t tb_package_get32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
           (uint32_t)bytes[3] << 24;
}

uint64_t tb_package_get64(const unsigned char *bytes)
{
    return (uint64_t)tb_package_get32(bytes) | (uint64_t)tb_package_get32(bytes + 4) << 32;
}

void tb_package_put16(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFu);
    bytes[1] = (unsigned char)((value >> 8) & 0xFFu);
}

void tb_package_put32(unsigned char *bytes, uint32_t value)
{
    for (size_t index = 0; index < 4; index++)
    {
        bytes[index] = (unsigned char)((value >> (8 * index)) & 0xFFu);
    }
}

void tb_package_put64(unsigned char *bytes, uint64_t value)
{
    tb_package_put32(bytes, (uint32_t)(value & 0xFFFFFFFFu));
    tb_package_put32(bytes + 4, (uint32_t)(value >> 32));
}

const char *tb_package_result_name(tb_package_result result)
{
    switch (result)
    {
    case TB_PACKAGE_OK:
        return "ok";
    case TB_PACKAGE_INVALID:
        return "invalid";
    case TB_PACKAGE_MALFORMED:
        return "malformed";
    case TB_PACKAGE_UNSUPPORTED:
        return "unsupported";
    case TB_PACKAGE_TOO_LARGE:
        return "too large";
    case TB_PACKAGE_KEY_REQUIRED:
        return "key required";
    case TB_PACKAGE_WRONG_KEY:
        return "wrong key";
    case TB_PACKAGE_DUPLICATE_NAME:
        return "duplicate name";
    case TB_PACKAGE_RANDOM_FAILED:
        return "random failed";
    case TB_PACKAGE_INPUT_FAILED:
        return "input failed";
    case TB_PACKAGE_OUTPUT_FAILED:
        return "output failed";
    default:
        return "unknown result";
    }
}
