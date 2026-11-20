#include "library.h"

#include "badger/assert.h"
#include "fs.h"
#include "name.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

tb_library *tb_native_library_record(const char *name)
{
    const size_t length = strlen(name);
    tb_library *library = NULL;

    /* coverage-off: no name is as long as the address space */
    if (length > SIZE_MAX - sizeof(tb_library) - 1)
    {
        return NULL;
    }
    /* coverage-on */
    library = malloc(sizeof(tb_library) + length + 1);
    /* coverage-off: allocation fails only when the system is out of memory */
    if (library == NULL)
    {
        return NULL;
    }
    /* coverage-on */
    library->handle = NULL;
    memcpy(library + 1, name, length + 1);
    library->name = (const char *)(library + 1);
    return library;
}

bool tb_native_library_is_text(const char *name)
{
    return tb_native_fs_text_valid(name, strlen(name));
}

bool tb_native_library_is_file_name(const char *name)
{
    TB_ASSERT(name != NULL);
    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    {
        return false;
    }
    for (const char *byte = name; *byte != '\0'; byte++)
    {
        if (*byte == '/' || *byte == '\\' || *byte == ':')
        {
            return false;
        }
    }
    return tb_native_library_is_text(name);
}

void tb_native_library_say(tb_library_error *error, const char *text)
{
    size_t length = 0;

    if (error == NULL || text == NULL)
    {
        return;
    }
    while (length < TB_LIBRARY_MESSAGE_CAPACITY - 1 && error->message[length] != '\0')
    {
        length++;
    }
    (void)tb_platform_copy_name(error->message + length, TB_LIBRARY_MESSAGE_CAPACITY - length,
                                text);
}

tb_library_result tb_native_library_fail(tb_library_error *error, tb_library_result result,
                                         const char *subject)
{
    if (error != NULL)
    {
        error->result = result;
        error->message[0] = '\0';
        tb_native_library_say(error, subject);
        tb_native_library_say(error, ": ");
    }
    return result;
}

tb_library_result tb_native_library_succeed(tb_library_error *error)
{
    if (error != NULL)
    {
        error->result = TB_LIBRARY_OK;
        error->message[0] = '\0';
    }
    return TB_LIBRARY_OK;
}

tb_library_result tb_native_library_not_loaded(tb_library_error *error, const char *name,
                                               tb_library_search search, const char *reason)
{
    static const char *const places[] = {
        [TB_LIBRARY_SEARCH_SYSTEM] = "not loaded from the system's libraries",
        [TB_LIBRARY_SEARCH_APPLICATION] = "not loaded from the application's directory",
        [TB_LIBRARY_SEARCH_PATH] = "not loaded",
    };

    TB_ASSERT((size_t)search < sizeof(places) / sizeof(places[0]));
    (void)tb_native_library_fail(error, TB_LIBRARY_UNAVAILABLE, name);
    tb_native_library_say(error, places[search]);
    if (reason != NULL && reason[0] != '\0')
    {
        tb_native_library_say(error, ": ");
        tb_native_library_say(error, reason);
    }
    return TB_LIBRARY_UNAVAILABLE;
}

tb_library_result tb_native_library_missing(tb_library_error *error, const tb_library *library,
                                            const char *name)
{
    (void)tb_native_library_fail(error, TB_LIBRARY_SYMBOL_MISSING, library->name);
    tb_native_library_say(error, "missing ");
    tb_native_library_say(error, name);
    return TB_LIBRARY_SYMBOL_MISSING;
}

tb_library_result tb_library_bind(const tb_library *library, const tb_library_binding *bindings,
                                  size_t count, tb_library_error *error)
{
    size_t missing = 0;

    TB_ASSERT(library != NULL);
    TB_ASSERT(bindings != NULL || count == 0);
    for (size_t index = 0; index < count; index++)
    {
        const tb_library_binding *const binding = &bindings[index];
        tb_library_result found = TB_LIBRARY_OK;

        TB_ASSERT(binding->name != NULL && binding->function != NULL);
        found = tb_library_find_function(library, binding->name, binding->function, NULL);
        if (found == TB_LIBRARY_OK || !binding->required)
        {
            continue;
        }
        if (missing == 0)
        {
            (void)tb_native_library_fail(error, TB_LIBRARY_SYMBOL_MISSING, library->name);
            tb_native_library_say(error, "missing ");
        }
        else
        {
            tb_native_library_say(error, ", ");
        }
        tb_native_library_say(error, binding->name);
        missing++;
    }
    if (missing > 0)
    {
        return TB_LIBRARY_SYMBOL_MISSING;
    }
    return tb_native_library_succeed(error);
}
