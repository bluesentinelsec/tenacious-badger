/* Private dynamic-loader operation for Linux adapters. */
#ifndef TB_LINUX_LIBRARY_LINUX_H
#define TB_LINUX_LIBRARY_LINUX_H

#include <stdbool.h>

/* Keep an already loaded system library mapped until process exit. The
 * caller supplies its soname or absolute path. Return false if the library
 * is absent, without loading it. */
bool tb_linux_library_keep_loaded(const char *name);

#endif
