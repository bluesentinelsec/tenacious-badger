/* backtrace and dladdr are not part of POSIX; request them from the strict
 * C17 headers of each system. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "badger/stack.h"

#include "badger/assert.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <string.h>

/* The most frames that one capture asks the system for. */
#define TB_POSIX_STACK_FRAMES_MAX ((size_t)128)

size_t tb_stack_capture(void **frames, size_t capacity, size_t skip)
{
    void *all[TB_POSIX_STACK_FRAMES_MAX + 1] = {0};
    size_t wanted = capacity;
    size_t count = 0;
    int captured = 0;

    TB_ASSERT(frames != NULL || capacity == 0);
    if (wanted > TB_POSIX_STACK_FRAMES_MAX)
    {
        wanted = TB_POSIX_STACK_FRAMES_MAX;
    }
    if (wanted == 0 || skip >= TB_POSIX_STACK_FRAMES_MAX)
    {
        return 0;
    }
    /* The system's first frame is this function, which is left out with the
     * frames that the caller asked to have left out. */
    captured = backtrace(all, (int)(TB_POSIX_STACK_FRAMES_MAX + 1));
    for (size_t index = skip + 1; index < (size_t)captured && count < wanted; index++)
    {
        frames[count] = all[index];
        count++;
    }
    return count;
}

/* Copies text, cut off at a whole character of UTF-8 when it does not fit. */
static void tb_posix_copy_text(char *destination, size_t capacity, const char *text)
{
    size_t length = strlen(text);

    if (length > capacity - 1)
    {
        length = capacity - 1;
        /* A byte that continues a character is not where one may be cut. */
        while (length > 0 && ((unsigned char)text[length] & 0xC0) == 0x80)
        {
            length--;
        }
    }
    memcpy(destination, text, length);
    destination[length] = '\0';
}

void tb_stack_describe(const void *address, tb_stack_location *location)
{
    Dl_info info = {0};
    const char *name = NULL;

    TB_ASSERT(location != NULL);
    memset(location, 0, sizeof(*location));
    location->address = address;
    if (address == NULL || dladdr(address, &info) == 0)
    {
        return;
    }
    if (info.dli_fname != NULL)
    {
        name = info.dli_fname;
        for (const char *character = info.dli_fname; *character != '\0'; character++)
        {
            if (*character == '/')
            {
                name = character + 1;
            }
        }
        tb_posix_copy_text(location->module, sizeof(location->module), name);
        location->module_offset = (size_t)((uintptr_t)address - (uintptr_t)info.dli_fbase);
    }
    if (info.dli_sname != NULL)
    {
        tb_posix_copy_text(location->function, sizeof(location->function), info.dli_sname);
        location->function_offset = (size_t)((uintptr_t)address - (uintptr_t)info.dli_saddr);
    }
}
