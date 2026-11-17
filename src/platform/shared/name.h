/* Private to the platform adapters, and shared by them: fitting a name into
 * a buffer of fixed size. Systems keep names of different lengths, and a
 * name that is cut must not end in the middle of a character. */
#ifndef TB_PLATFORM_NAME_H
#define TB_PLATFORM_NAME_H

#include <stddef.h>

/* Copies `name`, which is UTF-8 and NUL-terminated, into `destination`, of
 * `capacity` bytes, and terminates it. A name that does not fit is cut at
 * the last character that does, never inside one. Returns the bytes copied,
 * without the terminator. `destination` is required non-null and `capacity`
 * at least one; a NULL `name` is copied as the empty name. */
size_t tb_platform_copy_name(char *destination, size_t capacity, const char *name);

#endif
