#include "badger/assert.h"

#include "badger/fatal.h"

#include "badger/fmt.h"
#include "badger/platform.h"

/** Installed failure handler; a NULL handler selects the default report. */
typedef struct tb_assert_binding
{
    tb_assert_handler *handler;
    void *context;
} tb_assert_binding;

static tb_assert_binding tb_assert_installed = {0};

/* The default report: a fatal condition like any other, so that it is written
 * with the call stack and goes to the handler of badger/fatal.h. */
static _Noreturn void tb_assert_report_as_fatal(const tb_assert_failure *failure)
{
    if (failure->message != NULL)
    {
        tb_fatal("%s:%d: assertion failed in %s: %s (%s)", failure->file, failure->line,
                 failure->function, failure->expression, failure->message);
    }
    tb_fatal("%s:%d: assertion failed in %s: %s", failure->file, failure->line, failure->function,
             failure->expression);
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
        tb_assert_report_as_fatal(&failure);
    }
    /* Reached by a handler that returned: the caller's invariant is broken,
     * so continuing is never an option. */
    (void)tb_fmt_flush();
    tb_platform_abort();
}

bool tb_assert_library_enabled(void)
{
#if TB_ASSERT_ENABLED
    return true;
#else
    return false;
#endif
}
