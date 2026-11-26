/* The renderer: cameras see what they look at down −Z; every shape is
 * closed, inside its unit box, and faces outward; frames run on the null
 * backend; and on D3D12 (WARP, on Windows) a mesh and debug lines land on
 * the pixels they should. */
#include "badger/memory.h"
#include "badger/render_graph.h"
#include "badger/renderer.h"
#include "badger/window.h"

#include "tb_test.h"

#include <math.h>
#include <stddef.h>

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

/* The aspects the axis constraints are checked at, against a 16:9
 * reference: the reference itself, wider (21:9, 32:9) and taller (16:10,
 * 4:3, square, portrait). */
static const float camera_aspects[] = {16.0f / 9.0f, 64.0f / 27.0f, 32.0f / 9.0f, 1.6f,
                                       4.0f / 3.0f,  1.0f,          9.0f / 16.0f};
#define CAMERA_ASPECTS (sizeof(camera_aspects) / sizeof(camera_aspects[0]))
#define CAMERA_REFERENCE (16.0f / 9.0f)

static tb_camera constrained(tb_camera camera, tb_camera_axis_constraint constraint)
{
    camera.axis_constraint = constraint;
    return camera;
}

/* The point at the top right corner of the view a unit ahead (or on the
 * plane, orthographic) lands on the top right corner of clip space: the
 * projection and the half extent agree. */
static void expect_corner_at_corner(const tb_camera *camera, float aspect, float reference)
{
    const tb_vec2 half = tb_camera_half_extent(camera, aspect, reference);
    const tb_mat4 projection = tb_camera_projection_fit(camera, aspect, reference);
    const float ahead = camera->projection == TB_CAMERA_PERSPECTIVE ? -1.0f : 0.0f;
    const tb_vec4 clip = tb_mat4_transform(projection, (tb_vec4){half.x, half.y, ahead, 1.0f});

    TB_TEST_EXPECT_FLOAT_NEAR(clip.x / clip.w, 1.0f, 1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(clip.y / clip.w, 1.0f, 1e-4f);
}

/* At the reference's own aspect, every constraint is the camera's own
 * view, exactly: a frame of the design's shape projects as it always did,
 * so the images tests compare do not change. */
TB_TEST(renderer_camera, at_the_reference_aspect_every_constraint_is_the_same_view)
{
    const tb_camera camera = tb_camera_perspective(tb_radians(60.0f));
    const tb_mat4 vertical = tb_camera_projection_of(&camera, CAMERA_REFERENCE);

    for (int constraint = 0; constraint < TB_CAMERA_AXIS_COUNT; constraint++)
    {
        TB_TEST_CONTEXT("%s", tb_camera_axis_constraint_name((tb_camera_axis_constraint)constraint));
        const tb_camera held = constrained(camera, (tb_camera_axis_constraint)constraint);
        const tb_mat4 fit = tb_camera_projection_fit(&held, CAMERA_REFERENCE, CAMERA_REFERENCE);
        for (int column = 0; column < 4; column++)
        {
            TB_TEST_EXPECT_FLOAT(fit.columns[column].x, ==, vertical.columns[column].x);
            TB_TEST_EXPECT_FLOAT(fit.columns[column].y, ==, vertical.columns[column].y);
            TB_TEST_EXPECT_FLOAT(fit.columns[column].z, ==, vertical.columns[column].z);
            TB_TEST_EXPECT_FLOAT(fit.columns[column].w, ==, vertical.columns[column].w);
        }
    }
}

/* `design` always shows the whole reference view: a wider view keeps the
 * vertical field of view and sees more across (Hor+), as `vertical` does;
 * a taller one keeps the reference's horizontal field of view and sees
 * more down (Vert+). Neither extent is ever less than the reference's. */
TB_TEST(renderer_camera, design_holds_the_whole_reference_view_at_every_aspect)
{
    const tb_camera camera = tb_camera_perspective(tb_radians(60.0f));
    const tb_camera design = constrained(camera, TB_CAMERA_AXIS_DESIGN);
    const tb_camera vertical = constrained(camera, TB_CAMERA_AXIS_VERTICAL);
    const tb_vec2 reference = tb_camera_half_extent(&vertical, CAMERA_REFERENCE, CAMERA_REFERENCE);

    for (size_t index = 0; index < CAMERA_ASPECTS; index++)
    {
        const float aspect = camera_aspects[index];
        TB_TEST_CONTEXT("aspect %.3f", (double)aspect);
        const tb_vec2 held = tb_camera_half_extent(&design, aspect, CAMERA_REFERENCE);
        const tb_vec2 kept = tb_camera_half_extent(&vertical, aspect, CAMERA_REFERENCE);
        TB_TEST_EXPECT(held.x >= reference.x * (1.0f - 1e-5f));
        TB_TEST_EXPECT(held.y >= reference.y * (1.0f - 1e-5f));
        if (aspect >= CAMERA_REFERENCE)
        {
            TB_TEST_EXPECT_FLOAT_NEAR(held.x, kept.x, 1e-5f);
            TB_TEST_EXPECT_FLOAT_NEAR(held.y, kept.y, 1e-5f);
        }
        else
        {
            TB_TEST_EXPECT_FLOAT_NEAR(held.x, reference.x, 1e-5f);
            TB_TEST_EXPECT(held.y > reference.y);
            /* `vertical` crops the sides instead (Hor-). */
            TB_TEST_EXPECT(kept.x < reference.x);
        }
        expect_corner_at_corner(&design, aspect, CAMERA_REFERENCE);
    }
}

/* `vertical` keeps the vertical extent at every aspect, today's behaviour
 * and Unreal's MaintainYFOV; `horizontal` keeps the reference's horizontal
 * extent, Unreal's MaintainXFOV. */
TB_TEST(renderer_camera, vertical_and_horizontal_each_hold_their_extent)
{
    const tb_camera camera = tb_camera_perspective(tb_radians(60.0f));
    const tb_camera vertical = constrained(camera, TB_CAMERA_AXIS_VERTICAL);
    const tb_camera horizontal = constrained(camera, TB_CAMERA_AXIS_HORIZONTAL);
    const tb_vec2 reference = tb_camera_half_extent(&vertical, CAMERA_REFERENCE, CAMERA_REFERENCE);

    for (size_t index = 0; index < CAMERA_ASPECTS; index++)
    {
        const float aspect = camera_aspects[index];
        TB_TEST_CONTEXT("aspect %.3f", (double)aspect);
        const tb_vec2 tall = tb_camera_half_extent(&vertical, aspect, CAMERA_REFERENCE);
        const tb_vec2 wide = tb_camera_half_extent(&horizontal, aspect, CAMERA_REFERENCE);
        TB_TEST_EXPECT_FLOAT_NEAR(tall.y, reference.y, 1e-6f);
        TB_TEST_EXPECT_FLOAT_NEAR(tall.x, reference.y * aspect, 1e-5f);
        TB_TEST_EXPECT_FLOAT_NEAR(wide.x, reference.x, 1e-5f);
        TB_TEST_EXPECT_FLOAT_NEAR(wide.y, reference.x / aspect, 1e-5f);
        expect_corner_at_corner(&vertical, aspect, CAMERA_REFERENCE);
        expect_corner_at_corner(&horizontal, aspect, CAMERA_REFERENCE);
    }
}

/* Without a reference aspect, every constraint keeps the vertical extent:
 * tb_camera_projection_of() is that. */
TB_TEST(renderer_camera, without_a_reference_every_constraint_keeps_the_vertical_extent)
{
    const tb_camera camera = tb_camera_perspective(tb_radians(60.0f));

    for (int constraint = 0; constraint < TB_CAMERA_AXIS_COUNT; constraint++)
    {
        const tb_camera held = constrained(camera, (tb_camera_axis_constraint)constraint);
        const tb_vec2 half = tb_camera_half_extent(&held, 1.6f, 0.0f);
        TB_TEST_EXPECT_FLOAT_NEAR(half.y, tanf(tb_radians(30.0f)), 1e-6f);
        const tb_mat4 of = tb_camera_projection_of(&held, 1.6f);
        const tb_mat4 fit = tb_camera_projection_fit(&held, 1.6f, 0.0f);
        TB_TEST_EXPECT_FLOAT(of.columns[1].y, ==, fit.columns[1].y);
        expect_corner_at_corner(&held, 1.6f, 0.0f);
    }
}

/* An orthographic camera follows its constraint the same way: in
 * `design`, its height is the least it shows, growing in a taller view to
 * keep the reference's width. */
TB_TEST(renderer_camera, an_orthographic_design_height_is_the_least_it_shows)
{
    const tb_camera camera = tb_camera_orthographic(10.0f);
    const tb_camera horizontal = constrained(camera, TB_CAMERA_AXIS_HORIZONTAL);

    TB_TEST_EXPECT_FLOAT_NEAR(tb_camera_half_extent(&camera, 21.0f / 9.0f, CAMERA_REFERENCE).y,
                              5.0f, 1e-6f);
    const tb_vec2 taller = tb_camera_half_extent(&camera, 4.0f / 3.0f, CAMERA_REFERENCE);
    TB_TEST_EXPECT_FLOAT_NEAR(taller.x, 5.0f * CAMERA_REFERENCE, 1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(taller.y, 5.0f * CAMERA_REFERENCE * 0.75f, 1e-4f);
    const tb_vec2 wider = tb_camera_half_extent(&horizontal, 32.0f / 9.0f, CAMERA_REFERENCE);
    TB_TEST_EXPECT_FLOAT_NEAR(wider.y, 2.5f, 1e-4f);
    expect_corner_at_corner(&camera, 4.0f / 3.0f, CAMERA_REFERENCE);
    expect_corner_at_corner(&horizontal, 32.0f / 9.0f, CAMERA_REFERENCE);
}

TB_TEST(renderer_camera, each_constraint_has_a_name)
{
    TB_TEST_EXPECT_STRING(tb_camera_axis_constraint_name(TB_CAMERA_AXIS_DESIGN), ==, "design");
    TB_TEST_EXPECT_STRING(tb_camera_axis_constraint_name(TB_CAMERA_AXIS_VERTICAL), ==, "vertical");
    TB_TEST_EXPECT_STRING(tb_camera_axis_constraint_name(TB_CAMERA_AXIS_HORIZONTAL), ==,
                          "horizontal");
    TB_TEST_EXPECT_STRING(tb_camera_axis_constraint_name(TB_CAMERA_AXIS_COUNT), ==, "unknown");
    /* A camera made zeroed holds the design's view. */
    TB_TEST_EXPECT_INT(tb_camera_perspective(1.0f).axis_constraint, ==, TB_CAMERA_AXIS_DESIGN);
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

static void count_custom(const tb_renderer_draw_context *draw)
{
    int *const calls = draw->context;
    TB_TEST_EXPECT(draw->list != NULL && draw->device != NULL);
    TB_TEST_EXPECT_UINT(draw->width, >, 0);
    (*calls)++;
}

/* On the null backend, frames run, meshes come and go, custom draws are
 * recorded once each, and a capture has the window's size. */
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
    {
        int calls = 0;
        tb_rhi_format color = TB_RHI_FORMAT_UNKNOWN;
        tb_rhi_format depth = TB_RHI_FORMAT_UNKNOWN;
        tb_camera camera = tb_camera_orthographic(4.0f);
        TB_TEST_REQUIRE(tb_renderer_begin_frame(opened.renderer, &camera, (tb_vec4){0, 0, 0, 1}));
        tb_renderer_draw_custom(opened.renderer, count_custom, &calls);
        tb_renderer_draw_custom(opened.renderer, count_custom, &calls);
        TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
        TB_TEST_EXPECT_INT(calls, ==, 2);
        tb_renderer_target_formats(opened.renderer, &color, &depth);
        /* The null backend has bindless resources, so the raster lighting
         * mode, whose custom draws go into the scene's light, in HDR. */
        TB_TEST_EXPECT_UINT(color, ==, TB_RHI_FORMAT_R16G16B16A16_FLOAT);
        TB_TEST_EXPECT_UINT(depth, ==, TB_RHI_FORMAT_D32_FLOAT);
    }
    TB_TEST_EXPECT(tb_renderer_captured(opened.renderer, &width, &height) != NULL);
    /* The graph the frames went through describes the last of them. */
    TB_TEST_EXPECT_UINT(tb_render_graph_dump(tb_renderer_graph(opened.renderer), NULL, 0), >, 0);
    TB_TEST_EXPECT_UINT(width, >, 0);
    TB_TEST_EXPECT_UINT(height, >, 0);
    tb_mesh_destroy(opened.renderer, sphere);
    tb_mesh_destroy(opened.renderer, cube);
    tb_mesh_destroy(opened.renderer, NULL);
    close_screen(&opened);
    tb_renderer_destroy(NULL);
}
