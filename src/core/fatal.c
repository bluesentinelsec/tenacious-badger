#include "badger/fatal.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/platform.h"
#include "badger/stack.h"
#include "badger/str.h"

#include <stdarg.h>
#include <stdbool.h>

typedef struct tb_fatal_binding
{
    tb_fatal_handler *handler; /* NULL selects the default report. */
    void *context;
} tb_fatal_binding;

static tb_fatal_binding tb_fatal_installed = {0};

/* Set while the calling thread writes a report. A fatal condition that is met
 * then comes from the writing itself, and reporting it would not end. */
static _Thread_local bool tb_fatal_is_writing = false;

void tb_fatal_set_handler(tb_fatal_handler *handler, void *context)
{
    tb_fatal_installed.handler = handler;
    tb_fatal_installed.context = context;
}

static void tb_fatal_write_frame(size_t number, const void *address)
{
    tb_stack_location location = {0};

    tb_stack_describe(address, &location);
    (void)tb_fmt_print_error("    #%zu", number);
    if (location.function[0] != '\0')
    {
        (void)tb_fmt_print_error(" %s", location.function);
    }
    if (location.module[0] != '\0')
    {
        (void)tb_fmt_print_error(" (%s+%#zx)", location.module, location.module_offset);
    }
    else
    {
        (void)tb_fmt_print_error(" (%p)", address);
    }
    if (location.file[0] != '\0')
    {
        (void)tb_fmt_print_error(" %s:%u", location.file, (unsigned)location.line);
    }
    (void)tb_fmt_print_error("\n");
}

void tb_fatal_write(const tb_fatal_report *report)
{
    TB_ASSERT(report != NULL);
    tb_fatal_is_writing = true;
    (void)tb_fmt_print_error("%s\n", report->message);
    for (size_t index = 0; index < report->frame_count; index++)
    {
        tb_fatal_write_frame(index, report->frames[index]);
    }
    tb_fatal_is_writing = false;
}

/* Drops one newline from the end of a message, which a caller that used to
 * write to a stream may still have there. */
static void tb_fatal_trim(char *message)
{
    const size_t length = tb_str_length(message);

    if (length > 0 && message[length - 1] == '\n')
    {
        message[length - 1] = '\0';
    }
}

/* Reports and ends the process. `frames` is the stack of whoever met the
 * condition, captured by the function that it called. */
static _Noreturn void tb_fatal_end(char *message, void *const *frames, size_t frame_count)
{
    const tb_fatal_report report = {
        .message = message, .frames = frames, .frame_count = frame_count};

    /* coverage-off: needs the writing of a report to meet a fatal condition of its own */
    if (tb_fatal_is_writing)
    {
        tb_platform_abort();
    }
    /* coverage-on */
    tb_fatal_trim(message);
    if (tb_fatal_installed.handler != NULL)
    {
        tb_fatal_installed.handler(&report, tb_fatal_installed.context);
    }
    else
    {
        tb_fatal_write(&report);
    }
    /* Reached by the default report and by a handler that returned. */
    (void)tb_fmt_flush();
    tb_platform_abort();
}

void tb_fatal(const char *format, ...)
{
    char message[TB_FATAL_MESSAGE_CAPACITY] = {0};
    void *frames[TB_FATAL_FRAMES_MAX] = {0};
    size_t frame_count = 0;
    va_list arguments;

    TB_ASSERT(format != NULL);
    /* Captured here and not further in, so that the first frame is this
     * function, or its caller where a build has joined the two, in any build. */
    frame_count = tb_stack_capture(frames, TB_FATAL_FRAMES_MAX, 0);
    va_start(arguments, format);
    (void)tb_fmt_vbuffer(message, sizeof(message), format, arguments);
    va_end(arguments);
    tb_fatal_end(message, frames, frame_count);
}

void tb_fatal_message(const char *message)
{
    char copy[TB_FATAL_MESSAGE_CAPACITY] = {0};
    void *frames[TB_FATAL_FRAMES_MAX] = {0};
    size_t frame_count = 0;

    TB_ASSERT(message != NULL);
    frame_count = tb_stack_capture(frames, TB_FATAL_FRAMES_MAX, 0);
    (void)tb_fmt_buffer(copy, sizeof(copy), "%s", message);
    tb_fatal_end(copy, frames, frame_count);
}
