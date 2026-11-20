#include "badger/library.h"

#include "not_implemented.h"

/* Standard C cannot load a library: what a library is, and how its symbols
 * are found, is each system's own. */

tb_library_result tb_library_open(const char *name, tb_library_search search, tb_library **library,
                                  tb_library_error *error)
{
    (void)name;
    (void)search;
    (void)library;
    (void)error;
    tb_default_not_implemented("platform.library", "tb_library_open");
}

void tb_library_close(tb_library *library)
{
    (void)library;
    tb_default_not_implemented("platform.library", "tb_library_close");
}

tb_library_result tb_library_find_function(const tb_library *library, const char *name,
                                           tb_library_function *function, tb_library_error *error)
{
    (void)library;
    (void)name;
    (void)function;
    (void)error;
    tb_default_not_implemented("platform.library", "tb_library_find_function");
}

tb_library_result tb_library_find_data(const tb_library *library, const char *name, void **data,
                                       tb_library_error *error)
{
    (void)library;
    (void)name;
    (void)data;
    (void)error;
    tb_default_not_implemented("platform.library", "tb_library_find_data");
}

tb_library_result tb_library_bind(const tb_library *library, const tb_library_binding *bindings,
                                  size_t count, tb_library_error *error)
{
    (void)library;
    (void)bindings;
    (void)count;
    (void)error;
    tb_default_not_implemented("platform.library", "tb_library_bind");
}
