#include "name.h"

#include <stdbool.h>

/* In UTF-8 the bytes of a character after its first all have the form
 * 10xxxxxx, and no first byte has. */
#define TB_UTF8_CONTINUATION_MASK 0xC0u
#define TB_UTF8_CONTINUATION_BITS 0x80u

static bool tb_platform_is_continuation(char byte)
{
    return ((unsigned char)byte & TB_UTF8_CONTINUATION_MASK) == TB_UTF8_CONTINUATION_BITS;
}

size_t tb_platform_copy_name(char *destination, size_t capacity, const char *name)
{
    size_t length = 0;

    if (name != NULL)
    {
        /* One byte is kept for the terminator. */
        while (name[length] != '\0' && length + 1 < capacity)
        {
            length++;
        }
        /* Cut short: the byte after the cut continues a character, so the
         * cut is inside that character, and the whole of it is dropped. */
        if (name[length] != '\0')
        {
            while (length > 0 && tb_platform_is_continuation(name[length]))
            {
                length--;
            }
        }
    }
    for (size_t index = 0; index < length; index++)
    {
        destination[index] = name[index];
    }
    destination[length] = '\0';
    return length;
}
