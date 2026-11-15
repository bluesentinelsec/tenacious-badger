#include "badger/text.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/size.h"
#include "bytes.h"
#include "memory_internal.h"

#include <limits.h>
#include <string.h>

/* Text is bytes, so it needs no alignment. */
#define TB_TEXT_ALIGNMENT ((size_t)1)

/* A text is well formed when it has bytes wherever it has length. */
static bool tb_text_is_valid(tb_text text)
{
    return text.bytes != NULL || text.length == 0;
}

/* Checks a range against a text and returns the index it really ends at,
 * which is the length of the text when the caller passed TB_TEXT_END. */
static size_t tb_text_range_end(tb_text text, size_t begin, size_t end)
{
    TB_ASSERT_MSG(tb_text_is_valid(text), "a text with a length must have bytes");
    if (end == TB_TEXT_END)
    {
        end = text.length;
    }
    TB_ASSERT_MSG(begin <= end && end <= text.length,
                  "the range must be in order and within the text");
    return end;
}

/* Copies bytes when there are any. The C library must not be given a null
 * pointer, which an empty text may have, even to copy nothing. */
static void tb_text_copy_bytes(char *destination, const char *source, size_t length)
{
    if (length > 0)
    {
        memcpy(destination, source, length);
    }
}

/* ---------------------------------------------------------------------------
 * Making texts that borrow their bytes.
 * ------------------------------------------------------------------------- */

tb_text tb_text_make(const char *bytes, size_t length)
{
    const tb_text text = {.length = length, .bytes = bytes};

    TB_ASSERT_MSG(tb_text_is_valid(text), "a text with a length must have bytes");
    return text;
}

tb_text tb_text_from_cstr(const char *string)
{
    TB_ASSERT(string != NULL);
    return tb_text_make(string, strlen(string));
}

tb_text tb_text_sub(tb_text text, size_t begin, size_t end)
{
    tb_text part = {0};

    end = tb_text_range_end(text, begin, end);
    part.length = end - begin;
    /* The empty text may have no bytes to point into. */
    if (text.bytes != NULL)
    {
        part.bytes = text.bytes + begin;
    }
    return part;
}

tb_text tb_text_trim(tb_text text, tb_text set)
{
    tb_byte_set members = {0};
    size_t leading = {0};
    size_t trailing = {0};

    TB_ASSERT_MSG(tb_text_is_valid(text), "a text with a length must have bytes");
    TB_ASSERT_MSG(tb_text_is_valid(set), "a text with a length must have bytes");
    /* The set is built once for both ends. */
    tb_byte_set_add(&members, set.bytes, set.length);
    leading = tb_bytes_span(text.bytes, 0, text.length, &members);
    trailing = tb_bytes_rspan(text.bytes, leading, text.length, &members);
    return tb_text_sub(text, leading, text.length - trailing);
}

/* ---------------------------------------------------------------------------
 * Comparing.
 * ------------------------------------------------------------------------- */

int tb_text_compare(tb_text left, tb_text right)
{
    TB_ASSERT_MSG(tb_text_is_valid(left), "a text with a length must have bytes");
    TB_ASSERT_MSG(tb_text_is_valid(right), "a text with a length must have bytes");
    return tb_bytes_compare(left.bytes, left.length, right.bytes, right.length);
}

bool tb_text_equal(tb_text left, tb_text right)
{
    return left.length == right.length && tb_text_compare(left, right) == 0;
}

bool tb_text_matches_at(tb_text text, size_t position, tb_text pattern)
{
    TB_ASSERT_MSG(tb_text_is_valid(text), "a text with a length must have bytes");
    TB_ASSERT_MSG(tb_text_is_valid(pattern), "a text with a length must have bytes");
    TB_ASSERT_MSG(position <= text.length, "the position must be within the text");
    if (pattern.length > text.length - position)
    {
        return false;
    }
    if (pattern.length == 0)
    {
        return true;
    }
    return memcmp(text.bytes + position, pattern.bytes, pattern.length) == 0;
}

bool tb_text_starts_with(tb_text text, tb_text prefix)
{
    return tb_text_matches_at(text, 0, prefix);
}

bool tb_text_ends_with(tb_text text, tb_text suffix)
{
    TB_ASSERT_MSG(tb_text_is_valid(text), "a text with a length must have bytes");
    if (suffix.length > text.length)
    {
        return false;
    }
    return tb_text_matches_at(text, text.length - suffix.length, suffix);
}

/* ---------------------------------------------------------------------------
 * Searching.
 * ------------------------------------------------------------------------- */

size_t tb_text_find_char(tb_text text, size_t begin, size_t end, char character)
{
    end = tb_text_range_end(text, begin, end);
    return tb_bytes_find_byte(text.bytes, begin, end, character);
}

size_t tb_text_rfind_char(tb_text text, size_t begin, size_t end, char character)
{
    end = tb_text_range_end(text, begin, end);
    return tb_bytes_rfind_byte(text.bytes, begin, end, character);
}

size_t tb_text_find_any(tb_text text, size_t begin, size_t end, tb_text set)
{
    tb_byte_set members = {0};

    TB_ASSERT_MSG(tb_text_is_valid(set), "a text with a length must have bytes");
    end = tb_text_range_end(text, begin, end);
    tb_byte_set_add(&members, set.bytes, set.length);
    return tb_bytes_find_any(text.bytes, begin, end, &members);
}

size_t tb_text_rfind_any(tb_text text, size_t begin, size_t end, tb_text set)
{
    tb_byte_set members = {0};

    TB_ASSERT_MSG(tb_text_is_valid(set), "a text with a length must have bytes");
    end = tb_text_range_end(text, begin, end);
    tb_byte_set_add(&members, set.bytes, set.length);
    return tb_bytes_rfind_any(text.bytes, begin, end, &members);
}

size_t tb_text_find(tb_text text, size_t begin, size_t end, tb_text pattern)
{
    TB_ASSERT_MSG(tb_text_is_valid(pattern), "a text with a length must have bytes");
    end = tb_text_range_end(text, begin, end);
    return tb_bytes_find(text.bytes, begin, end, pattern.bytes, pattern.length);
}

size_t tb_text_rfind(tb_text text, size_t begin, size_t end, tb_text pattern)
{
    TB_ASSERT_MSG(tb_text_is_valid(pattern), "a text with a length must have bytes");
    end = tb_text_range_end(text, begin, end);
    return tb_bytes_rfind(text.bytes, begin, end, pattern.bytes, pattern.length);
}

size_t tb_text_span(tb_text text, size_t begin, size_t end, tb_text set)
{
    tb_byte_set members = {0};

    TB_ASSERT_MSG(tb_text_is_valid(set), "a text with a length must have bytes");
    end = tb_text_range_end(text, begin, end);
    tb_byte_set_add(&members, set.bytes, set.length);
    return tb_bytes_span(text.bytes, begin, end, &members);
}

size_t tb_text_rspan(tb_text text, size_t begin, size_t end, tb_text set)
{
    tb_byte_set members = {0};

    TB_ASSERT_MSG(tb_text_is_valid(set), "a text with a length must have bytes");
    end = tb_text_range_end(text, begin, end);
    tb_byte_set_add(&members, set.bytes, set.length);
    return tb_bytes_rspan(text.bytes, begin, end, &members);
}

/* ---------------------------------------------------------------------------
 * Making texts with bytes of their own.
 * ------------------------------------------------------------------------- */

/* Allocates the bytes of a text of a length, uninitialized. An empty text
 * allocates nothing. */
static char *tb_text_new_bytes(const tb_allocator *allocator, size_t length)
{
    if (length == 0)
    {
        return NULL;
    }
    return tb_allocator_alloc(allocator, length, TB_TEXT_ALIGNMENT);
}

tb_text tb_text_join(const tb_allocator *allocator, const tb_text *texts, size_t count,
                     tb_text separator)
{
    char *bytes = NULL;
    char *position = NULL;
    size_t length = {0};

    TB_ASSERT(texts != NULL || count == 0);
    TB_ASSERT_MSG(tb_text_is_valid(separator), "a text with a length must have bytes");
    for (size_t index = 0; index < count; index++)
    {
        TB_ASSERT_MSG(tb_text_is_valid(texts[index]), "a text with a length must have bytes");
        if (!tb_size_add(length, texts[index].length, &length))
        {
            tb_memory_fail(1, SIZE_MAX, TB_TEXT_ALIGNMENT);
        }
        if (index == 0)
        {
            continue;
        }
        if (!tb_size_add(length, separator.length, &length))
        {
            tb_memory_fail(1, SIZE_MAX, TB_TEXT_ALIGNMENT);
        }
    }
    bytes = tb_text_new_bytes(allocator, length);
    position = bytes;
    for (size_t index = 0; index < count; index++)
    {
        if (index > 0)
        {
            tb_text_copy_bytes(position, separator.bytes, separator.length);
            position += separator.length;
        }
        tb_text_copy_bytes(position, texts[index].bytes, texts[index].length);
        position += texts[index].length;
    }
    return tb_text_make(bytes, length);
}

tb_text tb_text_copy(const tb_allocator *allocator, tb_text text)
{
    return tb_text_join(allocator, &text, 1, tb_text_make(NULL, 0));
}

tb_text tb_text_cat(const tb_allocator *allocator, tb_text left, tb_text right)
{
    const tb_text texts[] = {left, right};

    return tb_text_join(allocator, texts, 2, tb_text_make(NULL, 0));
}

char *tb_text_to_cstr(const tb_allocator *allocator, tb_text text)
{
    char *string = NULL;
    size_t size = {0};

    TB_ASSERT_MSG(tb_text_is_valid(text), "a text with a length must have bytes");
    if (!tb_size_add(text.length, 1, &size))
    {
        tb_memory_fail(1, text.length, TB_TEXT_ALIGNMENT);
    }
    string = tb_allocator_alloc(allocator, size, TB_TEXT_ALIGNMENT);
    tb_text_copy_bytes(string, text.bytes, text.length);
    string[text.length] = '\0';
    return string;
}

tb_text tb_text_repeat(const tb_allocator *allocator, tb_text text, size_t count)
{
    char *bytes = NULL;
    size_t length = {0};

    TB_ASSERT_MSG(tb_text_is_valid(text), "a text with a length must have bytes");
    if (!tb_size_multiply(text.length, count, &length))
    {
        tb_memory_fail(count, text.length, TB_TEXT_ALIGNMENT);
    }
    bytes = tb_text_new_bytes(allocator, length);
    for (size_t copy = 0; copy < count; copy++)
    {
        tb_text_copy_bytes(bytes + copy * text.length, text.bytes, text.length);
    }
    return tb_text_make(bytes, length);
}

tb_text tb_text_reverse(const tb_allocator *allocator, tb_text text)
{
    char *bytes = NULL;

    TB_ASSERT_MSG(tb_text_is_valid(text), "a text with a length must have bytes");
    bytes = tb_text_new_bytes(allocator, text.length);
    for (size_t index = 0; index < text.length; index++)
    {
        bytes[index] = text.bytes[text.length - 1 - index];
    }
    return tb_text_make(bytes, text.length);
}

tb_text tb_text_map(const tb_allocator *allocator, tb_text text, tb_text from, tb_text to)
{
    /* What each byte value becomes; a byte that is not in `from` stays. */
    unsigned char mapped[UCHAR_MAX + 1] = {0};
    char *bytes = NULL;

    TB_ASSERT_MSG(tb_text_is_valid(text), "a text with a length must have bytes");
    TB_ASSERT_MSG(tb_text_is_valid(from), "a text with a length must have bytes");
    TB_ASSERT_MSG(tb_text_is_valid(to), "a text with a length must have bytes");
    TB_ASSERT_MSG(from.length == to.length, "from and to must have the same length");
    for (size_t value = 0; value <= UCHAR_MAX; value++)
    {
        mapped[value] = (unsigned char)value;
    }
    for (size_t index = 0; index < from.length; index++)
    {
        mapped[(unsigned char)from.bytes[index]] = (unsigned char)to.bytes[index];
    }
    bytes = tb_text_new_bytes(allocator, text.length);
    for (size_t index = 0; index < text.length; index++)
    {
        bytes[index] = (char)mapped[(unsigned char)text.bytes[index]];
    }
    return tb_text_make(bytes, text.length);
}

void tb_text_free(const tb_allocator *allocator, tb_text text)
{
    /* The bytes were handed out as constant, because nobody may change a
     * text. Their owner gives them back as the plain memory they are. */
    void *const memory = (void *)(uintptr_t)text.bytes;

    if (text.length == 0)
    {
        return;
    }
    tb_allocator_free(allocator, memory, text.length, TB_TEXT_ALIGNMENT);
}

/* ---------------------------------------------------------------------------
 * Formatting.
 * ------------------------------------------------------------------------- */

/* "%T" takes a tb_text by value. Like "%s", a precision is the most bytes to
 * write and the width pads with spaces. */
static void tb_text_format(tb_fmt_output *output, const tb_fmt_spec *spec, va_list *arguments)
{
    const tb_text text = va_arg(*arguments, tb_text);
    size_t length = text.length;

    if (spec->has_precision && spec->precision < length)
    {
        length = spec->precision;
    }
    tb_fmt_put_padded(output, spec, text.bytes, length);
}

void tb_text_register_format(void)
{
    (void)tb_fmt_register('T', tb_text_format);
}
