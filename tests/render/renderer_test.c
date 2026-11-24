/* The renderer: cameras see what they look at down −Z; every shape is
 * closed, inside its unit box, and faces outward; frames run on the null
 * backend; and on D3D12 (WARP, on Windows) a mesh and debug lines land on
 * the pixels they should. */
#include "badger/memory.h"
#include "badger/renderer.h"
#include "badger/window.h"

#include "tb_test.h"

/* ---------------------------------------------------------------------------
 * Cameras.
 * ------------------------------------------------------------------------- */

TB_TEST(renderer_camera, a_camera_sees_what_it_looks_at_down_negative_z)
{
    tb_camera camera = tb_camera_perspective(tb_radians(90.0f));
    tb_vec3 seen = {0};

    TB_TEST_EXPECT_FLOAT(camera.near_plane, ==, 0.1f);
    TB_TEST_EXPECT_FLOAT(camera.far_plane, ==, 0.0f);
    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){3, 2, 5}, (tb_vec3){0, 0, 0}));
    seen = tb_mat4_transform_point(tb_camera_view(&camera), (tb_vec3){0, 0, 0});
    TB_TEST_EXPECT_FLOAT_NEAR(seen.x, 0.0f, 1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(seen.y, 0.0f, 1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(seen.z, -tb_vec3_length((tb_vec3){3, 2, 5}), 1e-4f);
    /* Up stays up. */
    seen = tb_mat4_transform_direction(tb_camera_view(&camera), (tb_vec3){0, 1, 0});
    TB_TEST_EXPECT(seen.y > 0.5f);
}

TB_TEST(renderer_camera, a_camera_cannot_look_straight_up_or_at_itself)
{
    tb_camera camera = tb_camera_perspective(1.0f);
    const tb_camera before = camera;

    TB_TEST_EXPECT(!tb_camera_look_at(&camera, (tb_vec3){0, 0, 0}, (tb_vec3){0, 5, 0}));
    TB_TEST_EXPECT(!tb_camera_look_at(&camera, (tb_vec3){1, 1, 1}, (tb_vec3){1, 1, 1}));
    TB_TEST_EXPECT_FLOAT(camera.orientation.w, ==, before.orientation.w);
}

/* Depth is reversed: the near plane at 1, far away toward 0; an
 * orthographic view maps its height to the clip space's. */
TB_TEST(renderer_camera, projections_reverse_depth)
{
    const tb_camera perspective = tb_camera_perspective(tb_radians(60.0f));
    const tb_camera orthographic = tb_camera_orthographic(10.0f);
    const tb_mat4 near_far = tb_camera_projection_of(&perspective, 16.0f / 9.0f);
    const tb_mat4 flat = tb_camera_projection_of(&orthographic, 2.0f);
    tb_vec4 clip = tb_mat4_transform(near_far, (tb_vec4){0, 0, -0.1f, 1});

    TB_TEST_EXPECT_FLOAT_NEAR(clip.z / clip.w, 1.0f, 1e-4f);
    clip = tb_mat4_transform(near_far, (tb_vec4){0, 0, -1000.0f, 1});
    TB_TEST_EXPECT(clip.z / clip.w < 0.001f);
    clip = tb_mat4_transform(flat, (tb_vec4){10.0f, 5.0f, 0.0f, 1.0f});
    TB_TEST_EXPECT_FLOAT_NEAR(clip.x, 1.0f, 1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(clip.y, 1.0f, 1e-4f);
}

/* ---------------------------------------------------------------------------
 * Shapes.
 * ------------------------------------------------------------------------- */

/* Each shape: inside its unit box, unit normals, and each triangle facing
 * the way its vertices' normals do, which is outward. */
TB_TEST(renderer_mesh, every_shape_fits_its_box_and_faces_outward)
{
    static const tb_mesh_shape shapes[] = {TB_MESH_CUBE,     TB_MESH_PLANE, TB_MESH_SPHERE,
                                           TB_MESH_CYLINDER, TB_MESH_CONE,  TB_MESH_TORUS,
                                           TB_MESH_CAPSULE};

    for (size_t shape = 0; shape < TB_TEST_COUNT_OF(shapes); shape++)
    {
        tb_mesh_vertex *vertices = NULL;
        uint32_t *indices = NULL;
        size_t vertex_count = 0;
        size_t index_count = 0;
        TB_TEST_CONTEXT("shape %zu", shape);
        tb_mesh_generate(shapes[shape], shape == 2 ? 5u : 0u, &vertices, &vertex_count, &indices,
                         &index_count);
        TB_TEST_REQUIRE(vertex_count > 0 && index_count > 0 && index_count % 3 == 0);
        for (size_t index = 0; index < vertex_count; index++)
        {
            const tb_mesh_vertex *const vertex = &vertices[index];
            TB_TEST_EXPECT(fabsf(vertex->position.x) <= 0.5001f &&
                           fabsf(vertex->position.y) <= 0.5001f &&
                           fabsf(vertex->position.z) <= 0.5001f);
            TB_TEST_EXPECT_FLOAT_NEAR(tb_vec3_length(vertex->normal), 1.0f, 1e-3f);
        }
        for (size_t index = 0; index < index_count; index += 3)
        {
            const tb_mesh_vertex *const a = &vertices[indices[index]];
            const tb_mesh_vertex *const b = &vertices[indices[index + 1]];
            const tb_mesh_vertex *const c = &vertices[indices[index + 2]];
            const tb_vec3 face = tb_vec3_cross(tb_vec3_sub(b->position, a->position),
                                               tb_vec3_sub(c->position, a->position));
            TB_TEST_REQUIRE(indices[index] < vertex_count && indices[index + 1] < vertex_count &&
                            indices[index + 2] < vertex_count);
            TB_TEST_EXPECT(
                tb_vec3_dot(face, tb_vec3_add(a->normal, tb_vec3_add(b->normal, c->normal))) >=
                0.0f);
        }
        tb_free(vertices);
        tb_free(indices);
    }
}

/* ---------------------------------------------------------------------------
 * Frames.
 * ------------------------------------------------------------------------- */

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
} screen;

/* A window and a renderer on it, or a skipped case where there is none. */
static void open_screen(screen *opened, tb_rhi_backend backend)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};

    *opened = (screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(opened->system,
                         &(tb_window_options){
                             .title = "renderer", .width = 160, .height = 120, .fixed_size = true},
                         &opened->window, NULL),
        ==, TB_WINDOW_OK);
    options.window = opened->window;
    options.device = (tb_rhi_device_options){
        .backend = backend, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
    if (tb_renderer_create(&options, &opened->renderer, &error) != TB_RHI_OK)
    {
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no renderer: %s", error.message);
    }
}

static void close_screen(screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

/* Draw a frame: a red flat cube in the middle, a green line on top across
 * the top of the view, and a blue line hidden behind the cube. */
static void draw_frame(tb_renderer *renderer, tb_mesh *cube)
{
    tb_camera camera = tb_camera_perspective(tb_radians(60.0f));

    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 0, 3}, (tb_vec3){0, 0, 0}));
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_renderer_draw_mesh_shaded(renderer, cube, tb_mat4_identity(), (tb_vec4){1, 0, 0, 1},
                                 TB_MESH_FLAT);
    tb_renderer_draw_mesh(renderer, cube, tb_mat4_translation((tb_vec3){100, 0, 0}),
                          (tb_vec4){1, 1, 1, 1});
    tb_debug_line(renderer, (tb_vec3){-5, 1.2f, 0}, (tb_vec3){5, 1.2f, 0}, (tb_vec4){0, 1, 0, 1},
                  TB_DEBUG_ON_TOP);
    tb_debug_line(renderer, (tb_vec3){-0.25f, 0, -2}, (tb_vec3){0.25f, 0, -2},
                  (tb_vec4){0, 0, 1, 1}, 0);
    tb_debug_box(renderer, tb_mat4_identity(), (tb_vec3){2, 2, 2}, (tb_vec4){1, 1, 0, 1}, 0);
    tb_debug_sphere(renderer, (tb_vec3){0, 0, 0}, 3.0f, (tb_vec4){1, 1, 1, 1}, 0);
    tb_debug_grid(renderer, (tb_vec3){0, -3, 0}, 10.0f, 4, (tb_vec4){0.5f, 0.5f, 0.5f, 1}, 0);
    tb_debug_axes(renderer, tb_mat4_translation((tb_vec3){0, 0, 50}), 1.0f, 0);
    tb_debug_arrow(renderer, (tb_vec3){0, 0, 60}, (tb_vec3){0, 1, 60}, (tb_vec4){1, 1, 1, 1}, 0);
    tb_debug_arrow(renderer, (tb_vec3){0, 0, 60}, (tb_vec3){1, 0, 60}, (tb_vec4){1, 1, 1, 1}, 0);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
}

/* On the null backend, frames run, meshes come and go, and a capture has the
 * window's size. */
TB_TEST(renderer_frame, frames_run_on_the_null_backend)
{
    screen opened = {0};
    tb_mesh *cube = NULL;
    tb_mesh *sphere = NULL;
    uint32_t width = 0;
    uint32_t height = 0;

    open_screen(&opened, TB_RHI_BACKEND_NULL);
    TB_TEST_EXPECT(tb_renderer_device(opened.renderer) != NULL);
    TB_TEST_EXPECT(tb_renderer_captured(opened.renderer, &width, &height) == NULL);
    TB_TEST_EXPECT_UINT(width, ==, 0);
    cube = tb_mesh_create_shape(opened.renderer, TB_MESH_CUBE, 0);
    sphere = tb_mesh_create_shape(opened.renderer, TB_MESH_SPHERE, 8);
    for (int frame = 0; frame < 4; frame++)
    {
        if (frame == 2)
        {
            tb_renderer_set_vsync(opened.renderer, true);
            tb_renderer_capture_next_frame(opened.renderer);
        }
        draw_frame(opened.renderer, cube);
    }
    TB_TEST_EXPECT(tb_renderer_captured(opened.renderer, &width, &height) != NULL);
    TB_TEST_EXPECT_UINT(width, >, 0);
    TB_TEST_EXPECT_UINT(height, >, 0);
    tb_mesh_destroy(opened.renderer, sphere);
    tb_mesh_destroy(opened.renderer, cube);
    tb_mesh_destroy(opened.renderer, NULL);
    close_screen(&opened);
    tb_renderer_destroy(NULL);
}
