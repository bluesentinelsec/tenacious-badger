#include "badger/str.h"

#include "badger/assert.h"
#include "badger/size.h"
#include "bytes.h"
#include "memory_internal.h"

#include <limits.h>
#include <string.h>

/* Strings are bytes, so they need no alignment. */
#define TB_STR_ALIGNMENT ((size_t)1)

/* Checks a range against a string and returns the index it really ends at,
 * which is the length of the string when the caller passed TB_STR_END. */
static size_t tb_str_range_end(const char *string, size_t begin, size_t end)
{
    TB_ASSERT(string != NULL);
    if (end == TB_STR_END)
    {
        end = strlen(string);
    }
    TB_ASSERT_MSG(begin <= end && end <= strlen(string),
                  "the range must be in order and within the string");
    return end;
}

/* ---------------------------------------------------------------------------
 * Measuring and comparing.
 * ------------------------------------------------------------------------- */

size_t tb_str_length(const char *string)
{
    TB_ASSERT(string != NULL);
    return strlen(string);
}

int tb_str_compare(const char *left, const char *right)
{
    TB_ASSERT(left != NULL);
    TB_ASSERT(right != NULL);
    /* strcmp compares bytes as unsigned values, which is the order promised. */
    return strcmp(left, right);
}

bool tb_str_equal(const char *left, const char *right)
{
    return tb_str_compare(left, right) == 0;
}

bool tb_str_matches_at(const char *string, size_t position, const char *pattern)
{
    TB_ASSERT(string != NULL);
    TB_ASSERT(pattern != NULL);
    TB_ASSERT_MSG(position <= strlen(string), "the position must be within the string");
    /* The comparison stops at the terminator of the string, so it never
     * reads past the end however long the pattern is. */
    return strncmp(string + position, pattern, strlen(pattern)) == 0;
}

bool tb_str_starts_with(const char *string, const char *prefix)
{
    TB_ASSERT(prefix != NULL);
    return tb_str_matches_at(string, 0, prefix);
}

bool tb_str_ends_with(const char *string, const char *suffix)
{
    size_t length = {0};
    size_t suffix_length = {0};

    TB_ASSERT(string != NULL);
    TB_ASSERT(suffix != NULL);
    length = strlen(string);
    suffix_length = strlen(suffix);
    if (suffix_length > length)
    {
        return false;
    }
    return memcmp(string + length - suffix_length, suffix, suffix_length) == 0;
}

/* ---------------------------------------------------------------------------
 * Searching.
 * ------------------------------------------------------------------------- */

size_t tb_str_find_char(const char *string, size_t begin, size_t end, char character)
{
    end = tb_str_range_end(string, begin, end);
    return tb_bytes_find_byte(string, begin, end, character);
}

size_t tb_str_rfind_char(const char *string, size_t begin, size_t end, char character)
{
    end = tb_str_range_end(string, begin, end);
    return tb_bytes_rfind_byte(string, begin, end, character);
}

size_t tb_str_find_any(const char *string, size_t begin, size_t end, const char *set)
{
    tb_byte_set members = {0};

    TB_ASSERT(set != NULL);
    end = tb_str_range_end(string, begin, end);
    tb_byte_set_add(&members, set, strlen(set));
    return tb_bytes_find_any(string, begin, end, &members);
}

size_t tb_str_rfind_any(const char *string, size_t begin, size_t end, const char *set)
{
    tb_byte_set members = {0};

    TB_ASSERT(set != NULL);
    end = tb_str_range_end(string, begin, end);
    tb_byte_set_add(&members, set, strlen(set));
    return tb_bytes_rfind_any(string, begin, end, &members);
}

size_t tb_str_find(const char *string, size_t begin, size_t end, const char *pattern)
{
    TB_ASSERT(pattern != NULL);
    end = tb_str_range_end(string, begin, end);
    return tb_bytes_find(string, begin, end, pattern, strlen(pattern));
}

size_t tb_str_rfind(const char *string, size_t begin, size_t end, const char *pattern)
{
    TB_ASSERT(pattern != NULL);
    end = tb_str_range_end(string, begin, end);
    return tb_bytes_rfind(string, begin, end, pattern, strlen(pattern));
}

size_t tb_str_span(const char *string, size_t begin, size_t end, const char *set)
{
    tb_byte_set members = {0};

    TB_ASSERT(set != NULL);
    end = tb_str_range_end(string, begin, end);
    tb_byte_set_add(&members, set, strlen(set));
    return tb_bytes_span(string, begin, end, &members);
}

size_t tb_str_rspan(const char *string, size_t begin, size_t end, const char *set)
{
    tb_byte_set members = {0};

    TB_ASSERT(set != NULL);
    end = tb_str_range_end(string, begin, end);
    tb_byte_set_add(&members, set, strlen(set));
    return tb_bytes_rspan(string, begin, end, &members);
}

/* ---------------------------------------------------------------------------
 * Building new strings.
 * ------------------------------------------------------------------------- */

/* Allocates a string of `length` bytes with its terminator in place and the
 * bytes before it uninitialized. */
static char *tb_str_new(const tb_allocator *allocator, size_t length)
{
    char *string = NULL;
    size_t size = {0};

    if (!tb_size_add(length, 1, &size))
    {
        tb_memory_fail(1, length, TB_STR_ALIGNMENT);
    }
    string = tb_allocator_alloc(allocator, size, TB_STR_ALIGNMENT);
    string[length] = '\0';
    return string;
}

char *tb_str_sub(const tb_allocator *allocator, const char *string, size_t begin, size_t end)
{
    char *part = NULL;

    end = tb_str_range_end(string, begin, end);
    part = tb_str_new(allocator, end - begin);
    memcpy(part, string + begin, end - begin);
    return part;
}

char *tb_str_dup(const tb_allocator *allocator, const char *string)
{
    return tb_str_sub(allocator, string, 0, TB_STR_END);
}

char *tb_str_join(const tb_allocator *allocator, const char *const *strings, size_t count,
                  const char *separator)
{
    char *joined = NULL;
    char *position = NULL;
    size_t separator_length = {0};
    size_t length = {0};

    TB_ASSERT(strings != NULL || count == 0);
    if (separator != NULL)
    {
        separator_length = strlen(separator);
    }
    for (size_t index = 0; index < count; index++)
    {
        TB_ASSERT_MSG(strings[index] != NULL, "every string to join must be present");
        if (!tb_size_add(length, strlen(strings[index]), &length))
        {
            tb_memory_fail(1, SIZE_MAX, TB_STR_ALIGNMENT);
        }
        if (index == 0)
        {
            continue;
        }
        if (!tb_size_add(length, separator_length, &length))
        {
            tb_memory_fail(1, SIZE_MAX, TB_STR_ALIGNMENT);
        }
    }
    joined = tb_str_new(allocator, length);
    position = joined;
    for (size_t index = 0; index < count; index++)
    {
        const size_t string_length = strlen(strings[index]);

        if (index > 0 && separator_length > 0)
        {
            memcpy(position, separator, separator_length);
            position += separator_length;
        }
        memcpy(position, strings[index], string_length);
        position += string_length;
    }
    return joined;
}

char *tb_str_cat(const tb_allocator *allocator, const char *left, const char *right)
{
    const char *const strings[] = {left, right};

    TB_ASSERT(left != NULL);
    TB_ASSERT(right != NULL);
    return tb_str_join(allocator, strings, 2, NULL);
}

char *tb_str_repeat(const tb_allocator *allocator, const char *string, size_t count)
{
    char *repeated = NULL;
    size_t string_length = {0};
    size_t length = {0};

    TB_ASSERT(string != NULL);
    string_length = strlen(string);
    if (!tb_size_multiply(string_length, count, &length))
    {
        tb_memory_fail(count, string_length, TB_STR_ALIGNMENT);
    }
    repeated = tb_str_new(allocator, length);
    for (size_t copy = 0; copy < count; copy++)
    {
        memcpy(repeated + copy * string_length, string, string_length);
    }
    return repeated;
}

char *tb_str_reverse(const tb_allocator *allocator, const char *string)
{
    char *reversed = NULL;
    size_t length = {0};

    TB_ASSERT(string != NULL);
    length = strlen(string);
    reversed = tb_str_new(allocator, length);
    for (size_t index = 0; index < length; index++)
    {
        reversed[index] = string[length - 1 - index];
    }
    return reversed;
}

char *tb_str_map(const tb_allocator *allocator, const char *string, const char *from,
                 const char *to)
{
    /* What each byte value becomes; a byte that is not in `from` stays. */
    unsigned char mapped[UCHAR_MAX + 1] = {0};
    char *result = NULL;
    size_t length = {0};
    size_t from_length = {0};

    TB_ASSERT(string != NULL);
    TB_ASSERT(from != NULL);
    TB_ASSERT(to != NULL);
    from_length = strlen(from);
    TB_ASSERT_MSG(strlen(to) == from_length, "from and to must have the same length");
    for (size_t value = 0; value <= UCHAR_MAX; value++)
    {
        mapped[value] = (unsigned char)value;
    }
    for (size_t index = 0; index < from_length; index++)
    {
        mapped[(unsigned char)from[index]] = (unsigned char)to[index];
    }
    length = strlen(string);
    result = tb_str_new(allocator, length);
    for (size_t index = 0; index < length; index++)
    {
        result[index] = (char)mapped[(unsigned char)string[index]];
    }
    return result;
}

void tb_str_free(const tb_allocator *allocator, char *string)
{
    if (string == NULL)
    {
        return;
    }
    tb_allocator_free(allocator, string, strlen(string) + 1, TB_STR_ALIGNMENT);
}
