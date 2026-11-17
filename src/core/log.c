#include "badger/log.h"
#include "badger/assert.h"
#include "badger/file.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/thread.h"
#include "badger/utf8.h"
#include <limits.h>
#include <string.h>

struct tb_log
{
    tb_mutex lock;
    tb_log_config config;
};
/* Across all loggers: recursive cross-logger calls can otherwise invert locks. */
static _Thread_local bool tb_log_inside = {0};
static bool tb_log_text_valid(const char *bytes, size_t length)
{
    size_t error = {0};
    return tb_utf8_validate(bytes, length, &error) &&
           (length == 0 || memchr(bytes, 0, length) == NULL);
}
static bool tb_log_enter(tb_log *log)
{
    if (tb_log_inside)
    {
        return false;
    }
    tb_log_inside = true;
    tb_mutex_lock(&log->lock);
    return true;
}
static tb_log_result tb_log_leave(tb_log *log, tb_log_result result)
{
    tb_mutex_unlock(&log->lock);
    tb_log_inside = false;
    return result;
}
tb_log *tb_log_create(void)
{
    tb_log *log = NULL;
    log = TB_NEW(tb_log);
    log->config = (tb_log_config)TB_LOG_CONFIG_INIT;
    return log;
}
tb_log_result tb_log_destroy(tb_log *log)
{
    tb_log_result result = {0};
    if (tb_log_inside)
    {
        return TB_LOG_REENTRANT;
    }
    if (log == NULL)
    {
        return TB_LOG_OK;
    }
    result = tb_log_flush(log);
    tb_free(log);
    return result;
}
tb_log_result tb_log_configure(tb_log *log, const tb_log_config *config)
{
    TB_ASSERT(log != NULL && config != NULL);
    if (!tb_log_enter(log))
    {
        return TB_LOG_REENTRANT;
    }
    if ((unsigned)config->minimum > TB_LOG_OFF || config->write_record == NULL ||
        (config->category != NULL &&
         !tb_log_text_valid(config->category, tb_str_length(config->category))))
    {
        return tb_log_leave(log, TB_LOG_INVALID);
    }
    log->config = *config;
    return tb_log_leave(log, TB_LOG_OK);
}
static tb_log_result tb_log_filter(const tb_log *log, tb_log_level level, const char *category)
{
    if ((unsigned)level >= TB_LOG_OFF || !tb_log_text_valid(category, tb_str_length(category)))
    {
        return TB_LOG_INVALID;
    }
    if (level < log->config.minimum ||
        (log->config.category != NULL && !tb_str_equal(category, log->config.category)))
    {
        return TB_LOG_FILTERED;
    }
    return TB_LOG_OK;
}
static tb_log_result tb_log_emit(tb_log *log, tb_log_level level, const char *category,
                                 const char *message, size_t length)
{
    const tb_log_record record = {level, category, message != NULL ? message : "", length};
    if (!tb_log_text_valid(message, length))
    {
        return TB_LOG_INVALID;
    }
    return log->config.write_record(log->config.context, &record) ? TB_LOG_OK : TB_LOG_IO;
}
tb_log_result tb_log_write(tb_log *log, tb_log_level level, const char *category,
                           const char *message, size_t length)
{
    tb_log_result result = {0};
    TB_ASSERT(log != NULL && category != NULL && (message != NULL || length == 0));
    if (!tb_log_enter(log))
    {
        return TB_LOG_REENTRANT;
    }
    result = tb_log_filter(log, level, category);
    if (result == TB_LOG_OK)
    {
        result = tb_log_emit(log, level, category, message, length);
    }
    return tb_log_leave(log, result);
}
tb_log_result tb_log_vprintf(tb_log *log, tb_log_level level, const char *category,
                             const char *format, va_list arguments)
{
    char message[TB_LOG_MESSAGE_CAPACITY] = {0};
    size_t length = {0};
    tb_log_result result = {0};
    TB_ASSERT(log != NULL && category != NULL && format != NULL);
    if (!tb_log_enter(log))
    {
        return TB_LOG_REENTRANT;
    }
    result = tb_log_filter(log, level, category);
    if (result == TB_LOG_OK)
    {
        length = tb_fmt_vbuffer(message, sizeof(message), format, arguments);
        result = length < sizeof(message) ? tb_log_emit(log, level, category, message, length)
                                          : TB_LOG_TOO_LARGE;
    }
    return tb_log_leave(log, result);
}
tb_log_result tb_log_printf(tb_log *log, tb_log_level level, const char *category,
                            const char *format, ...)
{
    va_list arguments;
    tb_log_result result = {0};
    va_start(arguments, format);
    result = tb_log_vprintf(log, level, category, format, arguments);
    va_end(arguments);
    return result;
}
tb_log_result tb_log_flush(tb_log *log)
{
    tb_log_result result = TB_LOG_OK;
    TB_ASSERT(log != NULL);
    if (!tb_log_enter(log))
    {
        return TB_LOG_REENTRANT;
    }
    if (log->config.flush != NULL && !log->config.flush(log->config.context))
    {
        result = TB_LOG_IO;
    }
    return tb_log_leave(log, result);
}

typedef struct tb_log_output
{
    tb_file *file; /* NULL selects stderr. */
    bool failed;
} tb_log_output;
static void tb_log_output_bytes(void *context, const char *bytes, size_t length)
{
    tb_log_output *const output = context;
    size_t offset = {0};
    while (offset < length && !output->failed)
    {
        size_t written = {0};
        if (output->file != NULL)
        {
            output->failed = tb_file_write(output->file, bytes + offset, length - offset,
                                           &written) != TB_FS_OK ||
                             written == 0;
        }
        else
        {
            written = length - offset;
            if (written > INT_MAX)
            {
                written = INT_MAX;
            }
            (void)tb_fmt_print_error("%.*s", (int)written, bytes + offset);
        }
        offset += written;
    }
}
static bool tb_log_output_record(tb_file *file, const tb_log_record *record)
{
    static const char *const levels[] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
    tb_log_output output = {0};
    TB_ASSERT(record != NULL && (unsigned)record->level < TB_LOG_OFF);
    output.file = file;
    (void)tb_fmt_write(tb_log_output_bytes, &output, "[%s] [%s] ", levels[record->level],
                       record->category);
    tb_log_output_bytes(&output, record->message, record->length);
    tb_log_output_bytes(&output, "\n", 1);
    return file != NULL ? !output.failed : tb_fmt_flush_error();
}
bool tb_log_stderr_write(void *context, const tb_log_record *record)
{
    (void)context;
    return tb_log_output_record(NULL, record);
}
bool tb_log_stderr_flush(void *context)
{
    (void)context;
    return tb_fmt_flush_error();
}
bool tb_log_file_write(void *context, const tb_log_record *record)
{
    TB_ASSERT(context != NULL);
    return tb_log_output_record(context, record);
}
bool tb_log_file_flush(void *context)
{
    TB_ASSERT(context != NULL);
    return tb_file_flush(context) == TB_FS_OK;
}

const char *tb_log_result_name(tb_log_result result)
{
    switch (result)
    {
    case TB_LOG_OK:
        return "ok";
    case TB_LOG_FILTERED:
        return "filtered";
    case TB_LOG_INVALID:
        return "invalid argument";
    case TB_LOG_IO:
        return "sink failed";
    case TB_LOG_REENTRANT:
        return "called from a logger callback";
    case TB_LOG_TOO_LARGE:
        return "too large";
    }
    return "unknown result";
}
