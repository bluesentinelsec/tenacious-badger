#include "badger/assert.h"

#include "badger/platform.h"

#include <stdio.h>

/** Installed failure handler; a NULL handler selects the default report. */
typedef struct tb_assert_binding
{
    tb_assert_handler *handler;
    void *context;
} tb_assert_binding;

static tb_assert_binding tb_assert_installed = {0};

static void tb_assert_report_to_stderr(const tb_assert_failure *failure)
{
    if (failure->message != NULL)
    {
        fprintf(stderr, "%s:%d: assertion failed in %s: %s (%s)\n", failure->file, failure->line,
                failure->function, failure->expression, failure->message);
        return;
    }
    fprintf(stderr, "%s:%d: assertion failed in %s: %s\n", failure->file, failure->line,
            failure->function, failure->expression);
}

void tb_assert_set_handler(tb_assert_handler *handler, void *context)
{
    tb_assert_installed.handler = handler;
    tb_assert_installed.context = context;
}

void tb_assert_fail(const char *expression, const char *message, const char *file, int line,
                    const char *function)
{
    const tb_assert_failure failure = {.expression = expression,
                                       .message = message,
                                       .file = file,
                                       .function = function,
                                       .line = line};

    if (tb_assert_installed.handler != NULL)
    {
        tb_assert_installed.handler(&failure, tb_assert_installed.context);
    }
    else
    {
        tb_assert_report_to_stderr(&failure);
    }
    /* Reached by the default report and by a handler that returned: the caller's
     * invariant is broken, so continuing is never an option. */
    (void)fflush(stderr);
    tb_platform_abort();
}

bool tb_assert_library_enabled(void)
{
    return TB_ASSERT_ENABLED != 0;
}
