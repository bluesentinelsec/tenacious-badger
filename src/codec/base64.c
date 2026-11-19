/* Base64 (RFC 4648). Decoding checks all of the text, and measures it,
 * before it writes a byte, so a failure writes nothing. */
#include "badger/base64.h"

#include "badger/assert.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TB_BASE64_KNOWN_FLAGS                                                                      \
    ((unsigned)TB_BASE64_URL_SAFE | (unsigned)TB_BASE64_NO_PADDING |                               \
     (unsigned)TB_BASE64_SKIP_WHITESPACE)
#define TB_BASE64_PAD '='
/* What a character means to the decoder, beyond its value of 0 to 63. */
#define TB_BASE64_NOT_IN_ALPHABET 0xFFu
#define TB_BASE64_PADDING 0xFEu
#define TB_BASE64_WHITESPACE 0xFDu

static const char tb_base64_standard[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char tb_base64_url[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static const char *tb_base64_alphabet(unsigned flags)
{
    if ((flags & TB_BASE64_URL_SAFE) != 0)
    {
        return tb_base64_url;
    }
    return tb_base64_standard;
}

/* What each byte means in the alphabet that `flags` choose: a value from 0
 * to 63, padding, whitespace, or nothing. Built for each call; it costs less
 * than a few characters of searching. */
static void tb_base64_decoding_table(unsigned flags, unsigned char table[256])
{
    static const char whitespace[] = " \t\n\r\v\f";
    const char *const alphabet = tb_base64_alphabet(flags);
    memset(table, TB_BASE64_NOT_IN_ALPHABET, 256);
    for (size_t index = 0; index < sizeof(whitespace) - 1; index++)
    {
        table[(unsigned char)whitespace[index]] = TB_BASE64_WHITESPACE;
    }
    table[(unsigned char)TB_BASE64_PAD] = TB_BASE64_PADDING;
    for (unsigned value = 0; value < 64; value++)
    {
        table[(unsigned char)alphabet[value]] = (unsigned char)value;
    }
}

tb_base64_result tb_base64_encoded_size(size_t length, unsigned flags, size_t *size)
{
    const size_t groups = length / 3;
    const size_t remainder = length % 3;
    size_t characters = 0;
    TB_ASSERT(size != NULL);
    if ((flags & ~TB_BASE64_KNOWN_FLAGS) != 0)
    {
        return TB_BASE64_INVALID;
    }
    if (groups > (SIZE_MAX - 4) / 4)
    {
        return TB_BASE64_TOO_LARGE;
    }
    characters = groups * 4;
    if (remainder > 0)
    {
        characters += (flags & TB_BASE64_NO_PADDING) != 0 ? remainder + 1 : 4;
    }
    *size = characters;
    return TB_BASE64_OK;
}

size_t tb_base64_decoded_size_bound(size_t text_length)
{
    /* Three bytes for every four characters, and two for a last three. */
    return text_length / 4 * 3 + (text_length % 4 * 3 + 3) / 4;
}

tb_base64_result tb_base64_encode(const void *bytes, size_t length, unsigned flags, char *text,
                                  size_t capacity, size_t *written)
{
    const unsigned char *const input = bytes;
    const char *const alphabet = tb_base64_alphabet(flags);
    size_t needed = 0;
    size_t in = 0;
    size_t out = 0;
    tb_base64_result result = TB_BASE64_OK;
    TB_ASSERT(bytes != NULL || length == 0);
    TB_ASSERT(text != NULL || capacity == 0);
    TB_ASSERT(written != NULL);
    *written = 0;
    result = tb_base64_encoded_size(length, flags, &needed);
    if (result != TB_BASE64_OK)
    {
        return result;
    }
    if (capacity < needed)
    {
        *written = needed;
        return TB_BASE64_BUFFER_TOO_SMALL;
    }
    if (length == 0)
    {
        return TB_BASE64_OK;
    }
    /* Some text is needed, and the room for it was given. */
    TB_ASSERT(text != NULL && input != NULL);
    while (length - in >= 3)
    {
        const uint32_t group =
            ((uint32_t)input[in] << 16) | ((uint32_t)input[in + 1] << 8) | (uint32_t)input[in + 2];
        text[out] = alphabet[(group >> 18) & 63u];
        text[out + 1] = alphabet[(group >> 12) & 63u];
        text[out + 2] = alphabet[(group >> 6) & 63u];
        text[out + 3] = alphabet[group & 63u];
        in += 3;
        out += 4;
    }
    if (length - in > 0)
    {
        const bool two = length - in == 2;
        const uint32_t group =
            ((uint32_t)input[in] << 16) | (two ? (uint32_t)input[in + 1] << 8 : 0u);
        text[out] = alphabet[(group >> 18) & 63u];
        text[out + 1] = alphabet[(group >> 12) & 63u];
        out += 2;
        if (two)
        {
            text[out] = alphabet[(group >> 6) & 63u];
            out++;
        }
        if ((flags & TB_BASE64_NO_PADDING) == 0)
        {
            text[out] = TB_BASE64_PAD;
            out++;
            if (!two)
            {
                text[out] = TB_BASE64_PAD;
                out++;
            }
        }
    }
    TB_ASSERT(out == needed);
    *written = out;
    return TB_BASE64_OK;
}

/* Walk the text, checking it and, when `bytes` is given, writing what it
 * decodes to. Returns the decoded length in `decoded`. */
static tb_base64_result tb_base64_walk(const char *text, size_t length, unsigned flags,
                                       unsigned char *bytes, size_t *decoded, size_t *error_offset)
{
    const bool padded = (flags & TB_BASE64_NO_PADDING) == 0;
    const bool skip_whitespace = (flags & TB_BASE64_SKIP_WHITESPACE) != 0;
    uint32_t group = 0;
    unsigned in_group = 0;  /* Characters of the current group. */
    unsigned padding = 0;   /* `=` seen; once any are, only more may follow. */
    size_t last_offset = 0; /* The offset of the last character of the alphabet. */
    size_t out = 0;
    unsigned char table[256] = {0};
    tb_base64_decoding_table(flags, table);
    for (size_t offset = 0; offset < length; offset++)
    {
        const unsigned meaning = table[(unsigned char)text[offset]];
        if (meaning == TB_BASE64_WHITESPACE && skip_whitespace)
        {
            continue;
        }
        if (meaning == TB_BASE64_PADDING)
        {
            /* Padding only in padded text, only after two or three characters of a group. */
            if (!padded || in_group + padding < 2 || in_group + padding >= 4)
            {
                *error_offset = offset;
                return TB_BASE64_INVALID;
            }
            padding++;
            continue;
        }
        if (meaning > 63 || padding > 0)
        {
            *error_offset = offset;
            return TB_BASE64_INVALID;
        }
        group = (group << 6) | meaning;
        in_group++;
        last_offset = offset;
        if (in_group == 4)
        {
            if (bytes != NULL)
            {
                bytes[out] = (unsigned char)(group >> 16);
                bytes[out + 1] = (unsigned char)(group >> 8);
                bytes[out + 2] = (unsigned char)group;
            }
            out += 3;
            group = 0;
            in_group = 0;
        }
    }
    if (in_group == 0 && padding == 0)
    {
        *decoded = out;
        return TB_BASE64_OK;
    }
    /* A last group of one character holds no whole byte. */
    if (in_group == 1 || (padded && in_group + padding != 4))
    {
        *error_offset = length;
        return TB_BASE64_INVALID;
    }
    /* The bits past the last whole byte must be zero, so each byte string
     * has exactly one encoding. */
    {
        const unsigned spare_bits = in_group == 2 ? 4u : 2u;
        if ((group & ((1u << spare_bits) - 1u)) != 0)
        {
            *error_offset = last_offset;
            return TB_BASE64_INVALID;
        }
        group >>= spare_bits;
    }
    if (in_group == 2)
    {
        if (bytes != NULL)
        {
            bytes[out] = (unsigned char)group;
        }
        out += 1;
    }
    else
    {
        if (bytes != NULL)
        {
            bytes[out] = (unsigned char)(group >> 8);
            bytes[out + 1] = (unsigned char)group;
        }
        out += 2;
    }
    *decoded = out;
    return TB_BASE64_OK;
}

tb_base64_result tb_base64_decode(const char *text, size_t length, unsigned flags, void *bytes,
                                  size_t capacity, size_t *written, size_t *error_offset)
{
    size_t needed = 0;
    size_t offset = 0;
    tb_base64_result result = TB_BASE64_OK;
    TB_ASSERT(text != NULL || length == 0);
    TB_ASSERT(bytes != NULL || capacity == 0);
    TB_ASSERT(written != NULL);
    *written = 0;
    if ((flags & ~TB_BASE64_KNOWN_FLAGS) != 0)
    {
        return TB_BASE64_INVALID;
    }
    result = tb_base64_walk(text, length, flags, NULL, &needed, &offset);
    if (result != TB_BASE64_OK)
    {
        if (error_offset != NULL)
        {
            *error_offset = offset;
        }
        return result;
    }
    if (capacity < needed)
    {
        *written = needed;
        return TB_BASE64_BUFFER_TOO_SMALL;
    }
    result = tb_base64_walk(text, length, flags, bytes, written, &offset);
    TB_ASSERT(result == TB_BASE64_OK && *written == needed);
    return result;
}

tb_base64_result tb_base64_encode_alloc(const void *bytes, size_t length, unsigned flags,
                                        const tb_allocator *allocator, char **text,
                                        size_t *text_length)
{
    size_t needed = 0;
    char *output = NULL;
    tb_base64_result result = TB_BASE64_OK;
    TB_ASSERT(text != NULL);
    TB_ASSERT(text_length != NULL);
    *text = NULL;
    *text_length = 0;
    result = tb_base64_encoded_size(length, flags, &needed);
    if (result != TB_BASE64_OK)
    {
        return result;
    }
    if (needed == SIZE_MAX)
    {
        return TB_BASE64_TOO_LARGE;
    }
    output = tb_allocator_alloc(allocator, needed + 1, 1);
    result = tb_base64_encode(bytes, length, flags, output, needed, text_length);
    TB_ASSERT(result == TB_BASE64_OK);
    output[needed] = '\0';
    *text = output;
    return result;
}

tb_base64_result tb_base64_decode_alloc(const char *text, size_t length, unsigned flags,
                                        const tb_allocator *allocator, unsigned char **bytes,
                                        size_t *byte_length, size_t *error_offset)
{
    size_t needed = 0;
    unsigned char *output = NULL;
    tb_base64_result result = TB_BASE64_OK;
    TB_ASSERT(bytes != NULL);
    TB_ASSERT(byte_length != NULL);
    *bytes = NULL;
    *byte_length = 0;
    /* Measure: a decode into no room reports the size it needs. */
    result = tb_base64_decode(text, length, flags, NULL, 0, &needed, error_offset);
    if (result == TB_BASE64_OK)
    {
        needed = 0;
    }
    else if (result != TB_BASE64_BUFFER_TOO_SMALL)
    {
        return result;
    }
    output = tb_allocator_alloc(allocator, needed + 1, 1);
    result = tb_base64_decode(text, length, flags, output, needed, byte_length, error_offset);
    TB_ASSERT(result == TB_BASE64_OK);
    output[needed] = 0;
    *bytes = output;
    return result;
}

const char *tb_base64_result_name(tb_base64_result result)
{
    switch (result)
    {
    case TB_BASE64_OK:
        return "ok";
    case TB_BASE64_INVALID:
        return "invalid argument";
    case TB_BASE64_TOO_LARGE:
        return "too large";
    case TB_BASE64_BUFFER_TOO_SMALL:
        return "buffer too small";
    default:
        return "unknown result";
    }
}
