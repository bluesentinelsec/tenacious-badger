/* Private to the Linux and macOS adapters: where each finds the system's
 * libraries, which is all that differs between them. */
#ifndef TB_POSIX_LIBRARY_H
#define TB_POSIX_LIBRARY_H

#include "badger/library.h"

/* What to give dlopen() for a system library's name: the name itself, for
 * the dynamic linker to search, or a full path. On success `*path` is from
 * malloc. Returns TB_LIBRARY_OK, INVALID with the message written, or
 * NO_MEMORY. */
tb_library_result tb_posix_library_system_path(const char *name, char **path,
                                               tb_library_error *error);

#endif
