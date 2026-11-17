#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/log.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/thread.h"
#include "tb_test.h"
#include <string.h>

typedef struct capture
{
    char bytes[32768];
    size_t length;
    size_t records;
    size_t flushes;
    bool fail_write;
    bool fail_flush;
} capture;
static bool capture_write(void *context, const tb_log_record *record)
{
    capture *const state = context;
    if (state->fail_write)
    {
        return false;
    }
    if (record->length + 1 > sizeof(state->bytes) - state->length)
    {
        return false;
    }
    memcpy(state->bytes + state->length, record->message, record->length);
    state->length += record->length;
    state->bytes[state->length++] = '\n';
    state->records++;
    return true;
}
static bool capture_flush(void *context)
{
    capture *const state = context;
    state->flushes++;
    return !state->fail_flush;
}
static tb_log_config capture_config(capture *state)
{
    return (tb_log_config){TB_LOG_TRACE, NULL, capture_write, capture_flush, state};
}
TB_TEST(core_log, severity_category_filter_and_configuration_validation)
{
    tb_log *const log = tb_log_create();
    capture state = {0};
    tb_log_config config = {0};
    config = capture_config(&state);
    for (unsigned minimum = {0}; minimum <= TB_LOG_OFF; minimum++)
    {
        config.minimum = (tb_log_level)minimum;
        TB_TEST_REQUIRE_UINT(tb_log_configure(log, &config), ==, TB_LOG_OK);
        for (unsigned level = {0}; level < TB_LOG_OFF; level++)
        {
            TB_TEST_EXPECT_UINT(tb_log_write(log, (tb_log_level)level, "render", "x", 1), ==,
                                level >= minimum ? TB_LOG_OK : TB_LOG_FILTERED);
        }
    }
    config.minimum = TB_LOG_INFO;
    config.category = "files";
    TB_TEST_REQUIRE_UINT(tb_log_configure(log, &config), ==, TB_LOG_OK);
    TB_TEST_EXPECT_UINT(tb_log_printf(log, TB_LOG_INFO, "render", "%5000s", ""), ==,
                        TB_LOG_FILTERED);
    TB_TEST_EXPECT_UINT(tb_log_write(log, TB_LOG_INFO, "files", NULL, 0), ==, TB_LOG_OK);
    TB_TEST_EXPECT_UINT(tb_log_write(log, TB_LOG_OFF, "files", "x", 1), ==, TB_LOG_INVALID);
    TB_TEST_EXPECT_UINT(tb_log_write(log, (tb_log_level)-1, "files", "x", 1), ==, TB_LOG_INVALID);
    TB_TEST_EXPECT_UINT(tb_log_write(log, TB_LOG_INFO, "\xff", "x", 1), ==, TB_LOG_INVALID);
    TB_TEST_EXPECT_UINT(tb_log_write(log, TB_LOG_INFO, "files", "\xff", 1), ==, TB_LOG_INVALID);
    TB_TEST_EXPECT_UINT(tb_log_write(log, TB_LOG_INFO, "files", "x\0y", 3), ==, TB_LOG_INVALID);
    config.minimum = (tb_log_level)-1;
    TB_TEST_EXPECT_UINT(tb_log_configure(log, &config), ==, TB_LOG_INVALID);
    config.minimum = TB_LOG_INFO;
    config.category = "\xff";
    TB_TEST_EXPECT_UINT(tb_log_configure(log, &config), ==, TB_LOG_INVALID);
    config.category = NULL;
    config.write_record = NULL;
    TB_TEST_EXPECT_UINT(tb_log_configure(log, &config), ==, TB_LOG_INVALID);
    TB_TEST_EXPECT_UINT(tb_log_write(log, TB_LOG_INFO, "render", "x", 1), ==, TB_LOG_FILTERED);
    TB_TEST_EXPECT_UINT(tb_log_destroy(log), ==, TB_LOG_OK);
    TB_TEST_EXPECT_UINT(state.flushes, ==, 1);
    TB_TEST_EXPECT_UINT(tb_log_destroy(NULL), ==, TB_LOG_OK);
}
TB_TEST(core_log, format_limits_failures_and_recovery)
{
    tb_log *const log = tb_log_create();
    capture state = {0};
    tb_log_config config = {0};
    size_t allocations = {0};
    config = capture_config(&state);
    TB_TEST_REQUIRE_UINT(tb_log_configure(log, &config), ==, TB_LOG_OK);
    allocations = tb_memory_allocation_count();
    TB_TEST_EXPECT_UINT(
        tb_log_printf(log, TB_LOG_INFO, "test", "%s:%08x:%zu", "value", 42U, (size_t)7), ==,
        TB_LOG_OK);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, allocations);
    TB_TEST_EXPECT(memcmp(state.bytes, "value:0000002a:7\n", state.length) == 0);
    TB_TEST_EXPECT_UINT(tb_log_printf(log, TB_LOG_INFO, "test", "%4095s", ""), ==, TB_LOG_OK);
    TB_TEST_EXPECT_UINT(tb_log_printf(log, TB_LOG_INFO, "test", "%4096s", ""), ==,
                        TB_LOG_TOO_LARGE);
    TB_TEST_EXPECT_UINT(tb_log_printf(log, TB_LOG_INFO, "test", "%c", 0), ==, TB_LOG_INVALID);
    state.fail_write = true;
    TB_TEST_EXPECT_UINT(tb_log_printf(log, TB_LOG_ERROR, "test", "failed"), ==, TB_LOG_IO);
    state.fail_write = false;
    TB_TEST_EXPECT_UINT(tb_log_printf(log, TB_LOG_ERROR, "test", "recovered"), ==, TB_LOG_OK);
    state.fail_flush = true;
    TB_TEST_EXPECT_UINT(tb_log_flush(log), ==, TB_LOG_IO);
    state.fail_flush = false;
    TB_TEST_EXPECT_UINT(tb_log_flush(log), ==, TB_LOG_OK);
    config.flush = NULL;
    TB_TEST_EXPECT_UINT(tb_log_configure(log, &config), ==, TB_LOG_OK);
    TB_TEST_EXPECT_UINT(tb_log_flush(log), ==, TB_LOG_OK);
    config.flush = capture_flush;
    TB_TEST_EXPECT_UINT(tb_log_configure(log, &config), ==, TB_LOG_OK);
    state.fail_flush = true;
    TB_TEST_EXPECT_UINT(tb_log_destroy(log), ==, TB_LOG_IO);
}

typedef struct recursion
{
    tb_log *log;
    tb_log *other;
    tb_log_result results[6];
} recursion;
static bool recursive_write(void *context, const tb_log_record *record)
{
    recursion *const state = context;
    const tb_log_config config = TB_LOG_CONFIG_INIT;
    (void)record;
    state->results[0] = tb_log_write(state->log, TB_LOG_INFO, "nested", "x", 1);
    state->results[1] = tb_log_printf(state->other, TB_LOG_INFO, "nested", "x");
    state->results[2] = tb_log_flush(state->log);
    state->results[3] = tb_log_configure(state->other, &config);
    state->results[4] = tb_log_destroy(state->log);
    state->results[5] = tb_log_flush(state->other);
    return true;
}
TB_TEST(core_log, recursive_callbacks_are_rejected_across_loggers)
{
    recursion state = {0};
    tb_log_config config = {0};
    state.log = tb_log_create();
    state.other = tb_log_create();
    config = (tb_log_config){TB_LOG_TRACE, NULL, recursive_write, NULL, &state};
    TB_TEST_REQUIRE_UINT(tb_log_configure(state.log, &config), ==, TB_LOG_OK);
    TB_TEST_EXPECT_UINT(tb_log_printf(state.log, TB_LOG_INFO, "test", "outer"), ==, TB_LOG_OK);
    for (size_t index = {0}; index < TB_TEST_COUNT_OF(state.results); index++)
    {
        TB_TEST_EXPECT_UINT(state.results[index], ==, TB_LOG_REENTRANT);
    }
    TB_TEST_EXPECT_UINT(tb_log_destroy(state.log), ==, TB_LOG_OK);
    TB_TEST_EXPECT_UINT(tb_log_destroy(state.other), ==, TB_LOG_OK);
}

typedef struct writer
{
    tb_log *log;
    unsigned id;
    size_t failures;
} writer;
static void write_records(void *context)
{
    writer *const state = context;
    for (unsigned sequence = {0}; sequence < 200; sequence++)
    {
        if (tb_log_printf(state->log, TB_LOG_INFO, "workers", "<%u:%u>", state->id, sequence) !=
            TB_LOG_OK)
        {
            state->failures++;
        }
    }
}
TB_TEST(core_log, concurrent_records_and_configuration_changes_are_serialized)
{
    tb_log *const log = tb_log_create();
    capture states[2] = {0};
    tb_log_config config = {0};
    tb_thread threads[4] = {0};
    writer writers[4] = {0};
    config = capture_config(&states[0]);
    TB_TEST_REQUIRE_UINT(tb_log_configure(log, &config), ==, TB_LOG_OK);
    for (unsigned index = {0}; index < 4; index++)
    {
        writers[index] = (writer){log, index, 0};
        tb_thread_create(&threads[index], write_records, &writers[index]);
    }
    for (size_t change = {0}; change < 400; change++)
    {
        config.context = &states[change % 2];
        TB_TEST_REQUIRE_UINT(tb_log_configure(log, &config), ==, TB_LOG_OK);
    }
    for (size_t index = {0}; index < 4; index++)
    {
        tb_thread_join(&threads[index]);
        TB_TEST_EXPECT_UINT(writers[index].failures, ==, 0);
    }
    TB_TEST_EXPECT_UINT(states[0].records + states[1].records, ==, 800);
    for (unsigned id = {0}; id < 4; id++)
    {
        for (unsigned sequence = {0}; sequence < 200; sequence++)
        {
            char expected[32] = {0};
            size_t found = {0};
            (void)tb_fmt_buffer(expected, sizeof(expected), "<%u:%u>\n", id, sequence);
            for (size_t sink = {0}; sink < 2; sink++)
            {
                if (tb_str_find(states[sink].bytes, 0, TB_STR_END, expected) != TB_STR_NOT_FOUND)
                {
                    found++;
                }
            }
            TB_TEST_EXPECT_UINT(found, ==, 1);
        }
    }
    TB_TEST_EXPECT_UINT(tb_log_destroy(log), ==, TB_LOG_OK);
}
TB_TEST(core_log, native_file_sink_formats_records_and_borrows_the_handle)
{
    char path[32768] = {0};
    tb_file file = {0};
    tb_fs_buffer content = {0};
    tb_log *const log = tb_log_create();
    tb_log_config config = {0};
    TB_TEST_REQUIRE_UINT(tb_fs_temp_file((tb_path_view){0}, path, sizeof(path), &file), ==,
                         TB_FS_OK);
    config = (tb_log_config){TB_LOG_TRACE, NULL, tb_log_file_write, tb_log_file_flush, &file};
    TB_TEST_REQUIRE_UINT(tb_log_configure(log, &config), ==, TB_LOG_OK);
    TB_TEST_EXPECT_UINT(tb_log_printf(log, TB_LOG_WARNING, "files", "Unicode: \xc3\xa9 %u", 42U),
                        ==, TB_LOG_OK);
    TB_TEST_EXPECT_UINT(tb_log_flush(log), ==, TB_LOG_OK);
    TB_TEST_EXPECT_UINT(tb_log_destroy(log), ==, TB_LOG_OK);
    TB_TEST_EXPECT_UINT(tb_file_flush(&file), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(
        tb_fs_read_file((tb_path_view){path, tb_str_length(path)}, NULL, 1000, &content), ==,
        TB_FS_OK);
    TB_TEST_EXPECT_STRING((const char *)content.data, ==,
                          "[WARNING] [files] Unicode: \xc3\xa9 42\n");
    tb_fs_buffer_free(&content);
    /* Read-only handles report write failures without being consumed. */
    TB_TEST_REQUIRE_UINT(
        tb_file_open((tb_path_view){path, tb_str_length(path)}, TB_FILE_READ, &file), ==, TB_FS_OK);
    {
        const tb_log_record record = {TB_LOG_INFO, "test", "x", 1};
        TB_TEST_EXPECT(!tb_log_file_write(&file, &record));
    }
    TB_TEST_EXPECT_UINT(tb_file_close(&file), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove((tb_path_view){path, tb_str_length(path)}), ==, TB_FS_OK);
}
typedef struct bad_contract
{
    tb_log *log;
    size_t choice;
} bad_contract;
static void bad_log_contract(void *context)
{
    const bad_contract *const state = context;
    const tb_log_config config = TB_LOG_CONFIG_INIT;
    switch (state->choice)
    {
    case 0:
        (void)tb_log_flush(NULL);
        break;
    case 1:
        (void)tb_log_configure(NULL, &config);
        break;
    case 2:
        (void)tb_log_configure(state->log, NULL);
        break;
    case 3:
        (void)tb_log_write(NULL, TB_LOG_INFO, "x", "x", 1);
        break;
    case 4:
        (void)tb_log_write(state->log, TB_LOG_INFO, NULL, "x", 1);
        break;
    case 5:
        (void)tb_log_write(state->log, TB_LOG_INFO, "x", NULL, 1);
        break;
    case 6:
        (void)tb_log_printf(state->log, TB_LOG_INFO, "x", NULL);
        break;
    case 7:
        (void)tb_log_file_flush(NULL);
        break;
    case 8:
        (void)tb_log_file_write(NULL, NULL);
        break;
    case 9:
        (void)tb_log_stderr_write(NULL, NULL);
        break;
    default:
        break;
    }
}
TB_TEST(core_log, required_logger_contracts_are_asserted)
{
    bad_contract state = {0};
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    state.log = tb_log_create();
    for (size_t choice = {0}; choice < 10; choice++)
    {
        state.choice = choice;
        TB_TEST_EXPECT_TRAP(bad_log_contract, &state, "assertion failed");
    }
    TB_TEST_EXPECT_UINT(tb_log_destroy(state.log), ==, TB_LOG_OK);
}
