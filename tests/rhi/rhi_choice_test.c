/* How the RHI chooses a backend, as `graphics.renderer` asks: `auto` tries a
 * platform's backends in its order of preference and passes over any that
 * is not written yet or unavailable, saying why in the log; a backend named
 * outright is the only one tried; and one the platform does not have is
 * "not implemented", with the next step. Lists of backends made up here,
 * of the null backend's objects, stand in for a platform's, so every
 * platform runs every case. */
#include "rhi/rhi_backend.h"

#include "badger/fatal.h"
#include "badger/log.h"
#include "badger/str.h"

#include "tb_test.h"

#include <string.h>

/* A fatal condition, as a trap the case can expect. */
static void trap_fatal(const tb_fatal_report *report, void *context)
{
    (void)context;
    tb_test_trap(__FILE__, __LINE__, report->message);
}

/* What the log heard, each record on a line of its own. */
typedef struct transcript
{
    char text[4096];
    size_t length;
} transcript;

static bool hear(void *context, const tb_log_record *record)
{
    transcript *const log = context;
    const size_t room = sizeof(log->text) - log->length - 1;
    const size_t length = record->length < room ? record->length : room;

    memcpy(log->text + log->length, record->message, length);
    log->length += length;
    if (log->length + 1 < sizeof(log->text))
    {
        log->text[log->length] = '\n';
        log->length++;
    }
    log->text[log->length] = '\0';
    return true;
}

typedef struct listener
{
    tb_log *log;
    transcript heard;
} listener;

static void start_listening(listener *opened)
{
    tb_log_config config = TB_LOG_CONFIG_INIT;

    *opened = (listener){0};
    opened->log = tb_log_create();
    config.minimum = TB_LOG_TRACE;
    config.write_record = hear;
    config.flush = NULL;
    config.context = &opened->heard;
    TB_TEST_REQUIRE_UINT(tb_log_configure(opened->log, &config), ==, TB_LOG_OK);
}

static void stop_listening(listener *opened)
{
    (void)tb_log_destroy(opened->log);
}

/* How often each made-up backend was asked for a device. */
static size_t attempts[TB_RHI_BACKEND_COUNT];

static tb_rhi_result refuse_device(const tb_rhi_device_options *options, void **state,
                                   tb_rhi_device_info *info, tb_rhi_error *error)
{
    (void)options;
    (void)state;
    attempts[info->backend]++;
    return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "%s: no GPU here",
                       tb_rhi_backend_name(info->backend));
}

static tb_rhi_result make_device(const tb_rhi_device_options *options, void **state,
                                 tb_rhi_device_info *info, tb_rhi_error *error)
{
    attempts[info->backend]++;
    return tb_rhi_null_backend.device_create(options, state, info, error);
}

/* A backend of the null backend's objects that calls itself `backend`, and
 * makes a device or refuses to. */
static tb_rhi_backend_table made_up(tb_rhi_backend backend, bool available)
{
    tb_rhi_backend_table table = tb_rhi_null_backend;

    table.backend = backend;
    table.device_create = available ? make_device : refuse_device;
    return table;
}

static void forget_attempts(void)
{
    memset(attempts, 0, sizeof(attempts));
}

/* A platform whose first choice is not written yet, whose second is
 * unavailable on this machine, and whose third works. */
typedef struct testland
{
    tb_rhi_backend_table d3d12;
    tb_rhi_backend_table opengl;
    tb_rhi_backend_entry entries[3];
    tb_rhi_backend_list list;
} testland;

static void make_testland(testland *platform, bool opengl_available)
{
    platform->d3d12 = made_up(TB_RHI_BACKEND_D3D12, false);
    platform->opengl = made_up(TB_RHI_BACKEND_OPENGL, opengl_available);
    platform->entries[0] = (tb_rhi_backend_entry){TB_RHI_BACKEND_VULKAN, NULL};
    platform->entries[1] = (tb_rhi_backend_entry){TB_RHI_BACKEND_D3D12, &platform->d3d12};
    platform->entries[2] = (tb_rhi_backend_entry){TB_RHI_BACKEND_OPENGL, &platform->opengl};
    platform->list = (tb_rhi_backend_list){"Testland", platform->entries, 3};
    forget_attempts();
}

static tb_rhi_backend backend_of(const tb_rhi_device *device)
{
    tb_rhi_device_info info = {0};

    tb_rhi_device_info_of(device, &info);
    return info.backend;
}

TB_TEST(rhi_choice, auto_falls_back_in_order_and_says_why)
{
    testland platform = {0};
    listener heard = {0};
    tb_rhi_device *device = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    make_testland(&platform, true);
    start_listening(&heard);
    const tb_rhi_device_options options = {.backend = TB_RHI_BACKEND_AUTOMATIC, .log = heard.log};
    TB_TEST_REQUIRE_UINT(tb_rhi_device_create_from(&platform.list, &options, &device, &error), ==,
                         TB_RHI_OK);
    TB_TEST_EXPECT_UINT(backend_of(device), ==, TB_RHI_BACKEND_OPENGL);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(attempts[TB_RHI_BACKEND_D3D12], ==, 1);
    TB_TEST_EXPECT_UINT(attempts[TB_RHI_BACKEND_OPENGL], ==, 1);
    TB_TEST_EXPECT_STRING(heard.heard.text, ==,
                          "auto passed over vulkan: not implemented on Testland yet\n"
                          "d3d12 is unavailable: d3d12: no GPU here\n"
                          "using opengl on null, as graphics.renderer auto chose\n");
    tb_rhi_device_destroy(device);
    stop_listening(&heard);
}

/* With no backend that works, `auto` fails and says so; it never falls
 * back to the null backend, which draws nothing. */
TB_TEST(rhi_choice, auto_without_a_working_backend_is_unavailable)
{
    testland platform = {0};
    listener heard = {0};
    tb_rhi_device *device = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    make_testland(&platform, false);
    start_listening(&heard);
    const tb_rhi_device_options options = {.backend = TB_RHI_BACKEND_AUTOMATIC, .log = heard.log};
    TB_TEST_EXPECT_UINT(tb_rhi_device_create_from(&platform.list, &options, &device, &error), ==,
                        TB_RHI_UNAVAILABLE);
    TB_TEST_EXPECT(device == NULL);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_RHI_UNAVAILABLE);
    TB_TEST_EXPECT_STRING(error.message, ==,
                          "no backend of Testland can make a device; the log says why of each");
    TB_TEST_EXPECT(tb_str_find(heard.heard.text, 0, TB_STR_END,
                               "opengl is unavailable: opengl: no GPU here\n") != TB_STR_NOT_FOUND);
    TB_TEST_EXPECT(tb_str_find(heard.heard.text, 0, TB_STR_END,
                               "auto found no backend of Testland") != TB_STR_NOT_FOUND);
    stop_listening(&heard);

    /* Nor with no backend at all, as on a platform the engine does not
     * support; and without a log or an error, it says nothing. */
    const tb_rhi_backend_list empty = {"Nowhere", NULL, 0};
    const tb_rhi_device_options quiet = {.backend = TB_RHI_BACKEND_AUTOMATIC};
    TB_TEST_EXPECT_UINT(tb_rhi_device_create_from(&empty, &quiet, &device, NULL), ==,
                        TB_RHI_UNAVAILABLE);
    TB_TEST_EXPECT(device == NULL);
}

/* A backend named outright is the only one tried: an earlier one in the
 * order is not, and an unavailable one is reported, not passed over. */
TB_TEST(rhi_choice, a_named_backend_is_the_only_one_tried)
{
    testland platform = {0};
    listener heard = {0};
    tb_rhi_device *device = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    make_testland(&platform, true);
    start_listening(&heard);
    const tb_rhi_device_options opengl = {.backend = TB_RHI_BACKEND_OPENGL, .log = heard.log};
    TB_TEST_REQUIRE_UINT(tb_rhi_device_create_from(&platform.list, &opengl, &device, &error), ==,
                         TB_RHI_OK);
    TB_TEST_EXPECT_UINT(backend_of(device), ==, TB_RHI_BACKEND_OPENGL);
    TB_TEST_EXPECT_UINT(attempts[TB_RHI_BACKEND_D3D12], ==, 0);
    TB_TEST_EXPECT_STRING(heard.heard.text, ==,
                          "using opengl on null, as graphics.renderer opengl chose\n");
    tb_rhi_device_destroy(device);

    const tb_rhi_device_options d3d12 = {.backend = TB_RHI_BACKEND_D3D12, .log = heard.log};
    TB_TEST_EXPECT_UINT(tb_rhi_device_create_from(&platform.list, &d3d12, &device, &error), ==,
                        TB_RHI_UNAVAILABLE);
    TB_TEST_EXPECT(device == NULL);
    TB_TEST_EXPECT_STRING(error.message, ==, "d3d12: no GPU here");
    TB_TEST_EXPECT_UINT(attempts[TB_RHI_BACKEND_OPENGL], ==, 1);
    TB_TEST_EXPECT(tb_str_find(heard.heard.text, 0, TB_STR_END,
                               "d3d12 is unavailable: d3d12: no GPU here\n") != TB_STR_NOT_FOUND);
    stop_listening(&heard);
}

/* Every platform has the null backend, which no list names. */
TB_TEST(rhi_choice, every_platform_has_the_null_backend)
{
    const tb_rhi_backend_list empty = {"Nowhere", NULL, 0};
    const tb_rhi_device_options options = {.backend = TB_RHI_BACKEND_NULL};
    tb_rhi_device *device = NULL;

    TB_TEST_REQUIRE_UINT(tb_rhi_device_create_from(&empty, &options, &device, NULL), ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(backend_of(device), ==, TB_RHI_BACKEND_NULL);
    tb_rhi_device_destroy(device);
}

/* The backend a trap case names, on the platform it names. */
typedef struct named
{
    const tb_rhi_backend_list *list;
    tb_rhi_backend backend;
} named;

static void create_named(void *context)
{
    const named *const choice = context;
    const tb_rhi_device_options options = {.backend = choice->backend};
    tb_rhi_device *device = NULL;

    (void)tb_rhi_device_create_from(choice->list, &options, &device, NULL);
    tb_rhi_device_destroy(device);
}

/* A backend the platform lists but has not written, or does not build at
 * all, named outright, is "not implemented": the process ends, naming the
 * backend, the platform, and the next step, for a setting and a flag. */
TB_TEST(rhi_choice, a_backend_the_platform_lacks_is_not_implemented)
{
    testland platform = {0};
    named choice = {0};

    make_testland(&platform, true);
    tb_fatal_set_handler(trap_fatal, NULL);
    choice.list = &platform.list;
    choice.backend = TB_RHI_BACKEND_VULKAN;
    TB_TEST_EXPECT_TRAP(create_named, &choice,
                        "rhi: tb_rhi_device_create: the vulkan backend is not implemented on "
                        "Testland, not written yet; set graphics.renderer to auto, or run with "
                        "--renderer=auto");
    choice.backend = TB_RHI_BACKEND_METAL;
    TB_TEST_EXPECT_TRAP(create_named, &choice,
                        "rhi: tb_rhi_device_create: the metal backend is not implemented on "
                        "Testland, which does not build it; set graphics.renderer to auto, or "
                        "run with --renderer=auto");
    tb_fatal_set_handler(NULL, NULL);
}

/* This platform's own list: real backends, each once, never `auto` or the
 * null backend; and naming any backend it lacks is "not implemented". */
TB_TEST(rhi_choice, this_platform_lists_its_backends_in_order)
{
    const tb_rhi_backend_list *const list = tb_rhi_platform_backends();
    bool listed[TB_RHI_BACKEND_COUNT] = {0};
    named choice = {.list = list, .backend = TB_RHI_BACKEND_AUTOMATIC};

    TB_TEST_REQUIRE(list != NULL);
    TB_TEST_EXPECT(list->platform != NULL && list->platform[0] != '\0');
    for (size_t index = 0; index < list->count; index++)
    {
        const tb_rhi_backend_entry *const entry = &list->entries[index];
        TB_TEST_CONTEXT("entry %zu", index);
        TB_TEST_REQUIRE_UINT(entry->backend, <, TB_RHI_BACKEND_COUNT);
        TB_TEST_EXPECT_UINT(entry->backend, !=, TB_RHI_BACKEND_AUTOMATIC);
        TB_TEST_EXPECT_UINT(entry->backend, !=, TB_RHI_BACKEND_NULL);
        TB_TEST_EXPECT(!listed[entry->backend]);
        listed[entry->backend] = true;
        if (entry->table != NULL)
        {
            TB_TEST_EXPECT_UINT(entry->table->backend, ==, entry->backend);
        }
        else if (choice.backend == TB_RHI_BACKEND_AUTOMATIC)
        {
            choice.backend = entry->backend;
        }
    }
    for (size_t backend = TB_RHI_BACKEND_D3D12;
         choice.backend == TB_RHI_BACKEND_AUTOMATIC && backend < TB_RHI_BACKEND_NULL; backend++)
    {
        if (!listed[backend])
        {
            choice.backend = (tb_rhi_backend)backend;
        }
    }
    TB_TEST_REQUIRE_UINT(choice.backend, !=, TB_RHI_BACKEND_AUTOMATIC);
    tb_fatal_set_handler(trap_fatal, NULL);
    TB_TEST_EXPECT_TRAP(create_named, &choice, "is not implemented on");
    tb_fatal_set_handler(NULL, NULL);
}
