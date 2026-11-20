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

/* Room for a line of a report: a frame's number, function, module, and file,
 * each at the most its location holds, or the message. */
#define TB_FATAL_LINE_CAPACITY                                                                     \
    (TB_STACK_FUNCTION_CAPACITY + TB_STACK_MODULE_CAPACITY + TB_STACK_FILE_CAPACITY + 64)

/* Adds formatted text to a line, cut off where the line is full. */
static void tb_fatal_append(char *line, size_t *length, const char *format, ...)
{
    va_list arguments;
    size_t room = 0;
    size_t written = 0;

    if (*length >= TB_FATAL_LINE_CAPACITY - 1)
    {
        return;
    }
    room = TB_FATAL_LINE_CAPACITY - *length;
    va_start(arguments, format);
    written = tb_fmt_vbuffer(line + *length, room, format, arguments);
    va_end(arguments);
    /* The count is of the whole output, which may not have fit. */
    *length += written < room ? written : room - 1;
}

static void tb_fatal_write_frame(size_t number, const void *address)
{
    tb_stack_location location = {0};
    char line[TB_FATAL_LINE_CAPACITY] = {0};
    size_t length = 0;

    tb_stack_describe(address, &location);
    tb_fatal_append(line, &length, "    #%zu", number);
    if (location.function[0] != '\0')
    {
        tb_fatal_append(line, &length, " %s", location.function);
    }
    if (location.module[0] != '\0')
    {
        tb_fatal_append(line, &length, " (%s+%#zx)", location.module, location.module_offset);
    }
    else
    {
        tb_fatal_append(line, &length, " (%p)", address);
    }
    if (location.file[0] != '\0')
    {
        tb_fatal_append(line, &length, " %s:%u", location.file, (unsigned)location.line);
    }
    tb_fatal_append(line, &length, "\n");
    tb_platform_write_error(line, length);
}

void tb_fatal_write(const tb_fatal_report *report)
{
    char line[TB_FATAL_MESSAGE_CAPACITY + 2] = {0};
    size_t length = 0;

    TB_ASSERT(report != NULL);
    tb_fatal_is_writing = true;
    length = tb_fmt_buffer(line, sizeof(line), "%s\n", report->message);
    if (length >= sizeof(line))
    {
        length = sizeof(line) - 1;
    }
    tb_platform_write_error(line, length);
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
 * condition. Output is flushed first only when `may_flush`: a crashed thread
 * may hold a stream's lock. */
static _Noreturn void tb_fatal_end(char *message, void *const *frames, size_t frame_count,
                                   bool may_flush)
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
    if (may_flush)
    {
        (void)tb_fmt_flush();
    }
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
    tb_fatal_end(message, frames, frame_count, true);
}

void tb_fatal_message(const char *message)
{
    char copy[TB_FATAL_MESSAGE_CAPACITY] = {0};
    void *frames[TB_FATAL_FRAMES_MAX] = {0};
    size_t frame_count = 0;

    TB_ASSERT(message != NULL);
    frame_count = tb_stack_capture(frames, TB_FATAL_FRAMES_MAX, 0);
    (void)tb_fmt_buffer(copy, sizeof(copy), "%s", message);
    tb_fatal_end(copy, frames, frame_count, true);
}

void tb_fatal_with_stack(const char *message, void *const *frames, size_t frame_count)
{
    char copy[TB_FATAL_MESSAGE_CAPACITY] = {0};

    TB_ASSERT(message != NULL);
    TB_ASSERT(frames != NULL || frame_count == 0);
    (void)tb_fmt_buffer(copy, sizeof(copy), "%s", message);
    if (frame_count > TB_FATAL_FRAMES_MAX)
    {
        frame_count = TB_FATAL_FRAMES_MAX;
    }
    tb_fatal_end(copy, frames, frame_count, false);
}
