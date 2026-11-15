#include "bytes.h"

#include <string.h>

tb_byte_set tb_byte_set_of(const char *members, size_t length)
{
    tb_byte_set set = {0};

    for (size_t index = 0; index < length; index++)
    {
        set.has[(unsigned char)members[index]] = true;
    }
    return set;
}

size_t tb_bytes_find_byte(const char *bytes, size_t begin, size_t end, char byte)
{
    const char *found = NULL;

    if (begin == end)
    {
        return TB_BYTES_NOT_FOUND;
    }
    found = memchr(bytes + begin, (unsigned char)byte, end - begin);
    if (found == NULL)
    {
        return TB_BYTES_NOT_FOUND;
    }
    return (size_t)(found - bytes);
}

size_t tb_bytes_rfind_byte(const char *bytes, size_t begin, size_t end, char byte)
{
    for (size_t index = end; index > begin; index--)
    {
        if (bytes[index - 1] == byte)
        {
            return index - 1;
        }
    }
    return TB_BYTES_NOT_FOUND;
}

size_t tb_bytes_find_any(const char *bytes, size_t begin, size_t end, const tb_byte_set *set)
{
    for (size_t index = begin; index < end; index++)
    {
        if (set->has[(unsigned char)bytes[index]])
        {
            return index;
        }
    }
    return TB_BYTES_NOT_FOUND;
}

size_t tb_bytes_rfind_any(const char *bytes, size_t begin, size_t end, const tb_byte_set *set)
{
    for (size_t index = end; index > begin; index--)
    {
        if (set->has[(unsigned char)bytes[index - 1]])
        {
            return index - 1;
        }
    }
    return TB_BYTES_NOT_FOUND;
}

size_t tb_bytes_span(const char *bytes, size_t begin, size_t end, const tb_byte_set *set)
{
    size_t index = begin;

    while (index < end && set->has[(unsigned char)bytes[index]])
    {
        index++;
    }
    return index - begin;
}

size_t tb_bytes_rspan(const char *bytes, size_t begin, size_t end, const tb_byte_set *set)
{
    size_t index = end;

    while (index > begin && set->has[(unsigned char)bytes[index - 1]])
    {
        index--;
    }
    return end - index;
}

size_t tb_bytes_find(const char *bytes, size_t begin, size_t end, const char *pattern,
                     size_t pattern_length)
{
    size_t last_start = {0};
    size_t start = begin;

    if (pattern_length == 0)
    {
        return begin;
    }
    if (pattern_length > end - begin)
    {
        return TB_BYTES_NOT_FOUND;
    }
    /* The C library finds candidates for the first byte, and each candidate
     * is then compared in full. */
    last_start = end - pattern_length;
    while (start <= last_start)
    {
        start = tb_bytes_find_byte(bytes, start, last_start + 1, pattern[0]);
        if (start == TB_BYTES_NOT_FOUND)
        {
            return TB_BYTES_NOT_FOUND;
        }
        if (memcmp(bytes + start, pattern, pattern_length) == 0)
        {
            return start;
        }
        start++;
    }
    return TB_BYTES_NOT_FOUND;
}

size_t tb_bytes_rfind(const char *bytes, size_t begin, size_t end, const char *pattern,
                      size_t pattern_length)
{
    if (pattern_length == 0)
    {
        return end;
    }
    if (pattern_length > end - begin)
    {
        return TB_BYTES_NOT_FOUND;
    }
    for (size_t start = end - pattern_length + 1; start > begin; start--)
    {
        if (memcmp(bytes + start - 1, pattern, pattern_length) == 0)
        {
            return start - 1;
        }
    }
    return TB_BYTES_NOT_FOUND;
}
