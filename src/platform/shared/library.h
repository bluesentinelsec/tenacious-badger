/* Private to the platform adapters, and shared by them: what every adapter
 * of badger/library.h does the same way. A library's record, the checks of a
 * name, and the message of an error are here; loading and finding are the
 * adapter's own. */
#ifndef TB_PLATFORM_LIBRARY_H
#define TB_PLATFORM_LIBRARY_H

#include "badger/library.h"

#include <stdbool.h>
#include <stddef.h>

/* What tb_library_open() returns: the system's handle, and the name the
 * library was opened by, for the messages of later failures. */
struct tb_library
{
    void *handle;
    const char *name; /* Within the record's own allocation, after it. */
};

/* A record for `name` with no handle yet, in one allocation from malloc;
 * NULL when there is no memory. The adapter loads the library into it, or
 * frees it. */
tb_library *tb_native_library_record(const char *name);

/* Whether `name` is a file name: UTF-8, not empty, not "." or "..", and
 * without a separator of any system ('/', '\\', ':'). Required non-null. */
bool tb_native_library_is_file_name(const char *name);

/* Whether `name` is valid UTF-8 with no NUL before its terminator. */
bool tb_native_library_is_text(const char *name);

/* Starts a failure's message: sets `error`'s result and writes "subject: ".
 * Returns `result`. Every function here accepts a NULL `error`, and then
 * does nothing. */
tb_library_result tb_native_library_fail(tb_library_error *error, tb_library_result result,
                                         const char *subject);

/* Adds `text` to the message, cut at a character when it does not fit. A
 * NULL `text` adds nothing. */
void tb_native_library_say(tb_library_error *error, const char *text);

/* Clears `error` for a call that succeeded, and returns TB_LIBRARY_OK. */
tb_library_result tb_native_library_succeed(tb_library_error *error);

/* The message of a library that would not load: "name: not loaded from
 * <where>: <reason>", where `reason` is the system's, or NULL. */
tb_library_result tb_native_library_not_loaded(tb_library_error *error, const char *name,
                                               tb_library_search search, const char *reason);

/* The message of a symbol that is missing: "library: missing name". */
tb_library_result tb_native_library_missing(tb_library_error *error, const tb_library *library,
                                            const char *name);

#endif
