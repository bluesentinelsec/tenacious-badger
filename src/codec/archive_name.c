/* The names an archive writer accepts. */
#include "archive_internal.h"

#include "badger/utf8.h"

#include <string.h>

bool tb_archive_is_text(const char *text, size_t length)
{
    size_t error = 0;
    return text != NULL && length > 0 && memchr(text, '\0', length) == NULL &&
           tb_utf8_validate(text, length, &error);
}

bool tb_archive_is_relative_path(const char *name, size_t length)
{
    size_t start = 0;
    if (!tb_archive_is_text(name, length))
    {
        return false;
    }
    while (start <= length)
    {
        const char *const slash = memchr(name + start, '/', length - start);
        const size_t end = slash == NULL ? length : (size_t)(slash - name);
        const size_t component = end - start;
        const bool dots = (component == 1 && name[start] == '.') ||
                          (component == 2 && name[start] == '.' && name[start + 1] == '.');
        if (component == 0 || dots)
        {
            return false;
        }
        start = end + 1;
    }
    return true;
}
