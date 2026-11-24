/* The sandbox's scenes: the renderer's shapes under the fly camera; and,
 * drawn through the RHI itself inside the renderer's scene pass, a turning
 * triangle and a texture reached by its bindless index; and a clear. */
#include "sandbox.h"

#include "badger/canvas.h"
#include "badger/material.h"

#include "badger/fmt.h"
#include "badger/math.h"
#include "badger/memory.h"
#include "badger/sky.h"

#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * clear: nothing but the clear color, to see the swap chain present.
 * ------------------------------------------------------------------------- */

static bool clear_create(sandbox *box, void **state)
{
    (void)box;
    *state = NULL;
    return true;
}

static void clear_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    (void)box;
    (void)state;
    (void)frame;
}

static void clear_destroy(sandbox *box, void *state)
{
    (void)box;
    (void)state;
}

/* ---------------------------------------------------------------------------
 * shapes: every shape the renderer generates, lit, turning, on a grid.
 * ------------------------------------------------------------------------- */

#define SHAPE_COUNT 7

typedef struct shapes_scene
{
    tb_mesh *meshes[SHAPE_COUNT];
    tb_mesh *floor;
} shapes_scene;

bool shapes_create(sandbox *box, void **state)
{
    shapes_scene *const scene = tb_calloc(1, sizeof(*scene));

    *state = scene;
    for (int shape = 0; shape < SHAPE_COUNT; shape++)
    {
        scene->meshes[shape] = tb_mesh_create_shape(box->renderer, (tb_mesh_shape)shape, 0);
    }
    scene->floor = tb_mesh_create_shape(box->renderer, TB_MESH_PLANE, 0);
    return true;
}

void shapes_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    static const tb_vec4 colors[SHAPE_COUNT] = {{0.90f, 0.35f, 0.30f, 1}, {0.85f, 0.85f, 0.85f, 1},
                                                {0.30f, 0.70f, 0.95f, 1}, {0.95f, 0.75f, 0.25f, 1},
                                                {0.55f, 0.85f, 0.40f, 1}, {0.80f, 0.45f, 0.90f, 1},
                                                {0.95f, 0.55f, 0.20f, 1}};
    const shapes_scene *const scene = state;
    const float turn = (float)frame->seconds * 0.6f;

    tb_renderer_draw_mesh(
        box->renderer, scene->floor,
        tb_mat4_from_trs((tb_vec3){0, -0.01f, 0}, tb_quat_identity(), (tb_vec3){24, 1, 24}),
        (tb_vec4){0.22f, 0.23f, 0.25f, 1});
    for (int shape = 0; shape < SHAPE_COUNT; shape++)
    {
        const float x = ((float)shape - (float)(SHAPE_COUNT - 1) * 0.5f) * 1.6f;
        const tb_quat spin = tb_quat_mul(tb_quat_from_axis_angle((tb_vec3){0, 1, 0}, turn),
                                         tb_quat_from_axis_angle((tb_vec3){1, 0, 0}, 0.4f));
        const tb_mat4 transform =
            tb_mat4_from_trs((tb_vec3){x, 0.75f, 0}, spin, (tb_vec3){1, 1, 1});
        tb_renderer_draw_mesh(box->renderer, scene->meshes[shape], transform, colors[shape]);
        tb_debug_box(box->renderer, transform, (tb_vec3){0.5f, 0.5f, 0.5f},
                     (tb_vec4){1, 1, 1, 0.25f}, 0);
    }
    tb_debug_grid(box->renderer, (tb_vec3){0, 0, 0}, 24.0f, 24, (tb_vec4){0.35f, 0.36f, 0.4f, 1},
                  0);
    tb_debug_axes(box->renderer, tb_mat4_identity(), 1.0f, TB_DEBUG_ON_TOP);
    tb_debug_arrow(box->renderer, (tb_vec3){0, 2.5f, 0}, (tb_vec3){0, 1.6f, 0},
                   (tb_vec4){1, 1, 0.3f, 1}, 0);
}

void shapes_destroy(sandbox *box, void *state)
{
    shapes_scene *const scene = state;

    for (int shape = 0; shape < SHAPE_COUNT; shape++)
    {
        tb_mesh_destroy(box->renderer, scene->meshes[shape]);
    }
    tb_mesh_destroy(box->renderer, scene->floor);
    tb_free(scene);
}

/* ---------------------------------------------------------------------------
 * canvas: the 2D layer: shapes, lines, images made in memory, and sprites,
 * in logical units, over the shapes.
 * ------------------------------------------------------------------------- */

#define CANVAS_IMAGE 64u

typedef struct canvas_scene
{
    shapes_scene *shapes;
    tb_image *checker;
    tb_image *gradient;
    tb_image *noise;
} canvas_scene;

static tb_image *canvas_image(sandbox *box, int kind)
{
    const size_t size = (size_t)CANVAS_IMAGE * CANVAS_IMAGE * 4u;
    uint8_t *const pixels = tb_malloc(size);
    tb_image *image = NULL;

    if (kind == 0)
    {
        tb_image_fill_checkerboard(pixels, size, CANVAS_IMAGE, CANVAS_IMAGE, 8,
                                   (tb_vec4){0.95f, 0.95f, 0.95f, 1},
                                   (tb_vec4){0.2f, 0.25f, 0.35f, 1});
    }
    else if (kind == 1)
    {
        tb_image_fill_gradient(pixels, size, CANVAS_IMAGE, CANVAS_IMAGE, TB_IMAGE_GRADIENT_RADIAL,
                               (tb_vec4){1.0f, 0.8f, 0.2f, 1}, (tb_vec4){0.8f, 0.1f, 0.4f, 0});
    }
    else
    {
        tb_image_fill_noise(pixels, size, CANVAS_IMAGE, CANVAS_IMAGE, 8, 42);
    }
    image =
        tb_image_create(box->renderer,
                        &(tb_image_desc){.width = CANVAS_IMAGE,
                                         .height = CANVAS_IMAGE,
                                         .filter = kind == 0 ? TB_IMAGE_NEAREST : TB_IMAGE_LINEAR},
                        pixels, size);
    tb_free(pixels);
    return image;
}

static bool canvas_create(sandbox *box, void **state)
{
    canvas_scene *const scene = tb_calloc(1, sizeof(*scene));
    void *shapes = NULL;

    *state = scene;
    (void)shapes_create(box, &shapes);
    scene->shapes = shapes;
    scene->checker = canvas_image(box, 0);
    scene->gradient = canvas_image(box, 1);
    scene->noise = canvas_image(box, 2);
    return true;
}

static void canvas_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    const canvas_scene *const scene = state;
    const float t = (float)frame->seconds;
    const tb_canvas_view view = tb_canvas_view_logical(box->renderer);
    const tb_vec4 panel = {0.08f, 0.09f, 0.11f, 0.85f};
    const tb_vec4 accent = {0.35f, 0.65f, 1.0f, 1};

    shapes_draw(box, scene->shapes, frame);
    tb_canvas_set_view(box->renderer, &view);
    tb_canvas_set_layer(box->renderer, 0);
    tb_canvas_fill_rounded_rect(box->renderer, (tb_canvas_rect){20, 20, 380, 300}, 14.0f, panel);
    tb_canvas_stroke_rounded_rect(box->renderer, (tb_canvas_rect){20, 20, 380, 300}, 14.0f, 2.0f,
                                  accent);
    tb_canvas_set_layer(box->renderer, 1);
    tb_canvas_draw_image(box->renderer, scene->checker, (tb_vec2){40, 40}, (tb_vec4){1, 1, 1, 1});
    tb_canvas_draw_image(box->renderer, scene->gradient, (tb_vec2){120, 40}, (tb_vec4){1, 1, 1, 1});
    tb_canvas_draw_image(box->renderer, scene->noise, (tb_vec2){200, 40}, (tb_vec4){1, 1, 1, 1});
    tb_canvas_draw_sprite(box->renderer, &(tb_canvas_sprite){.image = scene->checker,
                                                             .position = {320, 72},
                                                             .size = {56, 56},
                                                             .pivot = {0.5f, 0.5f},
                                                             .rotation = t,
                                                             .tint = {1, 0.85f, 0.6f, 1}});
    for (int index = 0; index < 5; index++)
    {
        const float x = 50.0f + (float)index * 70.0f;
        tb_canvas_fill_circle(box->renderer, (tb_vec2){x, 160}, 8.0f + (float)index * 4.0f,
                              (tb_vec4){0.3f + 0.15f * (float)index, 0.8f, 0.5f, 0.9f});
        tb_canvas_stroke_circle(box->renderer, (tb_vec2){x, 160}, 26.0f, 1.5f, accent);
    }
    tb_canvas_fill_rect(box->renderer, (tb_canvas_rect){40, 200, 140, 40},
                        (tb_vec4){0.9f, 0.3f, 0.3f, 1});
    tb_canvas_stroke_rect(box->renderer, (tb_canvas_rect){200, 200, 160, 40}, 3.0f,
                          (tb_vec4){1, 1, 1, 1});
    for (int index = 0; index < 12; index++)
    {
        const float angle = t * 0.5f + (float)index * TB_TAU / 12.0f;
        tb_canvas_line(box->renderer, (tb_vec2){210, 285},
                       (tb_vec2){210 + cosf(angle) * 160.0f, 285 + sinf(angle) * 24.0f},
                       1.0f + (float)index * 0.25f, (tb_vec4){1, 1, 1, 0.6f});
    }
}

static void canvas_destroy(sandbox *box, void *state)
{
    canvas_scene *const scene = state;

    if (scene->shapes != NULL)
    {
        shapes_destroy(box, scene->shapes);
    }
    tb_image_destroy(box->renderer, scene->checker);
    tb_image_destroy(box->renderer, scene->gradient);
    tb_image_destroy(box->renderer, scene->noise);
    tb_free(scene);
}

/* ---------------------------------------------------------------------------
 * text: the UI font at sizes from 10 to 48, paragraphs wrapping in panels whose
 * width changes, left, centered and right, pixel fonts at whole multiples
 * of their sizes, and labels over the shapes.
 * ------------------------------------------------------------------------- */

#define TEXT_MARGIN 24.0f
#define TEXT_PANEL_PADDING 12.0f
#define TEXT_PARAGRAPH_SIZE 14.0f

static const char text_paragraph[] =
    "Tenacious Badger lays out text in lines: kerned pairs such as AV, To and "
    "Wa sit closer, and each glyph lands on a whole pixel at the display's "
    "own scale. Long words such as Llanfairpwllgwyngyllgogerychwyrndrobwll "
    "break where they must.\nA line break starts a new line.";

static const char *const text_shape_names[SHAPE_COUNT] = {"cube", "plane", "sphere", "cylinder",
                                                          "cone", "torus", "capsule"};

static bool text_create(sandbox *box, void **state)
{
    return shapes_create(box, state);
}

/* A rounded panel around a box of the canvas, grown by the padding. */
static void text_panel(sandbox *box, tb_canvas_rect inside)
{
    const tb_canvas_rect panel = {inside.x - TEXT_PANEL_PADDING, inside.y - TEXT_PANEL_PADDING,
                                  inside.width + TEXT_PANEL_PADDING * 2.0f,
                                  inside.height + TEXT_PANEL_PADDING * 2.0f};

    tb_canvas_fill_rounded_rect(box->renderer, panel, 8.0f, (tb_vec4){0.06f, 0.07f, 0.09f, 0.88f});
    tb_canvas_stroke_rounded_rect(box->renderer, panel, 8.0f, 1.0f,
                                  (tb_vec4){0.35f, 0.65f, 1.0f, 0.8f});
}

/* The UI font at a run of sizes, each line naming its own. */
static void text_sizes(sandbox *box, float y)
{
    static const float sizes[] = {10, 12, 14, 16, 20, 24, 32, 48};
    const tb_font *const font = box->fonts[SANDBOX_FONT_UI];

    for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++)
    {
        char line[96] = {0};
        const tb_text text =
            tb_text_make(line, tb_fmt_buffer(line, sizeof(line),
                                             "%.0f: The quick brown fox jumps over the lazy dog",
                                             (double)sizes[index]));
        tb_canvas_draw_text(box->renderer, font, text, sizes[index], (tb_vec2){TEXT_MARGIN, y},
                            (tb_vec4){0.92f, 0.94f, 0.97f, 1}, NULL);
        y += tb_canvas_measure_text(box->renderer, font, text, sizes[index], NULL).y;
    }
}

/* The paragraph in three panels, left, centered and right, as wide as the
 * time says, their bottoms at `bottom`. */
static void text_paragraphs(sandbox *box, float bottom, float t)
{
    static const tb_font_align aligns[] = {TB_FONT_ALIGN_LEFT, TB_FONT_ALIGN_CENTER,
                                           TB_FONT_ALIGN_RIGHT};
    const tb_font *const font = box->fonts[SANDBOX_FONT_UI];
    const float width = 210.0f + 100.0f * sinf(t * 0.7f - 1.2f);
    const tb_text text = TB_TEXT(text_paragraph);
    tb_font_layout_options options[3] = {0};
    float tallest = 0.0f;

    for (size_t index = 0; index < 3; index++)
    {
        options[index] = (tb_font_layout_options){.wrap_width = width, .align = aligns[index]};
        tallest = fmaxf(tallest, tb_canvas_measure_text(box->renderer, font, text,
                                                        TEXT_PARAGRAPH_SIZE, &options[index])
                                     .y);
    }
    for (size_t index = 0; index < 3; index++)
    {
        const tb_vec2 at = {TEXT_MARGIN + TEXT_PANEL_PADDING +
                                (float)index * (340.0f + TEXT_PANEL_PADDING * 3.0f),
                            bottom - TEXT_PANEL_PADDING - tallest};
        text_panel(box, (tb_canvas_rect){at.x, at.y, width, tallest});
        tb_canvas_set_layer(box->renderer, 1);
        tb_canvas_draw_text(box->renderer, font, text, TEXT_PARAGRAPH_SIZE, at,
                            (tb_vec4){0.85f, 0.88f, 0.92f, 1}, &options[index]);
        tb_canvas_set_layer(box->renderer, 0);
    }
}

/* One line of a pixel font, its right edge at `right`. */
static float text_pixel_line(sandbox *box, sandbox_font font, tb_text text, float size,
                             float pixel_size, tb_vec2 top_right, tb_vec4 color)
{
    const tb_font_layout_options options = {.pixel_size = pixel_size};
    tb_vec2 measured = {0};

    if (box->fonts[font] == NULL)
    {
        return top_right.y;
    }
    measured = tb_canvas_measure_text(box->renderer, box->fonts[font], text, size, &options);

    tb_canvas_draw_text(box->renderer, box->fonts[font], text, size,
                        (tb_vec2){top_right.x - measured.x, top_right.y}, color, &options);
    return top_right.y + measured.y;
}

/* The pixel fonts, on whole pixels at whole multiples of their sizes,
 * against the right edge. */
static void text_pixel_fonts(sandbox *box, float right, float y)
{
    y = text_pixel_line(box, SANDBOX_FONT_VGA, TB_TEXT("Px437 IBM VGA 8x16: C:\\> DIR /W"), 16.0f,
                        16.0f, (tb_vec2){right, y}, (tb_vec4){0.65f, 1.0f, 0.65f, 1});
    y = text_pixel_line(box, SANDBOX_FONT_VGA, TB_TEXT("A whole multiple larger"), 32.0f, 16.0f,
                        (tb_vec2){right, y}, (tb_vec4){1.0f, 0.75f, 0.35f, 1});
    y = text_pixel_line(box, SANDBOX_FONT_SILKSCREEN, TB_TEXT("SILKSCREEN"), 8.0f, 8.0f,
                        (tb_vec2){right, y + 8.0f}, (tb_vec4){1.0f, 0.55f, 0.8f, 1});
    (void)text_pixel_line(box, SANDBOX_FONT_SILKSCREEN, TB_TEXT("AND LARGER, BY WHOLE PIXELS"),
                          24.0f, 8.0f, (tb_vec2){right, y}, (tb_vec4){1.0f, 0.55f, 0.8f, 1});
}

static void text_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    const tb_canvas_view view = tb_canvas_view_logical(box->renderer);
    uint32_t width = 0;
    uint32_t height = 0;

    shapes_draw(box, state, frame);
    for (int shape = 0; shape < SHAPE_COUNT; shape++)
    {
        const float x = ((float)shape - (float)(SHAPE_COUNT - 1) * 0.5f) * 1.6f;
        tb_debug_text(box->renderer, (tb_vec3){x, 1.55f, 0.0f},
                      tb_text_from_cstr(text_shape_names[shape]), (tb_vec4){1.0f, 0.9f, 0.5f, 1});
    }
    if (box->fonts[SANDBOX_FONT_UI] == NULL)
    {
        return;
    }
    tb_renderer_size(box->renderer, &width, &height);
    tb_canvas_set_view(box->renderer, &view);
    text_sizes(box, TEXT_MARGIN);
    /* Room under the panels for the frame time. */
    text_paragraphs(box, (float)height / view.scale - 64.0f, (float)frame->seconds);
    text_pixel_fonts(box, (float)width / view.scale - TEXT_MARGIN, TEXT_MARGIN);
}

static void text_destroy(sandbox *box, void *state)
{
    shapes_destroy(box, state);
}

/* ---------------------------------------------------------------------------
 * triangle: three colored corners, turning, drawn through the RHI itself.
 * ------------------------------------------------------------------------- */

static const char triangle_source[] =
    "struct DrawConstants { float Angle; float Aspect; };\n"
    "ConstantBuffer<DrawConstants> Draw : register(b0);\n"
    "struct VertexOut { float4 Position : SV_Position; float4 Color : COLOR0; };\n"
    "VertexOut MainVS(uint id : SV_VertexID)\n"
    "{\n"
    "    const float turn = Draw.Angle + id * 2.0943951;\n"
    "    VertexOut output;\n"
    "    output.Position = float4(sin(turn) * 0.8 / Draw.Aspect, cos(turn) * 0.8, 0.5, 1);\n"
    "    output.Color = float4(id == 0, id == 1, id == 2, 1);\n"
    "    return output;\n"
    "}\n"
    "float4 MainPS(VertexOut input) : SV_Target { return input.Color; }\n";

typedef struct triangle_scene
{
    tb_rhi_pipeline pipeline;
    float angle;
} triangle_scene;

/* A pipeline from one source's MainVS and MainPS, the shaders released. */
static bool make_pipeline(sandbox *box, const char *path, const char *source,
                          tb_rhi_graphics_pipeline_desc *desc, tb_rhi_pipeline *pipeline)
{
    bool made = sandbox_shader(box, path, source, "MainVS", TB_RHI_STAGE_VERTEX, &desc->vertex) &&
                sandbox_shader(box, path, source, "MainPS", TB_RHI_STAGE_PIXEL, &desc->pixel);
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    if (made && tb_rhi_graphics_pipeline_create(box->device, desc, pipeline, &error) != TB_RHI_OK)
    {
        made = false;
        (void)tb_fmt_print_error("badger-sandbox: %s: %s\n", path, error.message);
    }
    tb_rhi_shader_destroy(box->device, desc->vertex);
    tb_rhi_shader_destroy(box->device, desc->pixel);
    return made;
}

static bool triangle_create(sandbox *box, void **state)
{
    triangle_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_rhi_graphics_pipeline_desc desc = sandbox_pipeline_desc(box);

    *state = scene;
    desc.name = "triangle";
    return make_pipeline(box, "/Sandbox/Triangle.hlsl", triangle_source, &desc, &scene->pipeline);
}

static void triangle_record(const tb_renderer_draw_context *draw)
{
    const triangle_scene *const scene = draw->context;
    const float constants[2] = {scene->angle, (float)draw->width / (float)draw->height};

    tb_rhi_cmd_pipeline(draw->list, scene->pipeline);
    tb_rhi_cmd_push_constants(draw->list, constants, sizeof(constants));
    tb_rhi_cmd_draw(draw->list, 3, 1, 0, 0);
}

static void triangle_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    triangle_scene *const scene = state;

    scene->angle = (float)frame->seconds;
    tb_renderer_draw_custom(box->renderer, triangle_record, scene);
}

static void triangle_destroy(sandbox *box, void *state)
{
    triangle_scene *const scene = state;

    tb_rhi_pipeline_destroy(box->device, scene->pipeline);
    tb_free(scene);
}

/* ---------------------------------------------------------------------------
 * textured: a checkerboard made in memory, on a turning quad, reached by the
 * texture's and the sampler's bindless indices.
 * ------------------------------------------------------------------------- */

static const char textured_source[] =
    "struct DrawConstants { uint Texture; uint Sampler; float Angle; float Aspect; };\n"
    "ConstantBuffer<DrawConstants> Draw : register(b0);\n"
    "struct VertexOut { float4 Position : SV_Position; float2 UV : TEXCOORD0; };\n"
    "VertexOut MainVS(uint id : SV_VertexID)\n"
    "{\n"
    "    const float2 corner = float2(id & 1, id >> 1);\n"
    "    const float2 centered = (corner - 0.5) * 1.4;\n"
    "    const float s = sin(Draw.Angle);\n"
    "    const float c = cos(Draw.Angle);\n"
    "    VertexOut output;\n"
    "    output.Position = float4((centered.x * c - centered.y * s) / Draw.Aspect,\n"
    "                             centered.x * s + centered.y * c, 0.5, 1);\n"
    "    output.UV = corner * 4;\n"
    "    return output;\n"
    "}\n"
    "float4 MainPS(VertexOut input) : SV_Target\n"
    "{\n"
    "    Texture2D<float4> image = ResourceDescriptorHeap[Draw.Texture];\n"
    "    SamplerState sampler = SamplerDescriptorHeap[Draw.Sampler];\n"
    "    return image.Sample(sampler, input.UV);\n"
    "}\n";

#define CHECKER_SIZE 64u
#define CHECKER_PITCH (CHECKER_SIZE * 4u) /* a multiple of 256, as copies need */

typedef struct textured_scene
{
    tb_rhi_pipeline pipeline;
    tb_rhi_texture texture;
    tb_rhi_sampler sampler;
    uint32_t texture_index;
    uint32_t sampler_index;
    float angle;
} textured_scene;

/* Fill the texture through a staging buffer, once, before any frame uses it. */
static bool textured_upload(sandbox *box, textured_scene *scene)
{
    uint8_t *texels = tb_calloc(CHECKER_SIZE * CHECKER_PITCH, 1);
    tb_rhi_buffer staging = {0};
    tb_rhi_command_list *list = NULL;

    for (uint32_t y = 0; y < CHECKER_SIZE; y++)
    {
        for (uint32_t x = 0; x < CHECKER_SIZE; x++)
        {
            const bool light = ((x / 8u) + (y / 8u)) % 2u == 0;
            uint8_t *const texel = &texels[y * CHECKER_PITCH + x * 4u];
            texel[0] = light ? 230 : (uint8_t)(40 + x * 2);
            texel[1] = light ? 230 : 50;
            texel[2] = light ? 230 : (uint8_t)(40 + y * 2);
            texel[3] = 255;
        }
    }
    if (tb_rhi_buffer_create(box->device,
                             &(tb_rhi_buffer_desc){.size = CHECKER_SIZE * CHECKER_PITCH,
                                                   .memory = TB_RHI_MEMORY_UPLOAD},
                             &staging, NULL) != TB_RHI_OK)
    {
        tb_free(texels);
        return false;
    }
    tb_rhi_buffer_write(box->device, staging, 0, texels, CHECKER_SIZE * CHECKER_PITCH);
    tb_free(texels);
    list = tb_rhi_command_list_begin(box->device);
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = scene->texture,
                                               .before = TB_RHI_STATE_COMMON,
                                               .after = TB_RHI_STATE_COPY_DEST});
    tb_rhi_cmd_copy_buffer_to_texture(list, &(tb_rhi_texture_copy){.buffer = staging,
                                                                   .row_pitch = CHECKER_PITCH,
                                                                   .texture = scene->texture});
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = scene->texture,
                                               .before = TB_RHI_STATE_COPY_DEST,
                                               .after = TB_RHI_STATE_SHADER_READ});
    (void)tb_rhi_submit(box->device, &list, 1);
    tb_rhi_buffer_destroy(box->device, staging);
    return true;
}

static bool textured_create(sandbox *box, void **state)
{
    textured_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_rhi_graphics_pipeline_desc desc = sandbox_pipeline_desc(box);

    *state = scene;
    if (tb_rhi_texture_create(box->device,
                              &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                                     .width = CHECKER_SIZE,
                                                     .height = CHECKER_SIZE,
                                                     .usage = TB_RHI_TEXTURE_SAMPLED,
                                                     .name = "checkerboard"},
                              &scene->texture, NULL) != TB_RHI_OK ||
        tb_rhi_sampler_create(box->device,
                              &(tb_rhi_sampler_desc){.min_filter = TB_RHI_FILTER_NEAREST,
                                                     .mag_filter = TB_RHI_FILTER_NEAREST},
                              &scene->sampler, NULL) != TB_RHI_OK ||
        !textured_upload(box, scene))
    {
        return false;
    }
    scene->texture_index = tb_rhi_texture_index(box->device, scene->texture);
    scene->sampler_index = tb_rhi_sampler_index(box->device, scene->sampler);
    desc.topology = TB_RHI_TOPOLOGY_TRIANGLE_STRIP;
    desc.name = "textured";
    return make_pipeline(box, "/Sandbox/Textured.hlsl", textured_source, &desc, &scene->pipeline);
}

static void textured_record(const tb_renderer_draw_context *draw)
{
    const textured_scene *const scene = draw->context;
    const struct
    {
        uint32_t texture;
        uint32_t sampler;
        float angle;
        float aspect;
    } constants = {scene->texture_index, scene->sampler_index, scene->angle,
                   (float)draw->width / (float)draw->height};

    tb_rhi_cmd_pipeline(draw->list, scene->pipeline);
    tb_rhi_cmd_push_constants(draw->list, &constants, sizeof(constants));
    tb_rhi_cmd_draw(draw->list, 4, 1, 0, 0);
}

static void textured_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    textured_scene *const scene = state;

    scene->angle = (float)frame->seconds * 0.5f;
    tb_renderer_draw_custom(box->renderer, textured_record, scene);
}

static void textured_destroy(sandbox *box, void *state)
{
    textured_scene *const scene = state;

    tb_rhi_pipeline_destroy(box->device, scene->pipeline);
    tb_rhi_texture_destroy(box->device, scene->texture);
    tb_rhi_sampler_destroy(box->device, scene->sampler);
    tb_free(scene);
}

/* ---------------------------------------------------------------------------
 * materials: the shapes with textures made in memory, mipmapped on the GPU,
 * a checkered floor that shows the filtering toward the horizon, and a row
 * of normal-mapped and metallic variants behind.
 * ------------------------------------------------------------------------- */

#define MATERIAL_TEXTURE 256u
#define MATERIAL_TILE 32u   /* texels a side of a tile of the normal map */
#define MATERIAL_BEVEL 6.0f /* texels of a tile's sloping edge */
#define MATERIAL_COUNT 6

typedef struct materials_scene
{
    shapes_scene *shapes;
    tb_texture *checker;
    tb_texture *gradient;
    tb_texture *noise;
    tb_texture *tiles;   /* a normal map of beveled tiles */
    tb_texture *surface; /* stripes of roughness and metal */
    tb_material *floor;
    tb_material *plain[MATERIAL_COUNT];
    tb_material *bumpy[MATERIAL_COUNT];
} materials_scene;

/* The height of a tile's texel: 0 in the grout, rising over the bevel to 1. */
static float materials_tile_height(float x, float y)
{
    const float tile = (float)MATERIAL_TILE;
    const float u = fmodf(x + tile, tile);
    const float v = fmodf(y + tile, tile);
    const float edge = fminf(fminf(u, tile - u), fminf(v, tile - v));

    return fminf(edge / MATERIAL_BEVEL, 1.0f);
}

/* A normal map of beveled tiles: each texel's normal from the slope of the
 * height around it, +Y up in the image. */
static void materials_fill_tiles(uint8_t *pixels)
{
    for (uint32_t y = 0; y < MATERIAL_TEXTURE; y++)
    {
        for (uint32_t x = 0; x < MATERIAL_TEXTURE; x++)
        {
            const float fx = (float)x + 0.5f;
            const float fy = (float)y + 0.5f;
            const float dx =
                materials_tile_height(fx + 1.0f, fy) - materials_tile_height(fx - 1.0f, fy);
            const float down =
                materials_tile_height(fx, fy + 1.0f) - materials_tile_height(fx, fy - 1.0f);
            /* Up in the image is down the rows. */
            const tb_vec3 normal = tb_vec3_normalize((tb_vec3){-dx * 1.5f, down * 1.5f, 1.0f});
            uint8_t *const texel = &pixels[((size_t)y * MATERIAL_TEXTURE + x) * 4u];
            texel[0] = (uint8_t)((normal.x * 0.5f + 0.5f) * 255.0f + 0.5f);
            texel[1] = (uint8_t)((normal.y * 0.5f + 0.5f) * 255.0f + 0.5f);
            texel[2] = (uint8_t)((normal.z * 0.5f + 0.5f) * 255.0f + 0.5f);
            texel[3] = UINT8_MAX;
        }
    }
}

/* Vertical stripes: roughness (green) from smooth to rough across, metal
 * (blue) in every other stripe. */
static void materials_fill_surface(uint8_t *pixels)
{
    for (uint32_t y = 0; y < MATERIAL_TEXTURE; y++)
    {
        for (uint32_t x = 0; x < MATERIAL_TEXTURE; x++)
        {
            const uint32_t stripe = x / 32u;
            uint8_t *const texel = &pixels[((size_t)y * MATERIAL_TEXTURE + x) * 4u];
            texel[0] = 0;
            texel[1] = (uint8_t)(40u + stripe * 30u);
            texel[2] = stripe % 2u == 0 ? UINT8_MAX : 0;
            texel[3] = UINT8_MAX;
        }
    }
}

static tb_texture *materials_texture(sandbox *box, int kind)
{
    const size_t size = (size_t)MATERIAL_TEXTURE * MATERIAL_TEXTURE * 4u;
    uint8_t *const pixels = tb_malloc(size);
    tb_texture_desc desc = {.width = MATERIAL_TEXTURE, .height = MATERIAL_TEXTURE};
    tb_texture *texture = NULL;

    switch (kind)
    {
    case 0:
        tb_image_fill_checkerboard(pixels, size, MATERIAL_TEXTURE, MATERIAL_TEXTURE, 32,
                                   (tb_vec4){0.92f, 0.92f, 0.90f, 1},
                                   (tb_vec4){0.18f, 0.22f, 0.30f, 1});
        desc.name = "checker";
        break;
    case 1:
        tb_image_fill_gradient(pixels, size, MATERIAL_TEXTURE, MATERIAL_TEXTURE,
                               TB_IMAGE_GRADIENT_VERTICAL, (tb_vec4){0.95f, 0.55f, 0.15f, 1},
                               (tb_vec4){0.20f, 0.45f, 0.95f, 1});
        desc.name = "gradient";
        break;
    case 2:
        tb_image_fill_noise(pixels, size, MATERIAL_TEXTURE, MATERIAL_TEXTURE, 16, 7);
        desc.name = "noise";
        break;
    case 3:
        materials_fill_tiles(pixels);
        desc.content = TB_TEXTURE_NORMAL;
        desc.name = "tiles";
        break;
    default:
        materials_fill_surface(pixels);
        desc.content = TB_TEXTURE_DATA;
        desc.name = "surface";
        break;
    }
    texture = tb_texture_create(box->renderer, &desc, pixels, size);
    tb_free(pixels);
    return texture;
}

static bool materials_create(sandbox *box, void **state)
{
    static const tb_vec4 tints[MATERIAL_COUNT] = {
        {1, 1, 1, 1}, {1, 1, 1, 1}, {0.95f, 0.75f, 0.55f, 1},
        {1, 1, 1, 1}, {1, 1, 1, 1}, {0.85f, 0.9f, 1.0f, 1}};
    materials_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_texture *bases[3] = {0};
    void *shapes = NULL;
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;

    *state = scene;
    (void)shapes_create(box, &shapes);
    scene->shapes = shapes;
    scene->checker = materials_texture(box, 0);
    scene->gradient = materials_texture(box, 1);
    scene->noise = materials_texture(box, 2);
    scene->tiles = materials_texture(box, 3);
    scene->surface = materials_texture(box, 4);
    bases[0] = scene->checker;
    bases[1] = scene->gradient;
    bases[2] = scene->noise;
    desc.base_color = scene->checker;
    desc.uv_scale = (tb_vec2){12, 12};
    desc.roughness = 0.8f;
    scene->floor = tb_material_create(box->renderer, &desc);
    for (int index = 0; index < MATERIAL_COUNT; index++)
    {
        desc = (tb_material_desc)TB_MATERIAL_DESC_INIT;
        desc.base_color = bases[index % 3];
        desc.base_color_factor = tints[index];
        desc.roughness = 0.7f;
        scene->plain[index] = tb_material_create(box->renderer, &desc);
        /* Behind: the same, bumped by tiles; every other one metal. */
        desc.normal = scene->tiles;
        desc.metallic_roughness = index % 2 == 0 ? NULL : scene->surface;
        desc.metallic = index % 2 == 0 ? 0.0f : 1.0f;
        desc.roughness = index % 2 == 0 ? 0.5f : 1.0f;
        scene->bumpy[index] = tb_material_create(box->renderer, &desc);
    }
    return true;
}

static void materials_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    static const tb_mesh_shape shapes[MATERIAL_COUNT] = {TB_MESH_CUBE,     TB_MESH_SPHERE,
                                                         TB_MESH_CYLINDER, TB_MESH_TORUS,
                                                         TB_MESH_CAPSULE,  TB_MESH_CONE};
    const materials_scene *const scene = state;
    const float turn = (float)frame->seconds * 0.4f;

    tb_renderer_draw_mesh_material(
        box->renderer, scene->shapes->floor,
        tb_mat4_from_trs((tb_vec3){0, -0.01f, 0}, tb_quat_identity(), (tb_vec3){60, 1, 60}),
        scene->floor);
    for (int index = 0; index < MATERIAL_COUNT; index++)
    {
        const float x = ((float)index - (float)(MATERIAL_COUNT - 1) * 0.5f) * 1.6f;
        const tb_quat spin = tb_quat_mul(tb_quat_from_axis_angle((tb_vec3){0, 1, 0}, turn),
                                         tb_quat_from_axis_angle((tb_vec3){1, 0, 0}, 0.4f));
        const tb_mesh *const mesh = scene->shapes->meshes[shapes[index]];
        tb_renderer_draw_mesh_material(
            box->renderer, mesh, tb_mat4_from_trs((tb_vec3){x, 0.75f, 0}, spin, (tb_vec3){1, 1, 1}),
            scene->plain[index]);
        tb_renderer_draw_mesh_material(
            box->renderer, mesh,
            tb_mat4_from_trs((tb_vec3){x, 0.75f, -2.0f}, spin, (tb_vec3){1, 1, 1}),
            scene->bumpy[index]);
    }
}

static void materials_destroy(sandbox *box, void *state)
{
    materials_scene *const scene = state;

    for (int index = 0; index < MATERIAL_COUNT; index++)
    {
        tb_material_destroy(box->renderer, scene->plain[index]);
        tb_material_destroy(box->renderer, scene->bumpy[index]);
    }
    tb_material_destroy(box->renderer, scene->floor);
    tb_texture_destroy(box->renderer, scene->checker);
    tb_texture_destroy(box->renderer, scene->gradient);
    tb_texture_destroy(box->renderer, scene->noise);
    tb_texture_destroy(box->renderer, scene->tiles);
    tb_texture_destroy(box->renderer, scene->surface);
    if (scene->shapes != NULL)
    {
        shapes_destroy(box, scene->shapes);
    }
    tb_free(scene);
}

/* ---------------------------------------------------------------------------
 * sky and sky_cube: the shapes under a procedural sky whose sun circles
 * slowly, and under a cube map made in memory: a dusk gradient with lines
 * every 15 degrees of latitude and longitude, to see its seams and its
 * orientation.
 * ------------------------------------------------------------------------- */

#define SKY_FACE 256u
#define SKY_GRID_DEGREES 15.0f

typedef struct sky_scene
{
    shapes_scene *shapes;
    tb_texture *cube; /* NULL for the procedural sky */
} sky_scene;

/* The direction of a texel of a cube's face, as Direct3D lays the faces
 * out: +X, -X, +Y, -Y, +Z, -Z, each seen from inside with u to the right
 * and v down. */
static tb_vec3 sky_face_direction(uint32_t face, float u, float v)
{
    switch (face)
    {
    case 0:
        return (tb_vec3){1, -v, -u};
    case 1:
        return (tb_vec3){-1, -v, u};
    case 2:
        return (tb_vec3){u, 1, v};
    case 3:
        return (tb_vec3){u, -1, -v};
    case 4:
        return (tb_vec3){u, -v, 1};
    default:
        return (tb_vec3){-u, -v, -1};
    }
}

/* A dusk: orange at the horizon to deep blue overhead, dark ground, and
 * lines of latitude and longitude. */
static void sky_fill_face(uint8_t *pixels, uint32_t face)
{
    const tb_vec3 zenith = {0.05f, 0.08f, 0.30f};
    const tb_vec3 horizon = {0.95f, 0.45f, 0.20f};
    const tb_vec3 ground = {0.08f, 0.06f, 0.06f};

    for (uint32_t y = 0; y < SKY_FACE; y++)
    {
        for (uint32_t x = 0; x < SKY_FACE; x++)
        {
            const float u = ((float)x + 0.5f) / (float)SKY_FACE * 2.0f - 1.0f;
            const float v = ((float)y + 0.5f) / (float)SKY_FACE * 2.0f - 1.0f;
            const tb_vec3 direction = tb_vec3_normalize(sky_face_direction(face, u, v));
            const float latitude = asinf(direction.y) * 180.0f / TB_PI;
            const float longitude = atan2f(direction.x, -direction.z) * 180.0f / TB_PI;
            const float near_line = fminf(fabsf(remainderf(latitude, SKY_GRID_DEGREES)),
                                          fabsf(remainderf(longitude, SKY_GRID_DEGREES)));
            tb_vec3 color = direction.y >= 0.0f
                                ? tb_vec3_lerp(horizon, zenith, sqrtf(direction.y))
                                : tb_vec3_lerp(horizon, ground, fminf(-direction.y * 8.0f, 1.0f));
            uint8_t *const texel =
                &pixels[((size_t)face * SKY_FACE * SKY_FACE + (size_t)y * SKY_FACE + x) * 4u];
            /* A line a little over half a degree wide, its edges smoothed. */
            const float line = 1.0f - fminf(fmaxf((near_line - 0.15f) / 0.3f, 0.0f), 1.0f);
            color = tb_vec3_lerp(color, (tb_vec3){1, 1, 1}, 0.5f * line);
            texel[0] = (uint8_t)(fminf(color.x, 1.0f) * 255.0f + 0.5f);
            texel[1] = (uint8_t)(fminf(color.y, 1.0f) * 255.0f + 0.5f);
            texel[2] = (uint8_t)(fminf(color.z, 1.0f) * 255.0f + 0.5f);
            texel[3] = UINT8_MAX;
        }
    }
}

static bool sky_create_any(sandbox *box, void **state, bool cube)
{
    sky_scene *const scene = tb_calloc(1, sizeof(*scene));
    void *shapes = NULL;

    *state = scene;
    (void)shapes_create(box, &shapes);
    scene->shapes = shapes;
    if (cube)
    {
        const size_t size = (size_t)SKY_FACE * SKY_FACE * 4u * 6u;
        uint8_t *const pixels = tb_malloc(size);
        tb_sky sky = {.kind = TB_SKY_CUBE};
        for (uint32_t face = 0; face < 6; face++)
        {
            sky_fill_face(pixels, face);
        }
        scene->cube = tb_texture_create_cube(
            box->renderer,
            &(tb_texture_desc){.width = SKY_FACE, .height = SKY_FACE, .name = "sky cube"}, pixels,
            size);
        tb_free(pixels);
        sky.cube = scene->cube;
        tb_renderer_set_sky(box->renderer, &sky);
    }
    return true;
}

static bool sky_create(sandbox *box, void **state)
{
    return sky_create_any(box, state, false);
}

static bool sky_cube_create(sandbox *box, void **state)
{
    return sky_create_any(box, state, true);
}

static void sky_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    const sky_scene *const scene = state;

    if (scene->cube == NULL)
    {
        /* The sun circles the sky once a minute, low ahead of the camera
         * at first, higher behind it. */
        const float turn = (float)frame->seconds * TB_TAU / 60.0f;
        tb_sky sky = TB_SKY_PROCEDURAL_INIT;
        sky.sun_direction = (tb_vec3){sinf(turn) + 0.3f, 0.3f - 0.2f * cosf(turn), -cosf(turn)};
        tb_renderer_set_sky(box->renderer, &sky);
    }
    shapes_draw(box, scene->shapes, frame);
}

static void sky_destroy(sandbox *box, void *state)
{
    sky_scene *const scene = state;

    /* The sky lasts until it is changed: the next scene starts without. */
    tb_renderer_set_sky(box->renderer, NULL);
    tb_texture_destroy(box->renderer, scene->cube);
    if (scene->shapes != NULL)
    {
        shapes_destroy(box, scene->shapes);
    }
    tb_free(scene);
}

const sandbox_scene sandbox_scenes[] = {
    {"shapes",
     "every shape the renderer makes, lit, under the fly camera",
     {0.10f, 0.11f, 0.13f, 1},
     shapes_create,
     shapes_draw,
     shapes_destroy,
     NULL},
    {"canvas",
     "the 2D layer: shapes, lines, images and sprites over the shapes",
     {0.10f, 0.11f, 0.13f, 1},
     canvas_create,
     canvas_draw,
     canvas_destroy,
     NULL},
    {"materials",
     "the shapes with textures, normal maps and metal, mipmapped, on a checkered floor",
     {0.10f, 0.11f, 0.13f, 1},
     materials_create,
     materials_draw,
     materials_destroy,
     NULL},
    {"sky",
     "the shapes under a procedural sky: a gradient, a ground, and a circling sun",
     {0.10f, 0.11f, 0.13f, 1},
     sky_create,
     sky_draw,
     sky_destroy,
     NULL},
    {"sky_cube",
     "the shapes under a cube-map sky made in memory: a dusk with lines of latitude and longitude",
     {0.10f, 0.11f, 0.13f, 1},
     sky_cube_create,
     sky_draw,
     sky_destroy,
     NULL},
    {"text",
     "text: sizes, wrapping paragraphs, alignment, pixel fonts, and labels in the world",
     {0.10f, 0.11f, 0.13f, 1},
     text_create,
     text_draw,
     text_destroy,
     NULL},
    {"skinning",
     "a row of tentacles skinned on the GPU, blending a wave and a curl, their bones drawn over",
     {0.10f, 0.11f, 0.13f, 1},
     sandbox_skinning_create,
     sandbox_skinning_draw,
     sandbox_skinning_destroy,
     NULL},
    {"ui",
     "the interface: windows of every widget over the shapes; the camera flies outside them",
     {0.10f, 0.11f, 0.13f, 1},
     ui_scene_create,
     ui_scene_draw,
     ui_scene_destroy,
     ui_scene_event},
    {"triangle",
     "a triangle of three colored corners, turning, drawn through the RHI",
     {0.08f, 0.09f, 0.11f, 1},
     triangle_create,
     triangle_draw,
     triangle_destroy,
     NULL},
    {"textured",
     "a checkerboard made in memory, sampled by bindless index",
     {0.08f, 0.09f, 0.11f, 1},
     textured_create,
     textured_draw,
     textured_destroy,
     NULL},
    {"clear",
     "the swap chain cleared to a plain color",
     {0.20f, 0.35f, 0.55f, 1},
     clear_create,
     clear_draw,
     clear_destroy,
     NULL},
};

const size_t sandbox_scene_count = sizeof(sandbox_scenes) / sizeof(sandbox_scenes[0]);
