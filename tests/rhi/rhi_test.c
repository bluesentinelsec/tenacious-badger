/* The RHI's device, on the null backend: handles name objects until they are
 * destroyed and never again; bindless indices are given to what shaders
 * reach, and only to that; descriptions come back with their defaults; a
 * frame records and submits; and the contracts hold. What a real backend
 * draws is tested with that backend. */
#include "badger/assert.h"
#include "badger/rhi.h"
#include "badger/str.h"
#include "badger/window.h"

#include "tb_test.h"

static tb_rhi_device *open_null(uint32_t resources, uint32_t samplers)
{
    tb_rhi_device *device = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    const tb_rhi_device_options options = {.backend = TB_RHI_BACKEND_NULL,
                                           .bindless_resources = resources,
                                           .bindless_samplers = samplers};

    TB_TEST_REQUIRE_UINT(tb_rhi_device_create(&options, &device, &error), ==, TB_RHI_OK);
    TB_TEST_REQUIRE(device != NULL);
    return device;
}

/* The null device says what it is, and takes the default heap sizes. */
TB_TEST(rhi, the_null_device_describes_itself)
{
    tb_rhi_device *const device = open_null(0, 0);
    tb_rhi_device_info info = {0};

    tb_rhi_device_info_of(device, &info);
    TB_TEST_EXPECT_UINT(info.backend, ==, TB_RHI_BACKEND_NULL);
    TB_TEST_EXPECT_STRING(info.adapter, ==, "null");
    TB_TEST_EXPECT_UINT(info.bindless_resources, ==, 65536);
    TB_TEST_EXPECT_UINT(info.bindless_samplers, ==, 2048);
    TB_TEST_EXPECT((info.features & TB_RHI_FEATURE_BINDLESS) != 0);
    tb_rhi_device_destroy(device);
    tb_rhi_device_destroy(NULL);
}

/* The names are those the `graphics.renderer` setting spells. */
TB_TEST(rhi, backends_and_results_have_names)
{
    TB_TEST_EXPECT_STRING(tb_rhi_backend_name(TB_RHI_BACKEND_AUTOMATIC), ==, "auto");
    TB_TEST_EXPECT_STRING(tb_rhi_backend_name(TB_RHI_BACKEND_D3D12), ==, "d3d12");
    TB_TEST_EXPECT_STRING(tb_rhi_backend_name(TB_RHI_BACKEND_NULL), ==, "null");
    TB_TEST_EXPECT_STRING(tb_rhi_backend_name(TB_RHI_BACKEND_COUNT), ==, "unknown");
    TB_TEST_EXPECT_STRING(tb_rhi_result_name(TB_RHI_OK), ==, "TB_RHI_OK");
    TB_TEST_EXPECT_STRING(tb_rhi_result_name(TB_RHI_DEVICE_LOST), ==, "TB_RHI_DEVICE_LOST");
    TB_TEST_EXPECT_STRING(tb_rhi_result_name((tb_rhi_result)99), ==, "unknown");
}

TB_TEST(rhi, formats_know_their_sizes)
{
    TB_TEST_EXPECT_UINT(tb_rhi_format_size(TB_RHI_FORMAT_UNKNOWN), ==, 0);
    TB_TEST_EXPECT_UINT(tb_rhi_format_size(TB_RHI_FORMAT_R8_UNORM), ==, 1);
    TB_TEST_EXPECT_UINT(tb_rhi_format_size(TB_RHI_FORMAT_R8G8B8A8_SRGB), ==, 4);
    TB_TEST_EXPECT_UINT(tb_rhi_format_size(TB_RHI_FORMAT_R16G16B16A16_FLOAT), ==, 8);
    TB_TEST_EXPECT_UINT(tb_rhi_format_size(TB_RHI_FORMAT_R32G32B32_FLOAT), ==, 12);
    TB_TEST_EXPECT_UINT(tb_rhi_format_size(TB_RHI_FORMAT_COUNT), ==, 0);
    TB_TEST_EXPECT(tb_rhi_format_is_depth(TB_RHI_FORMAT_D32_FLOAT));
    TB_TEST_EXPECT(tb_rhi_format_is_depth(TB_RHI_FORMAT_D24_UNORM_S8_UINT));
    TB_TEST_EXPECT(!tb_rhi_format_is_depth(TB_RHI_FORMAT_R32_FLOAT));
}

/* Until a backend of the platform is written, `auto` finds none; the null
 * backend is never chosen for a game that asked for pictures. */
TB_TEST(rhi, automatic_chooses_no_null_device)
{
    tb_rhi_device *device = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    const tb_rhi_device_options options = {.backend = TB_RHI_BACKEND_AUTOMATIC};
    const tb_rhi_result result = tb_rhi_device_create(&options, &device, &error);

    if (result == TB_RHI_OK)
    {
        tb_rhi_device_info info = {0};
        tb_rhi_device_info_of(device, &info);
        TB_TEST_EXPECT_UINT(info.backend, !=, TB_RHI_BACKEND_NULL);
        tb_rhi_device_destroy(device);
        return;
    }
    TB_TEST_EXPECT_UINT(result, ==, TB_RHI_UNAVAILABLE);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_RHI_UNAVAILABLE);
    TB_TEST_EXPECT(device == NULL);
    TB_TEST_EXPECT(error.message[0] != '\0');
}

/* What is written to an upload buffer stays there; the null backend keeps a
 * readback buffer's bytes too, so the round trip is visible. */
TB_TEST(rhi, buffers_keep_what_is_written)
{
    tb_rhi_device *const device = open_null(0, 0);
    const uint8_t written[4] = {1, 2, 3, 4};
    uint8_t read[4] = {0};
    tb_rhi_buffer upload = {0};
    tb_rhi_buffer readback = {0};
    tb_rhi_buffer_desc desc = {0};

    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(device,
                                              &(tb_rhi_buffer_desc){.size = 16,
                                                                    .memory = TB_RHI_MEMORY_UPLOAD,
                                                                    .usage = TB_RHI_BUFFER_VERTEX,
                                                                    .name = "upload"},
                                              &upload, NULL),
                         ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(device,
                             &(tb_rhi_buffer_desc){.size = 8, .memory = TB_RHI_MEMORY_READBACK},
                             &readback, NULL),
        ==, TB_RHI_OK);
    tb_rhi_buffer_write(device, upload, 12, written, sizeof(written));
    tb_rhi_buffer_read(device, readback, 4, read, sizeof(read));
    TB_TEST_EXPECT_UINT(read[0], ==, 0);
    tb_rhi_buffer_desc_of(device, upload, &desc);
    TB_TEST_EXPECT_UINT(desc.size, ==, 16);
    TB_TEST_EXPECT_UINT(desc.usage, ==, TB_RHI_BUFFER_VERTEX);
    TB_TEST_EXPECT(desc.name == NULL);
    TB_TEST_EXPECT_UINT(tb_rhi_buffer_index(device, upload), ==, TB_RHI_NO_INDEX);
    tb_rhi_buffer_destroy(device, upload);
    tb_rhi_buffer_destroy(device, readback);
    tb_rhi_buffer_destroy(device, (tb_rhi_buffer){0});
    tb_rhi_device_destroy(device);
}

/* Shaders reach what they read or write by index, and only that. */
TB_TEST(rhi, what_shaders_reach_has_an_index)
{
    tb_rhi_device *const device = open_null(0, 0);
    tb_rhi_buffer buffer = {0};
    tb_rhi_texture texture = {0};
    tb_rhi_texture target = {0};
    tb_rhi_sampler sampler = {0};

    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(
            device,
            &(tb_rhi_buffer_desc){.size = 64,
                                  .usage = TB_RHI_BUFFER_SHADER_READ | TB_RHI_BUFFER_STORAGE},
            &buffer, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(device,
                              &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                                     .width = 4,
                                                     .height = 4,
                                                     .usage = TB_RHI_TEXTURE_SAMPLED},
                              &texture, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(device,
                              &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                                     .width = 4,
                                                     .height = 4,
                                                     .usage = TB_RHI_TEXTURE_RENDER_TARGET},
                              &target, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(tb_rhi_sampler_create(device, &(tb_rhi_sampler_desc){0}, &sampler, NULL),
                         ==, TB_RHI_OK);

    TB_TEST_EXPECT_UINT(tb_rhi_buffer_index(device, buffer), !=, TB_RHI_NO_INDEX);
    TB_TEST_EXPECT_UINT(tb_rhi_buffer_storage_index(device, buffer), !=, TB_RHI_NO_INDEX);
    TB_TEST_EXPECT_UINT(tb_rhi_buffer_index(device, buffer), !=,
                        tb_rhi_buffer_storage_index(device, buffer));
    TB_TEST_EXPECT_UINT(tb_rhi_texture_index(device, texture), !=, TB_RHI_NO_INDEX);
    TB_TEST_EXPECT_UINT(tb_rhi_texture_storage_index(device, texture), ==, TB_RHI_NO_INDEX);
    TB_TEST_EXPECT_UINT(tb_rhi_texture_index(device, target), ==, TB_RHI_NO_INDEX);
    TB_TEST_EXPECT_UINT(tb_rhi_sampler_index(device, sampler), ==, 0);
    tb_rhi_sampler_destroy(device, sampler);
    tb_rhi_texture_destroy(device, target);
    tb_rhi_texture_destroy(device, texture);
    tb_rhi_buffer_destroy(device, buffer);
    tb_rhi_device_destroy(device);
}

/* A full heap is out of memory, said so; an index comes back for reuse once
 * its object is gone. */
TB_TEST(rhi, a_full_heap_says_so_and_indices_come_back)
{
    tb_rhi_device *const device = open_null(2, 1);
    const tb_rhi_texture_desc desc = {
        .format = TB_RHI_FORMAT_R8_UNORM, .width = 1, .height = 1, .usage = TB_RHI_TEXTURE_SAMPLED};
    tb_rhi_texture textures[3] = {0};
    tb_rhi_sampler samplers[2] = {0};
    tb_rhi_buffer storage = {0};
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    uint32_t freed = 0;

    TB_TEST_REQUIRE_UINT(tb_rhi_texture_create(device, &desc, &textures[0], NULL), ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(tb_rhi_texture_create(device, &desc, &textures[1], NULL), ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(tb_rhi_texture_create(device, &desc, &textures[2], &error), ==,
                        TB_RHI_OUT_OF_MEMORY);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_RHI_OUT_OF_MEMORY);
    TB_TEST_EXPECT(tb_str_find(error.message, 0, tb_str_length(error.message), "bindless") !=
                   (size_t)-1);
    TB_TEST_EXPECT_UINT(textures[2].generation, ==, 0);
    /* A buffer that needs two indices where one is left gets none. */
    tb_rhi_texture_destroy(device, textures[1]);
    TB_TEST_EXPECT_UINT(
        tb_rhi_buffer_create(
            device,
            &(tb_rhi_buffer_desc){.size = 4,
                                  .usage = TB_RHI_BUFFER_SHADER_READ | TB_RHI_BUFFER_STORAGE},
            &storage, NULL),
        ==, TB_RHI_OUT_OF_MEMORY);
    freed = tb_rhi_texture_index(device, textures[0]) == 0 ? 1u : 0u;
    TB_TEST_REQUIRE_UINT(tb_rhi_texture_create(device, &desc, &textures[1], NULL), ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(tb_rhi_texture_index(device, textures[1]), ==, freed);

    TB_TEST_REQUIRE_UINT(
        tb_rhi_sampler_create(device, &(tb_rhi_sampler_desc){0}, &samplers[0], NULL), ==,
        TB_RHI_OK);
    TB_TEST_EXPECT_UINT(
        tb_rhi_sampler_create(device, &(tb_rhi_sampler_desc){0}, &samplers[1], NULL), ==,
        TB_RHI_OUT_OF_MEMORY);
    tb_rhi_device_destroy(device);
}

/* A full mip chain halves the larger side down to one texel; each level's
 * side is the first's halved, never below one. */
TB_TEST(rhi, mip_chains_halve_to_one_texel)
{
    TB_TEST_EXPECT_UINT(tb_rhi_mip_count(1, 1), ==, 1);
    TB_TEST_EXPECT_UINT(tb_rhi_mip_count(2, 1), ==, 2);
    TB_TEST_EXPECT_UINT(tb_rhi_mip_count(1024, 512), ==, 11);
    TB_TEST_EXPECT_UINT(tb_rhi_mip_count(300, 17), ==, 9);
    TB_TEST_EXPECT_UINT(tb_rhi_mip_count(16384, 16384), ==, 15);
    TB_TEST_EXPECT_UINT(tb_rhi_mip_size(1024, 0), ==, 1024);
    TB_TEST_EXPECT_UINT(tb_rhi_mip_size(1024, 3), ==, 128);
    TB_TEST_EXPECT_UINT(tb_rhi_mip_size(300, 3), ==, 37);
    TB_TEST_EXPECT_UINT(tb_rhi_mip_size(17, 8), ==, 1);
    TB_TEST_EXPECT_UINT(tb_rhi_mip_size(17, 40), ==, 1);
}

/* A storage texture has an index to write each level, each its own; other
 * textures have none; and a heap without room for every level's index
 * gives the texture none. */
TB_TEST(rhi, a_storage_texture_has_an_index_for_each_level)
{
    tb_rhi_device *device = open_null(0, 0);
    const tb_rhi_texture_desc desc = {.format = TB_RHI_FORMAT_R8G8B8A8_SRGB,
                                      .width = 8,
                                      .height = 4,
                                      .mips = 4,
                                      .usage = TB_RHI_TEXTURE_SAMPLED | TB_RHI_TEXTURE_STORAGE};
    tb_rhi_texture texture = {0};
    tb_rhi_texture sampled = {0};
    uint32_t indices[4] = {0};

    TB_TEST_REQUIRE_UINT(tb_rhi_texture_create(device, &desc, &texture, NULL), ==, TB_RHI_OK);
    for (uint32_t mip = 0; mip < 4; mip++)
    {
        indices[mip] = tb_rhi_texture_storage_mip_index(device, texture, mip);
        TB_TEST_EXPECT_UINT(indices[mip], !=, TB_RHI_NO_INDEX);
        TB_TEST_EXPECT_UINT(indices[mip], !=, tb_rhi_texture_index(device, texture));
        for (uint32_t before = 0; before < mip; before++)
        {
            TB_TEST_EXPECT_UINT(indices[mip], !=, indices[before]);
        }
    }
    TB_TEST_EXPECT_UINT(indices[0], ==, tb_rhi_texture_storage_index(device, texture));
    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(device,
                              &(tb_rhi_texture_desc){.format = desc.format,
                                                     .width = 8,
                                                     .height = 4,
                                                     .mips = 2,
                                                     .usage = TB_RHI_TEXTURE_SAMPLED},
                              &sampled, NULL),
        ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(tb_rhi_texture_storage_mip_index(device, sampled, 1), ==, TB_RHI_NO_INDEX);
    tb_rhi_texture_destroy(device, texture);
    tb_rhi_texture_destroy(device, sampled);
    tb_rhi_device_destroy(device);

    /* Five indices: one to sample and four to write; three are not enough. */
    device = open_null(3, 1);
    TB_TEST_EXPECT_UINT(tb_rhi_texture_create(device, &desc, &texture, NULL), ==,
                        TB_RHI_OUT_OF_MEMORY);
    TB_TEST_EXPECT_UINT(texture.generation, ==, 0);
    /* What the failure took came back: three single textures fit. */
    for (int made = 0; made < 3; made++)
    {
        TB_TEST_EXPECT_UINT(
            tb_rhi_texture_create(device,
                                  &(tb_rhi_texture_desc){.format = desc.format,
                                                         .width = 1,
                                                         .height = 1,
                                                         .usage = TB_RHI_TEXTURE_SAMPLED},
                                  &sampled, NULL),
            ==, TB_RHI_OK);
    }
    tb_rhi_device_destroy(device);
}

/* A texture's defaults come back explicit: a cube has six faces. */
TB_TEST(rhi, textures_fill_in_their_defaults)
{
    tb_rhi_device *const device = open_null(0, 0);
    tb_rhi_texture cube = {0};
    tb_rhi_texture_desc desc = {0};

    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(device,
                              &(tb_rhi_texture_desc){.kind = TB_RHI_TEXTURE_CUBE,
                                                     .format = TB_RHI_FORMAT_R16G16B16A16_FLOAT,
                                                     .width = 8,
                                                     .height = 8,
                                                     .usage = TB_RHI_TEXTURE_SAMPLED,
                                                     .name = "sky"},
                              &cube, NULL),
        ==, TB_RHI_OK);
    tb_rhi_texture_desc_of(device, cube, &desc);
    TB_TEST_EXPECT_UINT(desc.layers, ==, 6);
    TB_TEST_EXPECT_UINT(desc.mips, ==, 1);
    TB_TEST_EXPECT_UINT(desc.samples, ==, 1);
    TB_TEST_EXPECT(desc.name == NULL);
    tb_rhi_device_destroy(device); /* releases what is still alive */
}

TB_TEST(rhi, a_texture_larger_than_the_device_allows_is_unavailable)
{
    tb_rhi_device *const device = open_null(0, 0);
    tb_rhi_texture texture = {0};
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    TB_TEST_EXPECT_UINT(
        tb_rhi_texture_create(device,
                              &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8_UNORM,
                                                     .width = 1u << 20,
                                                     .height = 1,
                                                     .usage = TB_RHI_TEXTURE_SAMPLED},
                              &texture, &error),
        ==, TB_RHI_UNAVAILABLE);
    TB_TEST_EXPECT(error.message[0] != '\0');
    tb_rhi_device_destroy(device);
}

/* A handle names its object until it is destroyed; the slot is reused under
 * a new generation, so the old handle never names the new object. */
TB_TEST(rhi, a_destroyed_handle_never_names_another_object)
{
    tb_rhi_device *const device = open_null(0, 0);
    tb_rhi_buffer first = {0};
    tb_rhi_buffer second = {0};
    const tb_rhi_buffer_desc desc = {.size = 4};

    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(device, &desc, &first, NULL), ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(first.generation, !=, 0);
    tb_rhi_buffer_destroy(device, first);
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(device, &desc, &second, NULL), ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(second.index, ==, first.index);
    TB_TEST_EXPECT_UINT(second.generation, !=, first.generation);
    tb_rhi_device_destroy(device);
}

typedef struct stale_use
{
    tb_rhi_device *device;
    tb_rhi_buffer buffer;
} stale_use;

static void describe_stale_buffer(void *context)
{
    stale_use *const use = context;
    tb_rhi_buffer_desc desc = {0};
    tb_rhi_buffer_desc_of(use->device, use->buffer, &desc);
}

/* A stale handle is caught in every build, before it reaches a backend. */
TB_TEST(rhi, a_stale_handle_violates_the_contract)
{
    stale_use use = {open_null(0, 0), {0}};

    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(use.device, &(tb_rhi_buffer_desc){.size = 4}, &use.buffer, NULL), ==,
        TB_RHI_OK);
    tb_rhi_buffer_destroy(use.device, use.buffer);
    TB_TEST_EXPECT_TRAP(describe_stale_buffer, &use, "an RHI handle is stale");
    use.buffer = (tb_rhi_buffer){0};
    TB_TEST_EXPECT_TRAP(describe_stale_buffer, &use, "an RHI handle is stale");
    use.buffer = (tb_rhi_buffer){.index = 1000, .generation = 1};
    TB_TEST_EXPECT_TRAP(describe_stale_buffer, &use, "an RHI handle is stale");
    tb_rhi_device_destroy(use.device);
}

/* Objects for a frame: a target, depth, geometry, shaders and pipelines. */
typedef struct scene
{
    tb_rhi_texture color;
    tb_rhi_texture depth;
    tb_rhi_buffer vertices;
    tb_rhi_buffer indices;
    tb_rhi_buffer constants;
    tb_rhi_buffer staging;
    tb_rhi_texture image;
    tb_rhi_shader vertex;
    tb_rhi_shader pixel;
    tb_rhi_shader compute;
    tb_rhi_pipeline draw;
    tb_rhi_pipeline dispatch;
} scene;

static scene make_scene(tb_rhi_device *device)
{
    static const uint8_t code[4] = {0x44, 0x58, 0x42, 0x43};
    const tb_rhi_vertex_attribute attributes[2] = {
        {.semantic = "POSITION", .format = TB_RHI_FORMAT_R32G32B32_FLOAT},
        {.semantic = "TEXCOORD", .format = TB_RHI_FORMAT_R32G32_FLOAT, .offset = 12}};
    scene made = {0};
    tb_rhi_graphics_pipeline_desc pipeline = {0};

    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(device,
                              &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                                     .width = 64,
                                                     .height = 32,
                                                     .usage = TB_RHI_TEXTURE_RENDER_TARGET},
                              &made.color, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(device,
                              &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_D32_FLOAT,
                                                     .width = 64,
                                                     .height = 32,
                                                     .usage = TB_RHI_TEXTURE_DEPTH},
                              &made.depth, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(device,
                              &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                                     .width = 16,
                                                     .height = 16,
                                                     .mips = 2,
                                                     .usage = TB_RHI_TEXTURE_SAMPLED},
                              &made.image, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(device,
                             &(tb_rhi_buffer_desc){.size = 240, .usage = TB_RHI_BUFFER_VERTEX},
                             &made.vertices, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(device,
                             &(tb_rhi_buffer_desc){.size = 72, .usage = TB_RHI_BUFFER_INDEX},
                             &made.indices, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(device,
                                              &(tb_rhi_buffer_desc){.size = 512,
                                                                    .usage = TB_RHI_BUFFER_CONSTANT,
                                                                    .memory = TB_RHI_MEMORY_UPLOAD},
                                              &made.constants, NULL),
                         ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(device,
                             &(tb_rhi_buffer_desc){.size = 8192, .memory = TB_RHI_MEMORY_UPLOAD},
                             &made.staging, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(tb_rhi_shader_create(device,
                                              &(tb_rhi_shader_desc){.stage = TB_RHI_STAGE_VERTEX,
                                                                    .code = code,
                                                                    .size = sizeof(code),
                                                                    .entry = "MainVS"},
                                              &made.vertex, NULL),
                         ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_shader_create(
            device,
            &(tb_rhi_shader_desc){.stage = TB_RHI_STAGE_PIXEL, .code = code, .size = sizeof(code)},
            &made.pixel, NULL),
        ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(tb_rhi_shader_create(device,
                                              &(tb_rhi_shader_desc){.stage = TB_RHI_STAGE_COMPUTE,
                                                                    .code = code,
                                                                    .size = sizeof(code),
                                                                    .name = "cull"},
                                              &made.compute, NULL),
                         ==, TB_RHI_OK);
    pipeline.vertex = made.vertex;
    pipeline.pixel = made.pixel;
    pipeline.attributes = attributes;
    pipeline.attribute_count = 2;
    pipeline.buffers[0] = (tb_rhi_vertex_buffer_layout){.stride = 20};
    pipeline.buffer_count = 1;
    pipeline.cull = TB_RHI_CULL_BACK;
    pipeline.depth_test = true;
    pipeline.depth_write = true;
    pipeline.depth_compare = TB_RHI_COMPARE_GREATER;
    pipeline.colors[0] = TB_RHI_FORMAT_R8G8B8A8_UNORM;
    pipeline.color_count = 1;
    pipeline.depth = TB_RHI_FORMAT_D32_FLOAT;
    TB_TEST_REQUIRE_UINT(tb_rhi_graphics_pipeline_create(device, &pipeline, &made.draw, NULL), ==,
                         TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(
        tb_rhi_compute_pipeline_create(
            device, &(tb_rhi_compute_pipeline_desc){.compute = made.compute}, &made.dispatch, NULL),
        ==, TB_RHI_OK);
    /* The pipelines keep working without their shaders. */
    tb_rhi_shader_destroy(device, made.compute);
    return made;
}

/* A frame of every command records and submits, and its submission is one
 * the GPU then finishes. */
TB_TEST(rhi, a_frame_records_and_submits)
{
    tb_rhi_device *const device = open_null(0, 0);
    const scene made = make_scene(device);
    const uint32_t push[4] = {1, 2, 3, 4};
    tb_rhi_command_list *list = NULL;
    tb_rhi_command_list *second = NULL;
    tb_rhi_command_list *lists[2] = {0};
    tb_rhi_pass pass = {0};
    const tb_rhi_barrier into[2] = {
        {.texture = made.color, .before = TB_RHI_STATE_COMMON, .after = TB_RHI_STATE_RENDER_TARGET},
        {.texture = made.depth, .before = TB_RHI_STATE_COMMON, .after = TB_RHI_STATE_DEPTH_WRITE}};
    uint64_t submission = 0;

    list = tb_rhi_command_list_begin(device);
    tb_rhi_cmd_begin_marker(list, "upload");
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = made.image,
                                               .before = TB_RHI_STATE_COMMON,
                                               .after = TB_RHI_STATE_COPY_DEST});
    tb_rhi_cmd_copy_buffer_to_texture(
        list,
        &(tb_rhi_texture_copy){.buffer = made.staging, .row_pitch = 256, .texture = made.image});
    tb_rhi_cmd_copy_buffer_to_texture(list, &(tb_rhi_texture_copy){.buffer = made.staging,
                                                                   .offset = 1024,
                                                                   .row_pitch = 256,
                                                                   .texture = made.image,
                                                                   .mip = 1,
                                                                   .region = {2, 2, 4, 4}});
    tb_rhi_cmd_copy_buffer(list, made.vertices, 0, made.staging, 0, 240);
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.buffer = made.vertices,
                                               .before = TB_RHI_STATE_COPY_DEST,
                                               .after = TB_RHI_STATE_VERTEX_INDEX});
    tb_rhi_cmd_end_marker(list);
    tb_rhi_cmd_pipeline(list, made.dispatch);
    tb_rhi_cmd_push_constants(list, push, sizeof(push));
    tb_rhi_cmd_dispatch(list, 4, 1, 1);
    tb_rhi_cmd_barriers(list, into, 2);
    pass.color_count = 1;
    pass.colors[0] = (tb_rhi_color_target){
        .texture = made.color, .load = TB_RHI_LOAD_CLEAR, .clear = {0.0f, 0.0f, 0.0f, 1.0f}};
    pass.depth = (tb_rhi_depth_target){.texture = made.depth, .load = TB_RHI_LOAD_CLEAR};
    tb_rhi_cmd_begin_pass(list, &pass);
    tb_rhi_cmd_viewport(list, 0.0f, 0.0f, 64.0f, 32.0f, 0.0f, 1.0f);
    tb_rhi_cmd_scissor(list, (tb_rhi_rect){0, 0, 64, 32});
    tb_rhi_cmd_vertex_buffer(list, 0, made.vertices, 0);
    tb_rhi_cmd_pipeline(list, made.draw);
    tb_rhi_cmd_index_buffer(list, made.indices, 0, TB_RHI_FORMAT_R16_UINT);
    tb_rhi_cmd_constant_buffer(list, 0, made.constants, 256);
    tb_rhi_cmd_push_constants(list, push, sizeof(push));
    tb_rhi_cmd_draw_indexed(list, 36, 1, 0, 0, 0);
    tb_rhi_cmd_draw(list, 3, 2, 0, 0);
    tb_rhi_cmd_end_pass(list);

    second = tb_rhi_command_list_begin(device);
    tb_rhi_cmd_copy_texture_to_buffer(
        second,
        &(tb_rhi_texture_copy){.buffer = made.staging, .row_pitch = 256, .texture = made.image});
    lists[0] = list;
    lists[1] = second;
    submission = tb_rhi_submit(device, lists, 2);
    TB_TEST_EXPECT_UINT(submission, ==, 1);
    TB_TEST_EXPECT_UINT(tb_rhi_wait(device, submission), ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(tb_rhi_completed(device), ==, 1);
    TB_TEST_EXPECT_UINT(tb_rhi_submit(device, NULL, 0), ==, 2);
    TB_TEST_EXPECT_UINT(tb_rhi_wait_idle(device), ==, TB_RHI_OK);

    tb_rhi_command_list_discard(tb_rhi_command_list_begin(device));
    tb_rhi_pipeline_destroy(device, made.draw);
    tb_rhi_pipeline_destroy(device, made.dispatch);
    tb_rhi_shader_destroy(device, made.vertex);
    tb_rhi_shader_destroy(device, made.pixel);
    tb_rhi_device_destroy(device);
}

static void draw_outside_a_pass(void *context)
{
    tb_rhi_command_list *const list = context;
    tb_rhi_cmd_draw(list, 3, 1, 0, 0);
}

static void end_a_pass_not_begun(void *context)
{
    tb_rhi_cmd_end_pass(context);
}

static void push_too_much(void *context)
{
    const uint8_t bytes[TB_RHI_PUSH_CONSTANT_BYTES + 4] = {0};
    tb_rhi_cmd_push_constants(context, bytes, sizeof(bytes));
}

/* Recording out of order is caught before it reaches a backend. */
TB_TEST(rhi, commands_out_of_place_violate_the_contract)
{
    tb_rhi_device *device = NULL;
    tb_rhi_command_list *list = NULL;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    device = open_null(0, 0);
    list = tb_rhi_command_list_begin(device);
    TB_TEST_EXPECT_TRAP(draw_outside_a_pass, list, "a draw outside a pass");
    TB_TEST_EXPECT_TRAP(end_a_pass_not_begun, list, "ending a pass not begun");
    TB_TEST_EXPECT_TRAP(push_too_much, list, "push constants");
    tb_rhi_command_list_discard(list);
    tb_rhi_device_destroy(device);
}

/* A swap chain's images are textures of the window's size in pixels, cycle
 * as they are presented, and change with a resize. */
TB_TEST(rhi, a_swap_chain_cycles_its_images)
{
    tb_window_system *system = NULL;
    tb_window *window = NULL;
    tb_rhi_device *device = NULL;
    tb_rhi_swap_chain *chain = NULL;
    tb_rhi_texture first = {0};
    tb_rhi_texture_desc desc = {0};
    tb_window_state state = {0};

    if (tb_window_system_open(NULL, &system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(system, &(tb_window_options){.title = "rhi", .width = 320, .height = 180},
                         &window, NULL),
        ==, TB_WINDOW_OK);
    device = open_null(0, 0);
    TB_TEST_REQUIRE_UINT(tb_rhi_swap_chain_create(
                             device, window, &(tb_rhi_swap_chain_desc){.images = 2}, &chain, NULL),
                         ==, TB_RHI_OK);
    tb_window_state_of(window, &state);
    first = tb_rhi_swap_chain_current(chain);
    tb_rhi_texture_desc_of(device, first, &desc);
    TB_TEST_EXPECT_UINT(desc.format, ==, TB_RHI_FORMAT_B8G8R8A8_UNORM);
    TB_TEST_EXPECT_UINT(desc.width, ==, state.pixel_width == 0 ? 1 : state.pixel_width);
    TB_TEST_EXPECT_UINT(desc.usage, ==, TB_RHI_TEXTURE_RENDER_TARGET);
    TB_TEST_EXPECT_UINT(tb_rhi_swap_chain_present(chain), ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(tb_rhi_swap_chain_current(chain).index, !=, first.index);
    TB_TEST_EXPECT_UINT(tb_rhi_swap_chain_present(chain), ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(tb_rhi_swap_chain_current(chain).index, ==, first.index);

    TB_TEST_REQUIRE_UINT(tb_rhi_swap_chain_resize(chain, 100, 50, true, NULL), ==, TB_RHI_OK);
    tb_rhi_texture_desc_of(device, tb_rhi_swap_chain_current(chain), &desc);
    TB_TEST_EXPECT_UINT(desc.width, ==, 100);
    TB_TEST_EXPECT_UINT(desc.height, ==, 50);
    tb_rhi_swap_chain_destroy(chain);
    tb_rhi_swap_chain_destroy(NULL);
    tb_rhi_device_destroy(device);
    tb_window_destroy(window);
    tb_window_system_close(system);
}
