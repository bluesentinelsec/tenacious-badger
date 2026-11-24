/* The render graph, on the null backend: passes whose work nothing uses are
 * culled, barriers move each resource into the state each pass declared,
 * transient textures share memory when their lifetimes do not overlap and
 * keep it from frame to frame, and imports end in the state asked of them. */
#include "badger/render_graph.h"
#include "badger/str.h"

#include "tb_test.h"

typedef struct ran
{
    char order[256];
} ran;

static void record(const tb_render_graph_pass_context *pass)
{
    ran *const log = pass->context;
    const size_t length = tb_str_length(log->order);
    TB_TEST_REQUIRE(pass->list != NULL);
    log->order[length] = (char)('a' + (char)(length % 26));
}

static tb_rhi_device *open_null(void)
{
    tb_rhi_device *device = NULL;
    TB_TEST_REQUIRE_UINT(
        tb_rhi_device_create(&(tb_rhi_device_options){.backend = TB_RHI_BACKEND_NULL}, &device,
                             NULL),
        ==, TB_RHI_OK);
    return device;
}

static tb_rhi_texture make_target(tb_rhi_device *device)
{
    tb_rhi_texture texture = {0};
    TB_TEST_REQUIRE_UINT(
        tb_rhi_texture_create(device,
                              &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                                     .width = 8,
                                                     .height = 8,
                                                     .usage = TB_RHI_TEXTURE_RENDER_TARGET},
                              &texture, NULL),
        ==, TB_RHI_OK);
    return texture;
}

static const tb_rhi_texture_desc color_desc = {
    .format = TB_RHI_FORMAT_R16G16B16A16_FLOAT, .width = 8, .height = 8};

static bool dumped(const tb_render_graph *graph, const char *line)
{
    char text[4096] = {0};
    (void)tb_render_graph_dump(graph, text, sizeof(text));
    TB_TEST_CONTEXT("%s", text);
    return tb_str_find(text, 0, tb_str_length(text), line) != TB_STR_NOT_FOUND;
}

/* A pass that writes what nothing reads is culled; one whose output a kept
 * pass samples is kept, and its target moves from target to shader read. */
TB_TEST(render_graph, unused_work_is_culled_and_barriers_follow_the_states)
{
    tb_rhi_device *const device = open_null();
    tb_render_graph *const graph = tb_render_graph_create(device);
    const tb_rhi_texture back = make_target(device);
    ran log = {0};
    tb_render_graph_texture scene = {0};
    tb_render_graph_texture unused = {0};
    tb_render_graph_texture output = {0};
    tb_render_graph_pass pass = {0};

    tb_render_graph_begin(graph);
    output = tb_render_graph_import_texture(graph, back, TB_RHI_STATE_PRESENT, TB_RHI_STATE_PRESENT,
                                            "back");
    scene = tb_render_graph_create_texture(graph, &color_desc, "scene");
    unused = tb_render_graph_create_texture(graph, &color_desc, "unused");

    pass = (tb_render_graph_pass){.name = "draw scene", .execute = record, .context = &log};
    pass.colors[0] = (tb_render_graph_color){.texture = scene, .load = TB_RHI_LOAD_CLEAR};
    pass.color_count = 1;
    tb_render_graph_add_pass(graph, &pass);

    pass = (tb_render_graph_pass){.name = "draw nothing used", .execute = record, .context = &log};
    pass.colors[0] = (tb_render_graph_color){.texture = unused, .load = TB_RHI_LOAD_CLEAR};
    pass.color_count = 1;
    tb_render_graph_add_pass(graph, &pass);

    pass = (tb_render_graph_pass){.name = "tonemap", .execute = record, .context = &log};
    pass.accesses[0] =
        (tb_render_graph_access){.texture = scene, .state = TB_RHI_STATE_SHADER_READ};
    pass.access_count = 1;
    pass.colors[0] = (tb_render_graph_color){.texture = output, .load = TB_RHI_LOAD_ANY};
    pass.color_count = 1;
    tb_render_graph_add_pass(graph, &pass);

    TB_TEST_EXPECT_UINT(tb_render_graph_execute(graph), ==, 1);
    TB_TEST_EXPECT_STRING(log.order, ==, "ab");
    TB_TEST_EXPECT(dumped(graph, "pass draw scene\n"));
    TB_TEST_EXPECT(dumped(graph, "culled draw nothing used\n"));
    TB_TEST_EXPECT(dumped(graph, "  barrier scene common -> render_target\n"));
    TB_TEST_EXPECT(dumped(graph, "  barrier scene render_target -> shader_read\n"));
    TB_TEST_EXPECT(dumped(graph, "  barrier back present -> render_target\n"));
    TB_TEST_EXPECT(dumped(graph, "final back render_target -> present\n"));
    TB_TEST_EXPECT(!dumped(graph, "unused common"));
    TB_TEST_EXPECT_UINT(tb_render_graph_pooled(graph), ==, 1);

    tb_render_graph_destroy(graph);
    tb_rhi_texture_destroy(device, back);
    tb_rhi_device_destroy(device);
}

/* Two transient textures of one description whose lifetimes do not overlap
 * share memory; the next frame takes the same memory again, in the state
 * the last left it in. */
TB_TEST(render_graph, transient_textures_share_memory_within_and_across_frames)
{
    tb_rhi_device *const device = open_null();
    tb_render_graph *const graph = tb_render_graph_create(device);
    const tb_rhi_texture back = make_target(device);
    ran log = {0};

    for (int frame = 0; frame < 2; frame++)
    {
        tb_render_graph_texture first = {0};
        tb_render_graph_texture second = {0};
        tb_render_graph_texture output = {0};
        tb_render_graph_pass pass = {0};

        tb_render_graph_begin(graph);
        output = tb_render_graph_import_texture(graph, back, TB_RHI_STATE_PRESENT,
                                                TB_RHI_STATE_PRESENT, "back");
        first = tb_render_graph_create_texture(graph, &color_desc, "first");
        second = tb_render_graph_create_texture(graph, &color_desc, "second");

        pass = (tb_render_graph_pass){.name = "make first", .execute = record, .context = &log};
        pass.colors[0] = (tb_render_graph_color){.texture = first, .load = TB_RHI_LOAD_CLEAR};
        pass.color_count = 1;
        tb_render_graph_add_pass(graph, &pass);

        pass = (tb_render_graph_pass){.name = "use first", .execute = record, .context = &log};
        pass.accesses[0] =
            (tb_render_graph_access){.texture = first, .state = TB_RHI_STATE_SHADER_READ};
        pass.access_count = 1;
        pass.colors[0] = (tb_render_graph_color){.texture = output, .load = TB_RHI_LOAD_CLEAR};
        pass.color_count = 1;
        tb_render_graph_add_pass(graph, &pass);

        pass = (tb_render_graph_pass){.name = "make second", .execute = record, .context = &log};
        pass.colors[0] = (tb_render_graph_color){.texture = second, .load = TB_RHI_LOAD_CLEAR};
        pass.color_count = 1;
        tb_render_graph_add_pass(graph, &pass);

        pass = (tb_render_graph_pass){.name = "use second", .execute = record, .context = &log};
        pass.accesses[0] =
            (tb_render_graph_access){.texture = second, .state = TB_RHI_STATE_SHADER_READ};
        pass.access_count = 1;
        pass.colors[0] = (tb_render_graph_color){.texture = output, .load = TB_RHI_LOAD_KEEP};
        pass.color_count = 1;
        tb_render_graph_add_pass(graph, &pass);

        (void)tb_render_graph_execute(graph);
        TB_TEST_EXPECT(dumped(graph, "  memory 0 for first\n"));
        TB_TEST_EXPECT(dumped(graph, "  memory 0 for second\n"));
        TB_TEST_EXPECT_UINT(tb_render_graph_pooled(graph), ==, 1);
    }
    /* The second frame found the memory where the first left it. */
    TB_TEST_EXPECT(dumped(graph, "  barrier first shader_read -> render_target\n"));

    /* Memory no frame uses for a while is released. */
    for (int frame = 0; frame < 5; frame++)
    {
        tb_render_graph_begin(graph);
        (void)tb_render_graph_execute(graph);
    }
    TB_TEST_EXPECT_UINT(tb_render_graph_pooled(graph), ==, 0);

    tb_render_graph_destroy(graph);
    tb_rhi_texture_destroy(device, back);
    tb_rhi_device_destroy(device);
}

typedef struct compute_use
{
    tb_render_graph_buffer buffer;
    tb_rhi_buffer seen;
} compute_use;

static void look_up_buffer(const tb_render_graph_pass_context *pass)
{
    compute_use *const use = pass->context;
    use->seen = tb_render_graph_rhi_buffer(pass->graph, use->buffer);
}

/* Compute passes writing one buffer one after the other have a barrier
 * between them; a pass that never culls is kept though nothing reads it;
 * and a pass's callback finds the RHI's buffer behind its handle. */
TB_TEST(render_graph, storage_writes_are_ordered_and_readbacks_are_kept)
{
    tb_rhi_device *const device = open_null();
    tb_render_graph *const graph = tb_render_graph_create(device);
    tb_rhi_buffer readback = {0};
    compute_use use = {0};
    tb_render_graph_buffer out = {0};
    tb_render_graph_pass pass = {0};

    TB_TEST_REQUIRE_UINT(
        tb_rhi_buffer_create(device,
                             &(tb_rhi_buffer_desc){.size = 64, .memory = TB_RHI_MEMORY_READBACK},
                             &readback, NULL),
        ==, TB_RHI_OK);
    tb_render_graph_begin(graph);
    use.buffer = tb_render_graph_create_buffer(
        graph, &(tb_rhi_buffer_desc){.size = 64, .usage = TB_RHI_BUFFER_STORAGE}, "particles");
    out = tb_render_graph_import_buffer(graph, readback, TB_RHI_STATE_COPY_DEST,
                                        TB_RHI_STATE_COPY_DEST, "readback");
    for (int step = 0; step < 2; step++)
    {
        pass = (tb_render_graph_pass){
            .name = step == 0 ? "spawn" : "simulate", .execute = look_up_buffer, .context = &use};
        pass.accesses[0] =
            (tb_render_graph_access){.buffer = use.buffer, .state = TB_RHI_STATE_STORAGE};
        pass.access_count = 1;
        tb_render_graph_add_pass(graph, &pass);
    }
    pass = (tb_render_graph_pass){
        .name = "read back", .execute = look_up_buffer, .context = &use, .never_cull = true};
    pass.accesses[0] =
        (tb_render_graph_access){.buffer = use.buffer, .state = TB_RHI_STATE_COPY_SOURCE};
    pass.accesses[1] = (tb_render_graph_access){.buffer = out, .state = TB_RHI_STATE_COPY_DEST};
    pass.access_count = 2;
    tb_render_graph_add_pass(graph, &pass);

    (void)tb_render_graph_execute(graph);
    TB_TEST_EXPECT(dumped(
        graph, "pass spawn\n  memory 0 for particles\n  barrier particles common -> storage\n"));
    TB_TEST_EXPECT(dumped(graph, "pass simulate\n  barrier particles storage -> storage\n"));
    TB_TEST_EXPECT(dumped(graph, "pass read back\n  barrier particles storage -> copy_source\n"));
    TB_TEST_EXPECT(!dumped(graph, "barrier readback"));
    TB_TEST_EXPECT_UINT(use.seen.generation, !=, 0);

    tb_render_graph_destroy(graph);
    tb_rhi_buffer_destroy(device, readback);
    tb_rhi_device_destroy(device);
}

/* Depth that a pass only tests is read, not written; what was there is kept. */
TB_TEST(render_graph, read_only_depth_keeps_its_producer)
{
    tb_rhi_device *const device = open_null();
    tb_render_graph *const graph = tb_render_graph_create(device);
    const tb_rhi_texture back = make_target(device);
    const tb_rhi_texture_desc depth_desc = {
        .format = TB_RHI_FORMAT_D32_FLOAT, .width = 8, .height = 8};
    ran log = {0};
    tb_render_graph_texture depth = {0};
    tb_render_graph_texture output = {0};
    tb_render_graph_pass pass = {0};
    char text[2048] = {0};

    tb_render_graph_begin(graph);
    output = tb_render_graph_import_texture(graph, back, TB_RHI_STATE_PRESENT, TB_RHI_STATE_PRESENT,
                                            "back");
    depth = tb_render_graph_create_texture(graph, &depth_desc, "depth");
    pass = (tb_render_graph_pass){.name = "prepass", .execute = record, .context = &log};
    pass.depth = (tb_render_graph_depth){.texture = depth, .load = TB_RHI_LOAD_CLEAR};
    tb_render_graph_add_pass(graph, &pass);
    pass = (tb_render_graph_pass){.name = "shade", .execute = record, .context = &log};
    pass.colors[0] = (tb_render_graph_color){.texture = output, .load = TB_RHI_LOAD_CLEAR};
    pass.color_count = 1;
    pass.depth = (tb_render_graph_depth){.texture = depth, .read_only = true};
    tb_render_graph_add_pass(graph, &pass);
    (void)tb_render_graph_execute(graph);
    TB_TEST_EXPECT_STRING(log.order, ==, "ab");
    TB_TEST_EXPECT(dumped(graph, "  barrier depth depth_write -> depth_read\n"));
    TB_TEST_EXPECT_UINT(tb_render_graph_dump(graph, NULL, 0), >, 0);
    TB_TEST_EXPECT_UINT(tb_render_graph_dump(graph, text, 8), ==,
                        tb_render_graph_dump(graph, NULL, 0));
    TB_TEST_EXPECT_UINT(tb_str_length(text), ==, 7);

    tb_render_graph_destroy(graph);
    tb_render_graph_destroy(NULL);
    tb_rhi_texture_destroy(device, back);
    tb_rhi_device_destroy(device);
}
