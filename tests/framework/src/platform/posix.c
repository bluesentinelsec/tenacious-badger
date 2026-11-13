/* SIGTRAP is a POSIX signal; request the POSIX API from the strict C17 headers. */
#define _POSIX_C_SOURCE 200809L

#include "../platform.h"

#include <signal.h>
#include <stdio.h>

void tb_test_platform_disable_error_dialogs(void)
{
    /* POSIX systems report crashes through signals and exit statuses only. */
}

void tb_test_platform_debug_break(void)
{
    (void)raise(SIGTRAP);
}

void tb_test_platform_print_location(const char *file, int line)
{
    /* GCC and Clang diagnostic form: editors and IDEs link `file:line`. */
    printf("%s:%d", file, line);
}
