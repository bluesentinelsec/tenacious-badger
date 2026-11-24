/* The RHI's device on a backend whose GPU lags: a destroyed object's handle
 * goes at once, but the backend's object and its bindless index stay until
 * the GPU has finished the work submitted before; and a lost device says so. */
#include "rhi/rhi_backend.h"

#include "tb_test.h"

/* A backend of the null backend's objects, whose GPU finishes only what a
 * test says it has. */
typedef struct lagging
{
    uint64_t completed;
    size_t buffers_released;
    bool lose;
} lagging;

static lagging gpu;

static void lagging_buffer_destroy(void *state, void *object)
{
    gpu.buffers_released++;
    tb_rhi_null_backend.buffer_destroy(state, object);
}

static tb_rhi_result lagging_submit(void *state, void *const *lists, size_t count,
                                    uint64_t submission)
{
    (void)submission;
    (void)tb_rhi_null_backend.submit(state, lists, count, 0);
    return gpu.lose ? TB_RHI_DEVICE_LOST : TB_RHI_OK;
}

static uint64_t lagging_completed(void *state)
{
    (void)state;
    return gpu.completed;
}

static tb_rhi_device *open_lagging(tb_rhi_backend_table *table)
{
    tb_rhi_device *device = NULL;
    const tb_rhi_device_options options = {.bindless_resources = 1};

    gpu = (lagging){0};
    *table = tb_rhi_null_backend;
    table->buffer_destroy = lagging_buffer_destroy;
    table->submit = lagging_submit;
    table->completed = lagging_completed;
    TB_TEST_REQUIRE_UINT(tb_rhi_device_create_on(table, &options, &device, NULL), ==, TB_RHI_OK);
    return device;
}

TB_TEST(rhi_device, an_object_outlives_its_handle_until_the_gpu_is_done)
{
    tb_rhi_backend_table table = {0};
    tb_rhi_device *const device = open_lagging(&table);
    const tb_rhi_buffer_desc desc = {.size = 4, .usage = TB_RHI_BUFFER_SHADER_READ};
    tb_rhi_buffer buffer = {0};
    tb_rhi_buffer again = {0};
    uint64_t submission = 0;

    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(device, &desc, &buffer, NULL), ==, TB_RHI_OK);
    submission = tb_rhi_submit(device, NULL, 0);
    tb_rhi_buffer_destroy(device, buffer);
    TB_TEST_EXPECT_UINT(gpu.buffers_released, ==, 0);
    /* Its one index is not free while the GPU may still read it. */
    TB_TEST_EXPECT_UINT(tb_rhi_buffer_create(device, &desc, &again, NULL), ==,
                        TB_RHI_OUT_OF_MEMORY);

    gpu.completed = submission;
    TB_TEST_EXPECT_UINT(tb_rhi_completed(device), ==, submission);
    TB_TEST_EXPECT_UINT(gpu.buffers_released, ==, 1);
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(device, &desc, &again, NULL), ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(tb_rhi_buffer_index(device, again), ==, 0);
    tb_rhi_device_destroy(device);
    TB_TEST_EXPECT_UINT(gpu.buffers_released, ==, 2);
}

TB_TEST(rhi_device, a_lost_device_says_so_when_waited_on)
{
    tb_rhi_backend_table table = {0};
    tb_rhi_device *const device = open_lagging(&table);
    uint64_t submission = 0;

    gpu.lose = true;
    submission = tb_rhi_submit(device, NULL, 0);
    TB_TEST_EXPECT_UINT(tb_rhi_wait(device, submission), ==, TB_RHI_DEVICE_LOST);
    TB_TEST_EXPECT_UINT(tb_rhi_wait_idle(device), ==, TB_RHI_DEVICE_LOST);
    gpu.completed = submission;
    tb_rhi_device_destroy(device);
}
