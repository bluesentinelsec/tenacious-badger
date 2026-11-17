#include "badger/utf8.h"
#include "badger/assert.h"
#include "badger/size.h"

static bool tb_utf8_is_continuation(unsigned char byte)
{
    return (byte & 0xC0u) == 0x80u;
}

size_t tb_utf8_encoded_size(uint32_t scalar)
{
    if (scalar > 0x10FFFFu || (scalar >= 0xD800u && scalar <= 0xDFFFu))
    {
        return 0;
    }
    if (scalar <= 0x7Fu)
    {
        return 1;
    }
    if (scalar <= 0x7FFu)
    {
        return 2;
    }
    if (scalar <= 0xFFFFu)
    {
        return 3;
    }
    return 4;
}

bool tb_utf8_decode(const char *bytes, size_t length, size_t offset, uint32_t *scalar, size_t *next)
{
    unsigned char lead = {0};
    uint32_t value = {0};
    uint32_t minimum = {0};
    size_t width = {0};

    TB_ASSERT(bytes != NULL || length == 0);
    TB_ASSERT(scalar != NULL);
    TB_ASSERT(next != NULL);
    if (offset >= length)
    {
        return false;
    }
    lead = (unsigned char)bytes[offset];
    if (lead <= 0x7Fu)
    {
        width = 1;
        value = lead;
    }
    else if (lead >= 0xC2u && lead <= 0xDFu)
    {
        width = 2;
        value = lead & 0x1Fu;
        minimum = 0x80u;
    }
    else if (lead >= 0xE0u && lead <= 0xEFu)
    {
        width = 3;
        value = lead & 0x0Fu;
        minimum = 0x800u;
    }
    else if (lead >= 0xF0u && lead <= 0xF4u)
    {
        width = 4;
        value = lead & 0x07u;
        minimum = 0x10000u;
    }
    else
    {
        return false;
    }
    if (width > length - offset)
    {
        return false;
    }
    for (size_t index = {0}; index + 1 < width; index++)
    {
        unsigned char byte = {0};
        byte = (unsigned char)bytes[offset + index + 1];
        if (!tb_utf8_is_continuation(byte))
        {
            return false;
        }
        value = (value << 6) | (byte & 0x3Fu);
    }
    if (value < minimum || tb_utf8_encoded_size(value) == 0)
    {
        return false;
    }
    *scalar = value;
    *next = offset + width;
    return true;
}

size_t tb_utf8_encode(uint32_t scalar, char *destination, size_t capacity)
{
    size_t width = {0};
    uint32_t remaining = {0};
    unsigned char prefix = {0};

    TB_ASSERT(destination != NULL || capacity == 0);
    width = tb_utf8_encoded_size(scalar);
    if (width == 0 || capacity < width)
    {
        return 0;
    }
    remaining = scalar;
    for (size_t index = {0}; index + 1 < width; index++)
    {
        destination[width - index - 1] = (char)(0x80u | (remaining & 0x3Fu));
        remaining >>= 6;
    }
    if (width == 2)
    {
        prefix = 0xC0u;
    }
    else if (width == 3)
    {
        prefix = 0xE0u;
    }
    else if (width == 4)
    {
        prefix = 0xF0u;
    }
    destination[0] = (char)(prefix | remaining);
    return width;
}

bool tb_utf8_validate(const char *bytes, size_t length, size_t *error)
{
    size_t position = {0};
    uint32_t scalar = {0};
    TB_ASSERT(bytes != NULL || length == 0);
    TB_ASSERT(error != NULL);
    while (position < length)
    {
        *error = position;
        if (!tb_utf8_decode(bytes, length, position, &scalar, &position))
        {
            return false;
        }
    }
    *error = length;
    return true;
}

bool tb_utf8_count(const char *bytes, size_t length, size_t *count)
{
    size_t position = {0};
    size_t found = {0};
    uint32_t scalar = {0};
    TB_ASSERT(bytes != NULL || length == 0);
    TB_ASSERT(count != NULL);
    while (position < length)
    {
        if (!tb_utf8_decode(bytes, length, position, &scalar, &position))
        {
            return false;
        }
        found++;
    }
    *count = found;
    return true;
}

bool tb_utf8_offset(const char *bytes, size_t length, size_t index, size_t *offset)
{
    size_t position = {0};
    uint32_t scalar = {0};
    TB_ASSERT(bytes != NULL || length == 0);
    TB_ASSERT(offset != NULL);
    for (size_t skipped = {0}; skipped < index; skipped++)
    {
        if (!tb_utf8_decode(bytes, length, position, &scalar, &position))
        {
            return false;
        }
    }
    *offset = position;
    return true;
}

bool tb_utf8_previous(const char *bytes, size_t length, size_t offset, uint32_t *scalar,
                      size_t *previous)
{
    size_t start = {0};
    size_t next = {0};
    uint32_t value = {0};
    TB_ASSERT(bytes != NULL || length == 0);
    TB_ASSERT(scalar != NULL);
    TB_ASSERT(previous != NULL);
    if (offset == 0 || offset > length)
    {
        return false;
    }
    start = offset - 1;
    while (start > 0 && offset - start < TB_UTF8_MAX_BYTES &&
           tb_utf8_is_continuation((unsigned char)bytes[start]))
    {
        start--;
    }
    if (!tb_utf8_decode(bytes, offset, start, &value, &next) || next != offset)
    {
        return false;
    }
    *scalar = value;
    *previous = start;
    return true;
}

bool tb_utf8_boundary(const char *bytes, size_t length, size_t offset, size_t *boundary)
{
    size_t position = {0};
    size_t next = {0};
    uint32_t scalar = {0};
    TB_ASSERT(bytes != NULL || length == 0);
    TB_ASSERT(boundary != NULL);
    if (offset > length)
    {
        return false;
    }
    while (position < offset)
    {
        if (!tb_utf8_decode(bytes, length, position, &scalar, &next))
        {
            return false;
        }
        if (next > offset)
        {
            break;
        }
        position = next;
    }
    *boundary = position;
    return true;
}

static bool tb_utf16_decode(const uint16_t *units, size_t length, size_t *offset, uint32_t *scalar)
{
    uint32_t first = {0};
    uint32_t second = {0};
    first = units[*offset];
    (*offset)++;
    if (first >= 0xDC00u && first <= 0xDFFFu)
    {
        return false;
    }
    if (first < 0xD800u || first > 0xDBFFu)
    {
        *scalar = first;
        return true;
    }
    if (*offset == length)
    {
        return false;
    }
    second = units[*offset];
    (*offset)++;
    if (second < 0xDC00u || second > 0xDFFFu)
    {
        return false;
    }
    *scalar = 0x10000u + ((first - 0xD800u) << 10) + second - 0xDC00u;
    return true;
}

bool tb_utf8_to_utf16(const char *bytes, size_t length, uint16_t *destination, size_t capacity,
                      size_t *written)
{
    size_t position = {0};
    size_t needed = {0};
    size_t output = {0};
    uint32_t scalar = {0};
    TB_ASSERT(bytes != NULL || length == 0);
    TB_ASSERT(destination != NULL || capacity == 0);
    TB_ASSERT(written != NULL);
    *written = 0;
    while (position < length)
    {
        size_t width = {0};
        width = 1;
        if (!tb_utf8_decode(bytes, length, position, &scalar, &position))
        {
            return false;
        }
        if (scalar > 0xFFFFu)
        {
            width = 2;
        }
        if (!tb_size_add(needed, width, &needed))
        {
            return false;
        }
    }
    *written = needed;
    if (destination == NULL)
    {
        return true;
    }
    if (capacity < needed)
    {
        return false;
    }
    position = 0;
    while (position < length)
    {
        (void)tb_utf8_decode(bytes, length, position, &scalar, &position);
        if (scalar <= 0xFFFFu)
        {
            destination[output] = (uint16_t)scalar;
            output++;
        }
        else
        {
            scalar -= 0x10000u;
            destination[output] = (uint16_t)(0xD800u + (scalar >> 10));
            output++;
            destination[output] = (uint16_t)(0xDC00u + (scalar & 0x3FFu));
            output++;
        }
    }
    return true;
}

bool tb_utf8_from_utf16(const uint16_t *units, size_t length, char *destination, size_t capacity,
                        size_t *written)
{
    size_t position = {0};
    size_t needed = {0};
    size_t output = {0};
    uint32_t scalar = {0};
    TB_ASSERT(units != NULL || length == 0);
    TB_ASSERT(destination != NULL || capacity == 0);
    TB_ASSERT(written != NULL);
    *written = 0;
    while (position < length)
    {
        if (!tb_utf16_decode(units, length, &position, &scalar))
        {
            return false;
        }
        if (!tb_size_add(needed, tb_utf8_encoded_size(scalar), &needed))
        {
            return false;
        }
    }
    *written = needed;
    if (destination == NULL)
    {
        return true;
    }
    if (capacity < needed)
    {
        return false;
    }
    position = 0;
    while (position < length)
    {
        (void)tb_utf16_decode(units, length, &position, &scalar);
        output += tb_utf8_encode(scalar, destination + output, capacity - output);
    }
    return true;
}
