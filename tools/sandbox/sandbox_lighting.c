/* The lighting scene: materials under moving lights, in the raster
 * lighting mode. Two rows of spheres from smooth to rough, one plastic and
 * one gold, and textured shapes, on a checkered floor, lit by a sun, three
 * point lights that circle them and a spot light that sweeps across them,
 * under a procedural sky or the plain ambient light; a window turns each on
 * and off. */
#include "sandbox.h"

#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/light.h"
#include "badger/material.h"
#include "badger/memory.h"
#include "badger/sky.h"
#include "badger/ui.h"

#include <math.h>

/* Spheres in a row, from smooth to rough. */
#define LIGHTING_SPHERES 5
#define LIGHTING_POINTS 3
/* The window of switches, in units from the frame's top left. */
#define LIGHTING_MARGIN 16.0f
#define LIGHTING_WINDOW_WIDTH 300.0f
#define LIGHTING_WINDOW_HEIGHT 330.0f
/* Texels a side of the floor's and the shapes' textures. */
#define LIGHTING_TEXTURE 128u

typedef struct lighting_scene
{
    tb_mesh *sphere;
    tb_mesh *plane;
    tb_mesh *torus;
    tb_mesh *cube;
    tb_texture *checker;
    tb_material *floor;
    tb_material *plastic[LIGHTING_SPHERES];
    tb_material *gold[LIGHTING_SPHERES];
    tb_material *tiles;
    tb_light_id sun;
    tb_light_id points[LIGHTING_POINTS];
    tb_light_id spot;
    tb_ui *ui;
    tb_ui_fonts fonts;
    tb_ui_input input;
    /* The switches. */
    bool sun_on;
    bool points_on;
    bool spot_on;
    bool moving;
    bool sky_on;
    float sun_intensity;
    float point_intensity;
    double clock; /* seconds the lights have moved */
} lighting_scene;

static const tb_vec3 lighting_point_colors[LIGHTING_POINTS] = {
    {1.0f, 0.35f, 0.25f}, {0.3f, 1.0f, 0.4f}, {0.3f, 0.5f, 1.0f}};

static tb_material *lighting_material(sandbox *box, tb_vec4 color, float roughness, float metallic)
{
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;

    desc.base_color_factor = color;
    desc.roughness = roughness;
    desc.metallic = metallic;
    return tb_material_create(box->renderer, &desc);
}

static void lighting_open_ui(lighting_scene *scene, sandbox *box)
{
    tb_ui_theme theme = tb_ui_theme_dark();

    if (!tb_ui_fonts_open(box->catalog, &scene->fonts))
    {
        (void)tb_fmt_print("badger-sandbox: lighting: this build of the engine carries no UI "
                           "font; the switches are not drawn\n");
        return;
    }
    tb_ui_theme_set_fonts(&theme, &scene->fonts);
    scene->ui = tb_ui_create(&(tb_ui_options){.theme = &theme});
}

bool sandbox_lighting_create(sandbox *box, void **state)
{
    lighting_scene *const scene = tb_calloc(1, sizeof(*scene));
    uint8_t *const pixels = tb_malloc((size_t)LIGHTING_TEXTURE * LIGHTING_TEXTURE * 4u);
    const size_t size = (size_t)LIGHTING_TEXTURE * LIGHTING_TEXTURE * 4u;
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;

    *state = scene;
    scene->sphere = tb_mesh_create_shape(box->renderer, TB_MESH_SPHERE, 48);
    scene->plane = tb_mesh_create_shape(box->renderer, TB_MESH_PLANE, 0);
    scene->torus = tb_mesh_create_shape(box->renderer, TB_MESH_TORUS, 48);
    scene->cube = tb_mesh_create_shape(box->renderer, TB_MESH_CUBE, 0);
    tb_image_fill_checkerboard(pixels, size, LIGHTING_TEXTURE, LIGHTING_TEXTURE, 16,
                               (tb_vec4){0.75f, 0.75f, 0.75f, 1}, (tb_vec4){0.3f, 0.3f, 0.32f, 1});
    scene->checker = tb_texture_create(box->renderer,
                                       &(tb_texture_desc){.width = LIGHTING_TEXTURE,
                                                          .height = LIGHTING_TEXTURE,
                                                          .name = "lighting checker"},
                                       pixels, size);
    tb_free(pixels);
    desc.base_color = scene->checker;
    desc.roughness = 0.85f;
    desc.uv_scale = (tb_vec2){8, 8};
    scene->floor = tb_material_create(box->renderer, &desc);
    desc.roughness = 0.35f;
    desc.uv_scale = (tb_vec2){2, 2};
    desc.base_color_factor = (tb_vec4){0.9f, 0.85f, 0.7f, 1};
    scene->tiles = tb_material_create(box->renderer, &desc);
    for (int index = 0; index < LIGHTING_SPHERES; index++)
    {
        const float roughness = 0.05f + 0.95f * (float)index / (float)(LIGHTING_SPHERES - 1);
        scene->plastic[index] =
            lighting_material(box, (tb_vec4){0.85f, 0.12f, 0.1f, 1}, roughness, 0.0f);
        scene->gold[index] =
            lighting_material(box, (tb_vec4){1.0f, 0.78f, 0.34f, 1}, roughness, 1.0f);
    }
    scene->sun_on = true;
    scene->points_on = true;
    scene->spot_on = true;
    scene->moving = true;
    scene->sky_on = true;
    scene->sun_intensity = 1.5f;
    scene->point_intensity = 3.0f;
    lighting_open_ui(scene, box);
    return true;
}

void sandbox_lighting_destroy(sandbox *box, void *state)
{
    lighting_scene *const scene = state;

    tb_renderer_remove_light(box->renderer, scene->sun);
    tb_renderer_remove_light(box->renderer, scene->spot);
    for (int index = 0; index < LIGHTING_POINTS; index++)
    {
        tb_renderer_remove_light(box->renderer, scene->points[index]);
    }
    tb_renderer_set_sky(box->renderer, NULL);
    tb_window_set_cursor(box->window, TB_CURSOR_ARROW);
    tb_ui_destroy(scene->ui);
    for (size_t font = 0; font < TB_UI_FONT_COUNT; font++)
    {
        if (scene->fonts.fonts[font] != NULL)
        {
            tb_canvas_forget_font(box->renderer, scene->fonts.fonts[font]);
        }
    }
    tb_ui_fonts_close(&scene->fonts);
    for (int index = 0; index < LIGHTING_SPHERES; index++)
    {
        tb_material_destroy(box->renderer, scene->plastic[index]);
        tb_material_destroy(box->renderer, scene->gold[index]);
    }
    tb_material_destroy(box->renderer, scene->tiles);
    tb_material_destroy(box->renderer, scene->floor);
    tb_texture_destroy(box->renderer, scene->checker);
    tb_mesh_destroy(box->renderer, scene->cube);
    tb_mesh_destroy(box->renderer, scene->torus);
    tb_mesh_destroy(box->renderer, scene->plane);
    tb_mesh_destroy(box->renderer, scene->sphere);
    tb_free(scene);
}

bool sandbox_lighting_event(sandbox *box, void *state, const tb_window_event *event)
{
    lighting_scene *const scene = state;

    (void)box;
    if (scene->ui == NULL)
    {
        return false;
    }
    tb_ui_input_add_event(&scene->input, event);
    if (event->type == TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN ||
        event->type == TB_WINDOW_EVENT_MOUSE_WHEEL)
    {
        return tb_ui_wants_mouse(scene->ui);
    }
    if (event->type == TB_WINDOW_EVENT_KEY_DOWN)
    {
        return tb_ui_wants_keyboard(scene->ui);
    }
    return false;
}

/* Add a light when its switch is on and it is not in the scene, change it
 * when both, and take it out when its switch is off. */
static void lighting_keep(sandbox *box, tb_light_id *id, bool on, const tb_light *light)
{
    if (!on)
    {
        tb_renderer_remove_light(box->renderer, *id);
        *id = (tb_light_id){0};
        return;
    }
    if (!tb_renderer_set_light(box->renderer, *id, light))
    {
        *id = tb_renderer_add_light(box->renderer, light);
    }
}

/* The lights where the clock puts them: the point lights circle the
 * spheres at their own heights, and the spot sweeps across them. */
static void lighting_place(sandbox *box, lighting_scene *scene)
{
    const tb_sky sky = TB_SKY_PROCEDURAL_INIT;
    const float t = (float)scene->clock;
    tb_light light = TB_LIGHT_INIT;
    tb_light spot = TB_LIGHT_INIT;

    light.kind = TB_LIGHT_DIRECTIONAL;
    light.direction = tb_vec3_negate(sky.sun_direction);
    light.color = sky.sun_color;
    light.intensity = scene->sun_intensity;
    lighting_keep(box, &scene->sun, scene->sun_on, &light);
    for (int index = 0; index < LIGHTING_POINTS; index++)
    {
        const float angle = t * 0.7f + (float)index * TB_PI * 2.0f / (float)LIGHTING_POINTS;
        light = (tb_light)TB_LIGHT_INIT;
        light.position =
            (tb_vec3){cosf(angle) * 4.0f, 0.6f + 0.5f * (float)index, sinf(angle) * 2.5f};
        light.color = lighting_point_colors[index];
        light.intensity = scene->point_intensity;
        light.range = 6.0f;
        lighting_keep(box, &scene->points[index], scene->points_on, &light);
        if (scene->points_on)
        {
            tb_debug_sphere(box->renderer, light.position, 0.08f,
                            (tb_vec4){light.color.x, light.color.y, light.color.z, 1}, 0);
        }
    }
    spot.kind = TB_LIGHT_SPOT;
    spot.position = (tb_vec3){0, 5.0f, 3.0f};
    spot.direction = tb_vec3_sub((tb_vec3){sinf(t * 0.5f) * 4.0f, 0, 0}, spot.position);
    spot.color = (tb_vec3){1.0f, 0.85f, 0.6f};
    spot.intensity = 40.0f;
    spot.range = 12.0f;
    spot.inner_angle = 0.15f;
    spot.outer_angle = 0.3f;
    lighting_keep(box, &scene->spot, scene->spot_on, &spot);
    if (scene->spot_on)
    {
        tb_debug_arrow(
            box->renderer, spot.position,
            tb_vec3_add(spot.position, tb_vec3_scale(tb_vec3_normalize(spot.direction), 1.0f)),
            (tb_vec4){1, 0.85f, 0.6f, 1}, 0);
    }
    tb_renderer_set_sky(box->renderer, scene->sky_on ? &sky : NULL);
}

static void lighting_window(lighting_scene *scene)
{
    tb_ui *const ui = scene->ui;

    if (!tb_ui_begin_window(ui, "Lighting",
                            (tb_canvas_rect){LIGHTING_MARGIN, LIGHTING_MARGIN,
                                             LIGHTING_WINDOW_WIDTH, LIGHTING_WINDOW_HEIGHT},
                            0))
    {
        return;
    }
    (void)tb_ui_checkbox(ui, "Sun", &scene->sun_on);
    (void)tb_ui_slider_float(ui, "Sun intensity", &scene->sun_intensity, 0.0f, 8.0f, "%.2f");
    (void)tb_ui_checkbox(ui, "Point lights", &scene->points_on);
    (void)tb_ui_slider_float(ui, "Point intensity", &scene->point_intensity, 0.0f, 20.0f, "%.1f");
    (void)tb_ui_checkbox(ui, "Spot light", &scene->spot_on);
    (void)tb_ui_checkbox(ui, "Move the lights", &scene->moving);
    (void)tb_ui_checkbox(ui, "Sky", &scene->sky_on);
    tb_ui_text_wrapped(ui, "Without the sky, the ambient light is the plain one. Anti-aliasing "
                           "and the render scale are graphics settings, in the inspector (F1).");
    tb_ui_end_window(ui);
}

void sandbox_lighting_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    lighting_scene *const scene = state;
    uint32_t width = 0;
    uint32_t height = 0;

    if (scene->moving)
    {
        scene->clock += frame->delta_seconds;
    }
    lighting_place(box, scene);
    tb_renderer_draw_mesh_material(box->renderer, scene->plane,
                                   tb_mat4_scaling((tb_vec3){24, 1, 24}), scene->floor);
    for (int index = 0; index < LIGHTING_SPHERES; index++)
    {
        const float x = ((float)index - (float)(LIGHTING_SPHERES - 1) * 0.5f) * 1.3f;
        tb_renderer_draw_mesh_material(box->renderer, scene->sphere,
                                       tb_mat4_translation((tb_vec3){x, 0.5f, 0.8f}),
                                       scene->plastic[index]);
        tb_renderer_draw_mesh_material(box->renderer, scene->sphere,
                                       tb_mat4_translation((tb_vec3){x, 0.5f, -0.8f}),
                                       scene->gold[index]);
    }
    tb_renderer_draw_mesh_material(
        box->renderer, scene->torus,
        tb_mat4_from_trs((tb_vec3){-4.2f, 0.6f, 0},
                         tb_quat_from_axis_angle((tb_vec3){1, 0, 0}, (float)frame->seconds * 0.5f),
                         (tb_vec3){1.2f, 1.2f, 1.2f}),
        scene->tiles);
    tb_renderer_draw_mesh_material(
        box->renderer, scene->cube,
        tb_mat4_from_trs((tb_vec3){4.2f, 0.5f, 0},
                         tb_quat_from_axis_angle((tb_vec3){0, 1, 0}, (float)frame->seconds * 0.3f),
                         (tb_vec3){1, 1, 1}),
        scene->tiles);
    if (scene->ui == NULL)
    {
        return;
    }
    tb_renderer_size(box->renderer, &width, &height);
    scene->input.scale = tb_canvas_view_logical(box->renderer).scale * box->ui_scale;
    scene->input.size = (tb_vec2){(float)width, (float)height};
    scene->input.delta_seconds = frame->delta_seconds;
    tb_ui_begin_frame(scene->ui, &scene->input);
    lighting_window(scene);
    tb_ui_end_frame(scene->ui);
    tb_ui_input_next_frame(&scene->input);
    tb_ui_draw(scene->ui, box->renderer);
    tb_window_set_cursor(box->window, tb_ui_cursor(scene->ui));
}
