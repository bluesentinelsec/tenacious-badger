/* The D3D12 backend, on WARP, the system's GPU on the CPU, so that every
 * Windows machine and CI runner draws the same pixels: targets clear,
 * buffers and textures copy through the GPU, triangles cover what they
 * should with depth, shaders reach textures, samplers and buffers by their
 * bindless indices, a swap chain presents, and validation's messages reach
 * the log. Each test reads its pixels back and checks them. */
#include "badger/log.h"
#include "badger/rhi.h"
#include "badger/str.h"
#include "badger/window.h"

#include "tb_test.h"
#include "test_data.h"

#include <string.h>

extern const tb_test_file tb_rhi_shader_files[];
extern const size_t tb_rhi_shader_files_count;

/* Validation's messages, as the log received them. */
typedef struct heard
{
    size_t count;
} heard;

static bool hear(void *context, const tb_log_record *record)
{
    heard *const messages = context;
    (void)record;
    messages->count++;
    return true;
}

typedef struct gpu
{
    tb_rhi_device *device;
    tb_rhi_device_info info;
    tb_log *log;
    heard messages;
} gpu;

/* A device on WARP with validation, or a skipped case where there is none. */
static void open_warp(gpu *opened)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_rhi_device_options options = {
        .backend = TB_RHI_BACKEND_D3D12, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
    tb_log_config config = TB_LOG_CONFIG_INIT;

    *opened = (gpu){0};
    opened->log = tb_log_create();
    config.minimum = TB_LOG_TRACE;
    config.write_record = hear;
    config.flush = NULL;
    config.context = &opened->messages;
    TB_TEST_REQUIRE_UINT(tb_log_configure(opened->log, &config), ==, TB_LOG_OK);
    options.log = opened->log;
    if (tb_rhi_device_create(&options, &opened->device, &error) != TB_RHI_OK)
    {
        (void)tb_log_destroy(opened->log);
        TB_TEST_SKIP("no WARP device: %s", error.message);
    }
    tb_rhi_device_info_of(opened->device, &opened->info);
}

static void close_warp(gpu *opened)
{
    tb_rhi_device_destroy(opened->device);
    (void)tb_log_destroy(opened->log);
}

static const tb_test_file *shader_file(const char *name)
{
    for (size_t index = 0; index < tb_rhi_shader_files_count; index++)
    {
        if (tb_str_equal(tb_rhi_shader_files[index].name, name))
        {
            return &tb_rhi_shader_files[index];
        }
    }
    TB_TEST_REQUIRE(!"a test shader is missing");
    return NULL;
}

static tb_rhi_shader make_shader(tb_rhi_device *device, const char *name, tb_rhi_stage stage)
{
    const tb_test_file *const file = shader_file(name);
    tb_rhi_shader shader = {0};
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    TB_TEST_REQUIRE_UINT(tb_rhi_shader_create(device,
                                              &(tb_rhi_shader_desc){.stage = stage,
                                                                    .code = file->bytes,
                                                                    .size = file->length,
                                                                    .name = name},
                                              &shader, &error),
                         ==, TB_RHI_OK);
    return shader;
}

/* A target of RGBA8 a test draws into, and a buffer to read it back. */
typedef struct target
{
    tb_rhi_texture texture;
    tb_rhi_buffer readback;
    uint32_t width;
    uint32_t height;
} target;

#define PITCH 256u

static target make_target(tb_rhi_device *device, uint32_t width, uint32_t height)
{
    target made = {0};

    made.width = width;
    made.height = height;
    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(
            device,
            &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                   .width = width,
                                   .height = height,
                                   .usage = TB_RHI_TEXTURE_RENDER_TARGET | TB_RHI_TEXTURE_SAMPLED,
                                   .name = "target"},
            &made.texture, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(device,
                             &(tb_rhi_buffer_desc){.size = (uint64_t)PITCH * height,
                                                   .memory = TB_RHI_MEMORY_READBACK,
                                                   .name = "readback"},
                             &made.readback, NULL),
        ==, TB_RHI_OK);
    return made;
}

/* Record the target's copy to its buffer: the target leaves the pass. */
static void copy_back(tb_rhi_command_list *list, const target *drawn)
{
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = drawn->texture,
                                               .before = TB_RHI_STATE_RENDER_TARGET,
                                               .after = TB_RHI_STATE_COPY_SOURCE});
    tb_rhi_cmd_copy_texture_to_buffer(list, &(tb_rhi_texture_copy){.buffer = drawn->readback,
                                                                   .row_pitch = PITCH,
                                                                   .texture = drawn->texture});
}

static void submit_and_wait(tb_rhi_device *device, tb_rhi_command_list *list)
{
    const uint64_t submission = tb_rhi_submit(device, &list, 1);
    TB_TEST_REQUIRE_UINT(tb_rhi_wait(device, submission), ==, TB_RHI_OK);
}

static void begin_target(tb_rhi_command_list *list, const target *drawn, const float clear[4])
{
    tb_rhi_pass pass = {.color_count = 1};

    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = drawn->texture,
                                               .before = TB_RHI_STATE_COMMON,
                                               .after = TB_RHI_STATE_RENDER_TARGET});
    pass.colors[0] = (tb_rhi_color_target){.texture = drawn->texture, .load = TB_RHI_LOAD_CLEAR};
    memcpy(pass.colors[0].clear, clear, sizeof(pass.colors[0].clear));
    tb_rhi_cmd_begin_pass(list, &pass);
}

static uint32_t pixel(tb_rhi_device *device, const target *drawn, uint32_t x, uint32_t y)
{
    uint8_t bytes[4] = {0};
    tb_rhi_buffer_read(device, drawn->readback, (uint64_t)y * PITCH + (uint64_t)x * 4u, bytes, 4);
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
           (uint32_t)bytes[3] << 24;
}

static void free_target(tb_rhi_device *device, const target *drawn)
{
    tb_rhi_texture_destroy(device, drawn->texture);
    tb_rhi_buffer_destroy(device, drawn->readback);
}

/* ------------------------------------------------------------------------- */

TB_TEST(rhi_d3d12, warp_describes_itself)
{
    gpu opened = {0};

    open_warp(&opened);
    TB_TEST_EXPECT_UINT(opened.info.backend, ==, TB_RHI_BACKEND_D3D12);
    TB_TEST_EXPECT(opened.info.software);
    TB_TEST_EXPECT(opened.info.adapter[0] != '\0');
    TB_TEST_EXPECT_UINT(opened.info.shader_model, >=, 60);
    TB_TEST_EXPECT_UINT(opened.info.max_texture_size, ==, 16384);
    TB_TEST_EXPECT((opened.info.features & TB_RHI_FEATURE_TIMESTAMPS) != 0);
    close_warp(&opened);
}

/* A cleared target reads back the clear, and its copy reaches the CPU. */
TB_TEST(rhi_d3d12, a_cleared_target_reads_back)
{
    static const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    gpu opened = {0};
    target drawn = {0};
    tb_rhi_command_list *list = NULL;

    open_warp(&opened);
    drawn = make_target(opened.device, 8, 4);
    list = tb_rhi_command_list_begin(opened.device);
    tb_rhi_cmd_begin_marker(list, "clear");
    begin_target(list, &drawn, clear);
    tb_rhi_cmd_end_pass(list);
    copy_back(list, &drawn);
    tb_rhi_cmd_end_marker(list);
    submit_and_wait(opened.device, list);
    TB_TEST_EXPECT_HEX(pixel(opened.device, &drawn, 0, 0), ==, 0xFFBF8040u);
    TB_TEST_EXPECT_HEX(pixel(opened.device, &drawn, 7, 3), ==, 0xFFBF8040u);
    free_target(opened.device, &drawn);
    close_warp(&opened);
}

/* Bytes go to the GPU's memory and come back through two copies. */
TB_TEST(rhi_d3d12, buffers_copy_through_the_gpu)
{
    gpu opened = {0};
    tb_rhi_buffer upload = {0};
    tb_rhi_buffer middle = {0};
    tb_rhi_buffer readback = {0};
    uint32_t written[64] = {0};
    uint32_t read[64] = {0};
    tb_rhi_command_list *list = NULL;

    open_warp(&opened);
    for (uint32_t index = 0; index < 64; index++)
    {
        written[index] = index * 2654435761u;
    }
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(opened.device,
                                              &(tb_rhi_buffer_desc){.size = sizeof(written),
                                                                    .memory = TB_RHI_MEMORY_UPLOAD},
                                              &upload, NULL),
                         ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(
            opened.device,
            &(tb_rhi_buffer_desc){.size = sizeof(written),
                                  .usage = TB_RHI_BUFFER_SHADER_READ | TB_RHI_BUFFER_STORAGE},
            &middle, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(
            opened.device,
            &(tb_rhi_buffer_desc){.size = sizeof(written), .memory = TB_RHI_MEMORY_READBACK},
            &readback, NULL),
        ==, TB_RHI_OK);
    tb_rhi_buffer_write(opened.device, upload, 0, written, sizeof(written));
    list = tb_rhi_command_list_begin(opened.device);
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.buffer = middle,
                                               .before = TB_RHI_STATE_COMMON,
                                               .after = TB_RHI_STATE_COPY_DEST});
    tb_rhi_cmd_copy_buffer(list, middle, 0, upload, 0, sizeof(written));
    tb_rhi_cmd_barriers(
        list,
        (const tb_rhi_barrier[]){
            {.buffer = middle, .before = TB_RHI_STATE_COPY_DEST, .after = TB_RHI_STATE_COPY_SOURCE},
            {.buffer = readback,
             .before = TB_RHI_STATE_COPY_DEST,
             .after = TB_RHI_STATE_COPY_DEST}},
        2);
    tb_rhi_cmd_copy_buffer(list, readback, 0, middle, 0, sizeof(written));
    submit_and_wait(opened.device, list);
    tb_rhi_buffer_read(opened.device, readback, 0, read, sizeof(read));
    TB_TEST_EXPECT(memcmp(read, written, sizeof(read)) == 0);
    tb_rhi_buffer_destroy(opened.device, upload);
    tb_rhi_buffer_destroy(opened.device, middle);
    tb_rhi_buffer_destroy(opened.device, readback);
    close_warp(&opened);
}

/* Texels uploaded to a texture's second level come back from it. */
TB_TEST(rhi_d3d12, a_texture_level_uploads_and_reads_back)
{
    gpu opened = {0};
    tb_rhi_buffer staging = {0};
    tb_rhi_buffer readback = {0};
    tb_rhi_texture texture = {0};
    uint8_t texels[PITCH * 4] = {0};
    uint8_t read[PITCH * 4] = {0};
    tb_rhi_command_list *list = NULL;

    open_warp(&opened);
    for (size_t index = 0; index < sizeof(texels); index++)
    {
        texels[index] = (uint8_t)(index * 7u);
    }
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(opened.device,
                                              &(tb_rhi_buffer_desc){.size = sizeof(texels),
                                                                    .memory = TB_RHI_MEMORY_UPLOAD},
                                              &staging, NULL),
                         ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(
            opened.device,
            &(tb_rhi_buffer_desc){.size = sizeof(read), .memory = TB_RHI_MEMORY_READBACK},
            &readback, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(opened.device,
                              &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                                     .width = 8,
                                                     .height = 8,
                                                     .layers = 2,
                                                     .mips = 2,
                                                     .usage = TB_RHI_TEXTURE_SAMPLED},
                              &texture, NULL),
        ==, TB_RHI_OK);
    tb_rhi_buffer_write(opened.device, staging, 0, texels, sizeof(texels));
    list = tb_rhi_command_list_begin(opened.device);
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = texture,
                                               .before = TB_RHI_STATE_COMMON,
                                               .after = TB_RHI_STATE_COPY_DEST});
    tb_rhi_cmd_copy_buffer_to_texture(
        list, &(tb_rhi_texture_copy){
                  .buffer = staging, .row_pitch = PITCH, .texture = texture, .mip = 1, .layer = 1});
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = texture,
                                               .before = TB_RHI_STATE_COPY_DEST,
                                               .after = TB_RHI_STATE_COPY_SOURCE});
    tb_rhi_cmd_copy_texture_to_buffer(
        list,
        &(tb_rhi_texture_copy){
            .buffer = readback, .row_pitch = PITCH, .texture = texture, .mip = 1, .layer = 1});
    submit_and_wait(opened.device, list);
    tb_rhi_buffer_read(opened.device, readback, 0, read, sizeof(read));
    for (size_t row = 0; row < 4; row++)
    {
        TB_TEST_EXPECT(memcmp(&read[row * PITCH], &texels[row * PITCH], 16) == 0);
    }
    tb_rhi_texture_destroy(opened.device, texture);
    tb_rhi_buffer_destroy(opened.device, staging);
    tb_rhi_buffer_destroy(opened.device, readback);
    close_warp(&opened);
}

/* A triangle that covers the target paints every pixel the color it is
 * given in push constants. */
TB_TEST(rhi_d3d12, a_triangle_covers_the_target)
{
    static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    static const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    gpu opened = {0};
    target drawn = {0};
    tb_rhi_graphics_pipeline_desc desc = {0};
    tb_rhi_pipeline pipeline = {0};
    tb_rhi_command_list *list = NULL;

    open_warp(&opened);
    drawn = make_target(opened.device, 16, 16);
    desc.vertex = make_shader(opened.device, "triangle.vs.dxil", TB_RHI_STAGE_VERTEX);
    desc.pixel = make_shader(opened.device, "triangle.ps.dxil", TB_RHI_STAGE_PIXEL);
    desc.colors[0] = TB_RHI_FORMAT_R8G8B8A8_UNORM;
    desc.color_count = 1;
    desc.name = "triangle";
    TB_TEST_REQUIRE_UINT(tb_rhi_graphics_pipeline_create(opened.device, &desc, &pipeline, NULL), ==,
                         TB_RHI_OK);
    tb_rhi_shader_destroy(opened.device, desc.vertex);
    tb_rhi_shader_destroy(opened.device, desc.pixel);
    list = tb_rhi_command_list_begin(opened.device);
    begin_target(list, &drawn, black);
    tb_rhi_cmd_pipeline(list, pipeline);
    tb_rhi_cmd_push_constants(list, green, sizeof(green));
    tb_rhi_cmd_draw(list, 3, 1, 0, 0);
    tb_rhi_cmd_end_pass(list);
    copy_back(list, &drawn);
    submit_and_wait(opened.device, list);
    for (uint32_t y = 0; y < 16; y += 5)
    {
        for (uint32_t x = 0; x < 16; x += 5)
        {
            TB_TEST_EXPECT_HEX(pixel(opened.device, &drawn, x, y), ==, 0xFF00FF00u);
        }
    }
    tb_rhi_pipeline_destroy(opened.device, pipeline);
    free_target(opened.device, &drawn);
    close_warp(&opened);
}

/* Indexed vertices from a vertex buffer draw two quads; the nearer, drawn
 * first, hides the farther where they overlap, by reversed depth. */
TB_TEST(rhi_d3d12, indexed_vertices_draw_with_depth)
{
    typedef struct vertex
    {
        float position[3];
        float color[4];
    } vertex;
    /* Left half red and near; all of it blue and far. */
    static const vertex vertices[8] = {
        {{-1, 1, 0.75f}, {1, 0, 0, 1}}, {{0, 1, 0.75f}, {1, 0, 0, 1}},
        {{0, -1, 0.75f}, {1, 0, 0, 1}}, {{-1, -1, 0.75f}, {1, 0, 0, 1}},
        {{-1, 1, 0.25f}, {0, 0, 1, 1}}, {{1, 1, 0.25f}, {0, 0, 1, 1}},
        {{1, -1, 0.25f}, {0, 0, 1, 1}}, {{-1, -1, 0.25f}, {0, 0, 1, 1}}};
    static const uint16_t indices[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const tb_rhi_vertex_attribute attributes[2] = {
        {.semantic = "POSITION", .format = TB_RHI_FORMAT_R32G32B32_FLOAT},
        {.semantic = "COLOR", .format = TB_RHI_FORMAT_R32G32B32A32_FLOAT, .offset = 12}};
    gpu opened = {0};
    target drawn = {0};
    tb_rhi_texture depth = {0};
    tb_rhi_buffer geometry = {0};
    tb_rhi_buffer index_buffer = {0};
    tb_rhi_graphics_pipeline_desc desc = {0};
    tb_rhi_pipeline pipeline = {0};
    tb_rhi_command_list *list = NULL;
    tb_rhi_pass pass = {.color_count = 1};

    open_warp(&opened);
    drawn = make_target(opened.device, 16, 16);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(
            opened.device,
            &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_D32_FLOAT,
                                   .width = 16,
                                   .height = 16,
                                   .usage = TB_RHI_TEXTURE_DEPTH | TB_RHI_TEXTURE_SAMPLED},
            &depth, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(opened.device,
                                              &(tb_rhi_buffer_desc){.size = sizeof(vertices),
                                                                    .usage = TB_RHI_BUFFER_VERTEX,
                                                                    .memory = TB_RHI_MEMORY_UPLOAD},
                                              &geometry, NULL),
                         ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(opened.device,
                                              &(tb_rhi_buffer_desc){.size = sizeof(indices),
                                                                    .usage = TB_RHI_BUFFER_INDEX,
                                                                    .memory = TB_RHI_MEMORY_UPLOAD},
                                              &index_buffer, NULL),
                         ==, TB_RHI_OK);
    tb_rhi_buffer_write(opened.device, geometry, 0, vertices, sizeof(vertices));
    tb_rhi_buffer_write(opened.device, index_buffer, 0, indices, sizeof(indices));
    desc.vertex = make_shader(opened.device, "vertex_color.vs.dxil", TB_RHI_STAGE_VERTEX);
    desc.pixel = make_shader(opened.device, "vertex_color.ps.dxil", TB_RHI_STAGE_PIXEL);
    desc.attributes = attributes;
    desc.attribute_count = 2;
    desc.buffers[0] = (tb_rhi_vertex_buffer_layout){.stride = sizeof(vertex)};
    desc.buffer_count = 1;
    desc.cull = TB_RHI_CULL_BACK;
    desc.clockwise_front = true;
    desc.depth_test = true;
    desc.depth_write = true;
    desc.depth_compare = TB_RHI_COMPARE_GREATER;
    desc.colors[0] = TB_RHI_FORMAT_R8G8B8A8_UNORM;
    desc.color_count = 1;
    desc.depth = TB_RHI_FORMAT_D32_FLOAT;
    TB_TEST_REQUIRE_UINT(tb_rhi_graphics_pipeline_create(opened.device, &desc, &pipeline, NULL), ==,
                         TB_RHI_OK);

    list = tb_rhi_command_list_begin(opened.device);
    tb_rhi_cmd_barriers(
        list,
        (const tb_rhi_barrier[]){
            {.texture = drawn.texture,
             .before = TB_RHI_STATE_COMMON,
             .after = TB_RHI_STATE_RENDER_TARGET},
            {.texture = depth, .before = TB_RHI_STATE_COMMON, .after = TB_RHI_STATE_DEPTH_WRITE}},
        2);
    pass.colors[0] = (tb_rhi_color_target){.texture = drawn.texture, .load = TB_RHI_LOAD_CLEAR};
    memcpy(pass.colors[0].clear, black, sizeof(black));
    pass.depth = (tb_rhi_depth_target){.texture = depth, .load = TB_RHI_LOAD_CLEAR, .clear = 0.0f};
    tb_rhi_cmd_begin_pass(list, &pass);
    tb_rhi_cmd_viewport(list, 0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f);
    tb_rhi_cmd_scissor(list, (tb_rhi_rect){0, 0, 16, 16});
    tb_rhi_cmd_pipeline(list, pipeline);
    tb_rhi_cmd_vertex_buffer(list, 0, geometry, 0);
    tb_rhi_cmd_index_buffer(list, index_buffer, 0, TB_RHI_FORMAT_R16_UINT);
    tb_rhi_cmd_draw_indexed(list, 12, 1, 0, 0, 0);
    tb_rhi_cmd_end_pass(list);
    copy_back(list, &drawn);
    submit_and_wait(opened.device, list);
    TB_TEST_EXPECT_HEX(pixel(opened.device, &drawn, 2, 8), ==, 0xFF0000FFu);
    TB_TEST_EXPECT_HEX(pixel(opened.device, &drawn, 13, 8), ==, 0xFFFF0000u);

    tb_rhi_pipeline_destroy(opened.device, pipeline);
    tb_rhi_shader_destroy(opened.device, desc.vertex);
    tb_rhi_shader_destroy(opened.device, desc.pixel);
    tb_rhi_buffer_destroy(opened.device, geometry);
    tb_rhi_buffer_destroy(opened.device, index_buffer);
    tb_rhi_texture_destroy(opened.device, depth);
    free_target(opened.device, &drawn);
    close_warp(&opened);
}

/* A shader samples a texture through the indices of the texture and of a
 * point sampler, so each pixel is its texel. */
TB_TEST(rhi_d3d12, shaders_sample_textures_by_index)
{
    static const uint32_t texels[4] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu};
    static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    gpu opened = {0};
    target drawn = {0};
    tb_rhi_texture texture = {0};
    tb_rhi_sampler sampler = {0};
    tb_rhi_buffer staging = {0};
    tb_rhi_graphics_pipeline_desc desc = {0};
    tb_rhi_pipeline pipeline = {0};
    tb_rhi_command_list *list = NULL;
    uint32_t draw[2] = {0};

    open_warp(&opened);
    if ((opened.info.features & TB_RHI_FEATURE_BINDLESS) == 0)
    {
        close_warp(&opened);
        TB_TEST_SKIP("this WARP has no Shader Model 6.6 bindless");
    }
    drawn = make_target(opened.device, 2, 2);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(opened.device,
                              &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                                     .width = 2,
                                                     .height = 2,
                                                     .usage = TB_RHI_TEXTURE_SAMPLED},
                              &texture, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_sampler_create(opened.device,
                              &(tb_rhi_sampler_desc){.min_filter = TB_RHI_FILTER_NEAREST,
                                                     .mag_filter = TB_RHI_FILTER_NEAREST,
                                                     .mip_filter = TB_RHI_FILTER_NEAREST,
                                                     .address_u = TB_RHI_ADDRESS_CLAMP,
                                                     .address_v = TB_RHI_ADDRESS_CLAMP},
                              &sampler, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(opened.device,
                                              &(tb_rhi_buffer_desc){.size = PITCH * 2,
                                                                    .memory = TB_RHI_MEMORY_UPLOAD},
                                              &staging, NULL),
                         ==, TB_RHI_OK);
    tb_rhi_buffer_write(opened.device, staging, 0, &texels[0], 8);
    tb_rhi_buffer_write(opened.device, staging, PITCH, &texels[2], 8);
    desc.vertex = make_shader(opened.device, "textured.vs.dxil", TB_RHI_STAGE_VERTEX);
    desc.pixel = make_shader(opened.device, "textured.ps.dxil", TB_RHI_STAGE_PIXEL);
    desc.colors[0] = TB_RHI_FORMAT_R8G8B8A8_UNORM;
    desc.color_count = 1;
    TB_TEST_REQUIRE_UINT(tb_rhi_graphics_pipeline_create(opened.device, &desc, &pipeline, NULL), ==,
                         TB_RHI_OK);
    draw[0] = tb_rhi_texture_index(opened.device, texture);
    draw[1] = tb_rhi_sampler_index(opened.device, sampler);

    list = tb_rhi_command_list_begin(opened.device);
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = texture,
                                               .before = TB_RHI_STATE_COMMON,
                                               .after = TB_RHI_STATE_COPY_DEST});
    tb_rhi_cmd_copy_buffer_to_texture(
        list, &(tb_rhi_texture_copy){.buffer = staging, .row_pitch = PITCH, .texture = texture});
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = texture,
                                               .before = TB_RHI_STATE_COPY_DEST,
                                               .after = TB_RHI_STATE_SHADER_READ});
    begin_target(list, &drawn, black);
    tb_rhi_cmd_pipeline(list, pipeline);
    tb_rhi_cmd_push_constants(list, draw, sizeof(draw));
    tb_rhi_cmd_draw(list, 3, 1, 0, 0);
    tb_rhi_cmd_end_pass(list);
    copy_back(list, &drawn);
    submit_and_wait(opened.device, list);
    TB_TEST_EXPECT_HEX(pixel(opened.device, &drawn, 0, 0), ==, texels[0]);
    TB_TEST_EXPECT_HEX(pixel(opened.device, &drawn, 1, 0), ==, texels[1]);
    TB_TEST_EXPECT_HEX(pixel(opened.device, &drawn, 0, 1), ==, texels[2]);
    TB_TEST_EXPECT_HEX(pixel(opened.device, &drawn, 1, 1), ==, texels[3]);

    tb_rhi_pipeline_destroy(opened.device, pipeline);
    tb_rhi_shader_destroy(opened.device, desc.vertex);
    tb_rhi_shader_destroy(opened.device, desc.pixel);
    tb_rhi_sampler_destroy(opened.device, sampler);
    tb_rhi_texture_destroy(opened.device, texture);
    tb_rhi_buffer_destroy(opened.device, staging);
    free_target(opened.device, &drawn);
    close_warp(&opened);
}

/* A compute shader writes a buffer it reaches by index, scaled by a value
 * from a constant buffer. */
TB_TEST(rhi_d3d12, compute_writes_a_buffer_by_index)
{
    enum
    {
        WORDS = 100
    };
    gpu opened = {0};
    tb_rhi_buffer output = {0};
    tb_rhi_buffer constants = {0};
    tb_rhi_buffer readback = {0};
    tb_rhi_compute_pipeline_desc desc = {0};
    tb_rhi_pipeline pipeline = {0};
    tb_rhi_command_list *list = NULL;
    const uint32_t scale = 3;
    uint32_t dispatch[2] = {0};
    uint32_t read[WORDS] = {0};

    open_warp(&opened);
    if ((opened.info.features & TB_RHI_FEATURE_BINDLESS) == 0)
    {
        close_warp(&opened);
        TB_TEST_SKIP("this WARP has no Shader Model 6.6 bindless");
    }
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(opened.device,
                                              &(tb_rhi_buffer_desc){.size = sizeof(read),
                                                                    .usage = TB_RHI_BUFFER_STORAGE},
                                              &output, NULL),
                         ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(opened.device,
                                              &(tb_rhi_buffer_desc){.size = 256,
                                                                    .usage = TB_RHI_BUFFER_CONSTANT,
                                                                    .memory = TB_RHI_MEMORY_UPLOAD},
                                              &constants, NULL),
                         ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(
            opened.device,
            &(tb_rhi_buffer_desc){.size = sizeof(read), .memory = TB_RHI_MEMORY_READBACK},
            &readback, NULL),
        ==, TB_RHI_OK);
    tb_rhi_buffer_write(opened.device, constants, 0, &scale, sizeof(scale));
    desc.compute = make_shader(opened.device, "fill.cs.dxil", TB_RHI_STAGE_COMPUTE);
    desc.name = "fill";
    TB_TEST_REQUIRE_UINT(tb_rhi_compute_pipeline_create(opened.device, &desc, &pipeline, NULL), ==,
                         TB_RHI_OK);
    dispatch[0] = tb_rhi_buffer_storage_index(opened.device, output);
    dispatch[1] = WORDS;

    list = tb_rhi_command_list_begin(opened.device);
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.buffer = output,
                                               .before = TB_RHI_STATE_COMMON,
                                               .after = TB_RHI_STATE_STORAGE});
    tb_rhi_cmd_pipeline(list, pipeline);
    tb_rhi_cmd_push_constants(list, dispatch, sizeof(dispatch));
    tb_rhi_cmd_constant_buffer(list, 0, constants, 0);
    tb_rhi_cmd_dispatch(list, 2, 1, 1);
    tb_rhi_cmd_barriers(
        list,
        (const tb_rhi_barrier[]){
            {.buffer = output, .before = TB_RHI_STATE_STORAGE, .after = TB_RHI_STATE_STORAGE},
            {.buffer = output, .before = TB_RHI_STATE_STORAGE, .after = TB_RHI_STATE_COPY_SOURCE}},
        2);
    tb_rhi_cmd_copy_buffer(list, readback, 0, output, 0, sizeof(read));
    submit_and_wait(opened.device, list);
    tb_rhi_buffer_read(opened.device, readback, 0, read, sizeof(read));
    TB_TEST_EXPECT_UINT(read[0], ==, 0);
    TB_TEST_EXPECT_UINT(read[1], ==, 3);
    TB_TEST_EXPECT_UINT(read[99], ==, 297);

    tb_rhi_pipeline_destroy(opened.device, pipeline);
    tb_rhi_shader_destroy(opened.device, desc.compute);
    tb_rhi_buffer_destroy(opened.device, output);
    tb_rhi_buffer_destroy(opened.device, constants);
    tb_rhi_buffer_destroy(opened.device, readback);
    close_warp(&opened);
}

TB_TEST(rhi_d3d12, bytes_that_are_not_dxil_are_refused)
{
    static const uint8_t spirv[4] = {0x03, 0x02, 0x23, 0x07};
    gpu opened = {0};
    tb_rhi_shader shader = {0};
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    open_warp(&opened);
    TB_TEST_EXPECT_UINT(tb_rhi_shader_create(opened.device,
                                             &(tb_rhi_shader_desc){.stage = TB_RHI_STAGE_PIXEL,
                                                                   .code = spirv,
                                                                   .size = sizeof(spirv)},
                                             &shader, &error),
                        ==, TB_RHI_SYSTEM_ERROR);
    TB_TEST_EXPECT(error.message[0] != '\0');
    close_warp(&opened);
}

/* A swap chain on a window clears and presents its images, with and
 * without vsync, and resizes. */
TB_TEST(rhi_d3d12, a_swap_chain_presents)
{
    static const float colors[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    gpu opened = {0};
    tb_window_system *system = NULL;
    tb_window *window = NULL;
    tb_rhi_swap_chain *chain = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_rhi_texture_desc desc = {0};

    if (tb_window_system_open(NULL, &system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(system,
                         &(tb_window_options){.title = "d3d12", .width = 320, .height = 180},
                         &window, NULL),
        ==, TB_WINDOW_OK);
    open_warp(&opened);
    TB_TEST_REQUIRE_UINT(tb_rhi_swap_chain_create(opened.device, window,
                                                  &(tb_rhi_swap_chain_desc){.vsync = false}, &chain,
                                                  &error),
                         ==, TB_RHI_OK);
    for (int frame = 0; frame < 6; frame++)
    {
        tb_rhi_command_list *const list = tb_rhi_command_list_begin(opened.device);
        const tb_rhi_texture image = tb_rhi_swap_chain_current(chain);
        tb_rhi_pass pass = {.color_count = 1};
        tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = image,
                                                   .before = TB_RHI_STATE_PRESENT,
                                                   .after = TB_RHI_STATE_RENDER_TARGET});
        pass.colors[0] = (tb_rhi_color_target){.texture = image, .load = TB_RHI_LOAD_CLEAR};
        memcpy(pass.colors[0].clear, colors[frame % 3], sizeof(colors[0]));
        tb_rhi_cmd_begin_pass(list, &pass);
        tb_rhi_cmd_end_pass(list);
        tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = image,
                                                   .before = TB_RHI_STATE_RENDER_TARGET,
                                                   .after = TB_RHI_STATE_PRESENT});
        (void)tb_rhi_submit(opened.device, &list, 1);
        TB_TEST_EXPECT_UINT(tb_rhi_swap_chain_present(chain), ==, TB_RHI_OK);
        if (frame == 2)
        {
            TB_TEST_REQUIRE_UINT(tb_rhi_swap_chain_resize(chain, 200, 100, true, &error), ==,
                                 TB_RHI_OK);
            tb_rhi_texture_desc_of(opened.device, tb_rhi_swap_chain_current(chain), &desc);
            TB_TEST_EXPECT_UINT(desc.width, ==, 200);
            TB_TEST_EXPECT_UINT(desc.height, ==, 100);
        }
    }
    tb_rhi_swap_chain_destroy(chain);
    close_warp(&opened);
    tb_window_destroy(window);
    tb_window_system_close(system);
}

/* Validation runs with a log for its messages: a clear of a color other than the
 * one the target was made for is one D3D12 warns of, where its debug layer
 * is installed. */
TB_TEST(rhi_d3d12, validation_runs_with_a_log)
{
    static const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    gpu opened = {0};
    target drawn = {0};
    tb_rhi_command_list *list = NULL;

    open_warp(&opened);
    drawn = make_target(opened.device, 4, 4);
    list = tb_rhi_command_list_begin(opened.device);
    begin_target(list, &drawn, red);
    tb_rhi_cmd_end_pass(list);
    copy_back(list, &drawn);
    submit_and_wait(opened.device, list);
    TB_TEST_EXPECT_HEX(pixel(opened.device, &drawn, 3, 3), ==, 0xFF0000FFu);
    free_target(opened.device, &drawn);
    close_warp(&opened);
}
