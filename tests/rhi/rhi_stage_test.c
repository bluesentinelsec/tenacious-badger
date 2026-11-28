#include "rhi/rhi_internal.h"
#include "tb_test.h"

TB_TEST(rhi_stage, selected_resources_keep_their_generations_and_sparse_heap_indices)
{
    const tb_rhi_device_options options = {.backend = TB_RHI_BACKEND_NULL};
    tb_rhi_device *source = NULL, *staged = NULL;
    TB_TEST_REQUIRE_UINT(tb_rhi_device_create(&options, &source, NULL), ==, TB_RHI_OK);
    const tb_rhi_buffer_desc desc = {.size = 16,
                                     .usage = TB_RHI_BUFFER_SHADER_READ | TB_RHI_BUFFER_STORAGE};
    tb_rhi_buffer first = {0}, obsolete = {0}, kept = {0};
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(source, &desc, &first, NULL), ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(source, &desc, &obsolete, NULL), ==, TB_RHI_OK);
    tb_rhi_buffer_destroy(source, obsolete);
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(source, &desc, &kept, NULL), ==, TB_RHI_OK);
    TB_TEST_REQUIRE_UINT(kept.index, ==, obsolete.index);
    TB_TEST_REQUIRE_UINT(kept.generation, !=, obsolete.generation);
    tb_rhi_texture texture = {0};
    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(
            source,
            &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                   .width = 4,
                                   .height = 4,
                                   .mips = 3,
                                   .usage = TB_RHI_TEXTURE_SAMPLED | TB_RHI_TEXTURE_STORAGE},
            &texture, NULL),
        ==, TB_RHI_OK);
    tb_rhi_sampler sampler = {0};
    TB_TEST_REQUIRE_UINT(tb_rhi_sampler_create(source, &(tb_rhi_sampler_desc){0}, &sampler, NULL),
                         ==, TB_RHI_OK);
    const tb_rhi_buffer duplicates[] = {kept, kept};
    const tb_rhi_resource_set resources = {.buffers = duplicates,
                                           .buffer_count = 2,
                                           .textures = &texture,
                                           .texture_count = 1,
                                           .samplers = &sampler,
                                           .sampler_count = 1};
    tb_rhi_device_options small = options;
    small.bindless_resources = tb_rhi_texture_storage_mip_index(source, texture, 2);
    TB_TEST_EXPECT_UINT(tb_rhi_device_stage_resources(source, &small, &resources, &staged, NULL),
                        ==, TB_RHI_UNAVAILABLE);
    TB_TEST_EXPECT_POINTER(staged, ==, NULL);
    TB_TEST_REQUIRE_UINT(tb_rhi_device_stage_resources(source, &options, &resources, &staged, NULL),
                         ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(tb_rhi_buffer_index(staged, kept), ==, tb_rhi_buffer_index(source, kept));
    TB_TEST_EXPECT_UINT(tb_rhi_buffer_storage_index(staged, kept), ==,
                        tb_rhi_buffer_storage_index(source, kept));
    TB_TEST_EXPECT_UINT(tb_rhi_texture_index(staged, texture), ==,
                        tb_rhi_texture_index(source, texture));
    for (uint32_t mip = 0; mip < 3; mip++)
    {
        TB_TEST_EXPECT_UINT(tb_rhi_texture_storage_mip_index(staged, texture, mip), ==,
                            tb_rhi_texture_storage_mip_index(source, texture, mip));
    }
    TB_TEST_EXPECT_UINT(tb_rhi_sampler_index(staged, sampler), ==,
                        tb_rhi_sampler_index(source, sampler));
    tb_rhi_device_destroy(source);
    tb_rhi_buffer reused = {0};
    TB_TEST_REQUIRE_UINT(tb_rhi_buffer_create(staged, &desc, &reused, NULL), ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(reused.index, ==, first.index);
    TB_TEST_EXPECT_UINT(reused.generation, !=, first.generation);
    /* Only the first buffer's two indices were left out. Both remain available. */
    TB_TEST_EXPECT_UINT(tb_rhi_buffer_index(staged, reused), <, 2);
    TB_TEST_EXPECT_UINT(tb_rhi_buffer_storage_index(staged, reused), <, 2);
    tb_rhi_device_destroy(staged);
}
