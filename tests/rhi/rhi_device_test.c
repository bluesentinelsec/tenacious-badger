/* The RHI's device on a backend whose GPU lags: a destroyed object's handle
 * goes at once, but the backend's object and its bindless index stay until
 * the GPU has finished the work submitted before; and a lost device says so. */
#include "rhi/rhi_internal.h"

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

/* The renderer drops shader handles after pipeline creation. Rebuilding a
 * pipeline must still see the original description after those slots are reused. */
TB_TEST(rhi_device, pipelines_retain_their_inputs_after_shader_slot_reuse)
{
    tb_rhi_backend_table table = {0};
    tb_rhi_device *const device = open_lagging(&table);
    unsigned char code[] = {19, 23};
    char entry[] = "vs";
    char name[] = "geometry";
    char semantic[] = "POSITION";
    tb_rhi_shader_desc desc = {.stage = TB_RHI_STAGE_VERTEX,
                               .code = code,
                               .size = sizeof(code),
                               .entry = entry,
                               .name = name};
    tb_rhi_shader vertex = {0};
    tb_rhi_shader pixel = {0};
    tb_rhi_shader compute = {0};
    tb_rhi_pipeline material = {0};
    tb_rhi_pipeline depth = {0};
    tb_rhi_pipeline dispatch = {0};
    tb_rhi_vertex_attribute attr = {.semantic = semantic, .semantic_index = 2};
    tb_rhi_graphics_pipeline_desc graphics = {.name = name,
                                              .attributes = &attr,
                                              .attribute_count = 1,
                                              .buffer_count = 1,
                                              .buffers = {{.stride = 12}},
                                              .depth_write = true};

    TB_TEST_REQUIRE(tb_rhi_shader_create(device, &desc, &vertex, NULL) == TB_RHI_OK);
    desc.stage = TB_RHI_STAGE_PIXEL;
    TB_TEST_REQUIRE(tb_rhi_shader_create(device, &desc, &pixel, NULL) == TB_RHI_OK);
    graphics.vertex = vertex;
    graphics.pixel = pixel;
    TB_TEST_REQUIRE(tb_rhi_graphics_pipeline_create(device, &graphics, &material, NULL) ==
                    TB_RHI_OK);
    graphics.pixel = (tb_rhi_shader){0};
    TB_TEST_REQUIRE(tb_rhi_graphics_pipeline_create(device, &graphics, &depth, NULL) == TB_RHI_OK);
    tb_rhi_shader_destroy(device, pixel);
    tb_rhi_shader_destroy(device, vertex);

    code[0] = 47;
    entry[0] = 'c';
    name[0] = 'C';
    semantic[0] = '_';
    attr.semantic_index = 9;
    desc.stage = TB_RHI_STAGE_COMPUTE;
    TB_TEST_REQUIRE(tb_rhi_shader_create(device, &desc, &compute, NULL) == TB_RHI_OK);
    TB_TEST_EXPECT_UINT(compute.index, ==, vertex.index);
    const tb_rhi_compute_pipeline_desc kernel = {.compute = compute, .name = name};
    TB_TEST_REQUIRE(tb_rhi_compute_pipeline_create(device, &kernel, &dispatch, NULL) == TB_RHI_OK);
    tb_rhi_shader_destroy(device, compute);

    const tb_rhi_pipeline_data *const kept =
        tb_rhi_slot_of(device, TB_RHI_KIND_PIPELINE, material.index, material.generation)
            ->desc.pipeline;
    const tb_rhi_pipeline_data *const shadow =
        tb_rhi_slot_of(device, TB_RHI_KIND_PIPELINE, depth.index, depth.generation)->desc.pipeline;
    const tb_rhi_pipeline_data *const work =
        tb_rhi_slot_of(device, TB_RHI_KIND_PIPELINE, dispatch.index, dispatch.generation)
            ->desc.pipeline;
    TB_TEST_EXPECT_POINTER(shadow->first, ==, kept->first);
    TB_TEST_EXPECT_POINTER(shadow->pixel, ==, NULL);
    TB_TEST_EXPECT_UINT(kept->pixel->desc.stage, ==, TB_RHI_STAGE_PIXEL);
    TB_TEST_EXPECT_UINT(kept->graphics.buffers[0].stride, ==, 12);
    TB_TEST_EXPECT(kept->graphics.depth_write);
    TB_TEST_EXPECT_STRING(kept->graphics.name, ==, "geometry");
    TB_TEST_EXPECT_STRING(kept->graphics.attributes[0].semantic, ==, "POSITION");
    TB_TEST_EXPECT_UINT(kept->graphics.attributes[0].semantic_index, ==, 2);
    tb_rhi_pipeline_destroy(device, material);
    TB_TEST_EXPECT_STRING(shadow->first->desc.entry, ==, "vs");
    TB_TEST_EXPECT_STRING(shadow->first->desc.name, ==, "geometry");
    TB_TEST_EXPECT_UINT(((const unsigned char *)shadow->first->desc.code)[0], ==, 19);
    TB_TEST_EXPECT_UINT(((const unsigned char *)work->first->desc.code)[0], ==, 47);
    TB_TEST_EXPECT_STRING(work->compute.name, ==, "Ceometry");
    tb_rhi_device_destroy(device);
}
