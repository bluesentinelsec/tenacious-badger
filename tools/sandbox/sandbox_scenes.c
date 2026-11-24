/* The sandbox's scenes: the renderer's shapes under the fly camera; and,
 * drawn through the RHI itself inside the renderer's scene pass, a turning
 * triangle and a texture reached by its bindless index; and a clear. */
#include "sandbox.h"

#include "badger/canvas.h"

#include "badger/fmt.h"
#include "badger/math.h"
#include "badger/memory.h"

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

static bool shapes_create(sandbox *box, void **state)
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

static void shapes_draw(sandbox *box, void *state, const sandbox_frame *frame)
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

static void shapes_destroy(sandbox *box, void *state)
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
 * text: Roboto at sizes from 10 to 48, paragraphs wrapping in panels whose
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

/* Roboto at a run of sizes, each line naming its own. */
static void text_sizes(sandbox *box, float y)
{
    static const float sizes[] = {10, 12, 14, 16, 20, 24, 32, 48};
    const tb_font *const font = box->fonts[SANDBOX_FONT_ROBOTO];

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
    const tb_font *const font = box->fonts[SANDBOX_FONT_ROBOTO];
    const float width = 230.0f + 110.0f * sinf(t * 0.7f);
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
    const tb_vec2 measured =
        tb_canvas_measure_text(box->renderer, box->fonts[font], text, size, &options);

    tb_canvas_draw_text(box->renderer, box->fonts[font], text, size,
                        (tb_vec2){top_right.x - measured.x, top_right.y}, color, &options);
    return top_right.y + measured.y;
}

/* The pixel fonts, on whole pixels at whole multiples of their sizes,
 * against the right edge. */
static void text_pixel_fonts(sandbox *box, float right, float y)
{
    y = text_pixel_line(box, SANDBOX_FONT_VGA, TB_TEXT("Px437 IBM VGA 8x16 at 16 px: C:\\> DIR /W"),
                        16.0f, 16.0f, (tb_vec2){right, y}, (tb_vec4){0.65f, 1.0f, 0.65f, 1});
    y = text_pixel_line(box, SANDBOX_FONT_VGA, TB_TEXT("Twice: 32 px"), 32.0f, 16.0f,
                        (tb_vec2){right, y}, (tb_vec4){1.0f, 0.75f, 0.35f, 1});
    y = text_pixel_line(box, SANDBOX_FONT_SILKSCREEN, TB_TEXT("SILKSCREEN AT 8 PX"), 8.0f, 8.0f,
                        (tb_vec2){right, y + 8.0f}, (tb_vec4){1.0f, 0.55f, 0.8f, 1});
    (void)text_pixel_line(box, SANDBOX_FONT_SILKSCREEN, TB_TEXT("AND AT 24, THREE TIMES"), 24.0f,
                          8.0f, (tb_vec2){right, y}, (tb_vec4){1.0f, 0.55f, 0.8f, 1});
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

const sandbox_scene sandbox_scenes[] = {
    {"shapes",
     "every shape the renderer makes, lit, under the fly camera",
     {0.10f, 0.11f, 0.13f, 1},
     shapes_create,
     shapes_draw,
     shapes_destroy},
    {"canvas",
     "the 2D layer: shapes, lines, images and sprites over the shapes",
     {0.10f, 0.11f, 0.13f, 1},
     canvas_create,
     canvas_draw,
     canvas_destroy},
    {"text",
     "text: sizes, wrapping paragraphs, alignment, pixel fonts, and labels in the world",
     {0.10f, 0.11f, 0.13f, 1},
     text_create,
     text_draw,
     text_destroy},
    {"triangle",
     "a triangle of three colored corners, turning, drawn through the RHI",
     {0.08f, 0.09f, 0.11f, 1},
     triangle_create,
     triangle_draw,
     triangle_destroy},
    {"textured",
     "a checkerboard made in memory, sampled by bindless index",
     {0.08f, 0.09f, 0.11f, 1},
     textured_create,
     textured_draw,
     textured_destroy},
    {"clear",
     "the swap chain cleared to a plain color",
     {0.20f, 0.35f, 0.55f, 1},
     clear_create,
     clear_draw,
     clear_destroy},
};

const size_t sandbox_scene_count = sizeof(sandbox_scenes) / sizeof(sandbox_scenes[0]);
