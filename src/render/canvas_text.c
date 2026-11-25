/* Text on the canvas: layouts drawn as quads of a glyph atlas's coverage,
 * the atlas kept on the GPU as a single-channel texture copied from what
 * changed in it, and labels at points in the world. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/fatal.h"
#include "badger/memory.h"

#include <math.h>
#include <string.h>

/* Subpixel positions of the pen that smooth glyphs are drawn at. */
#define TB_TEXT_SUBPIXEL_BINS 4u
/* The fewest pixels a side of the atlas. */
#define TB_TEXT_SMALLEST_ATLAS 16u
/* Times a frame's glyphs are found again when the atlas keeps clearing. */
#define TB_TEXT_MOST_REFRESHES 3
/* The layer labels are drawn on: over everything. */
#define TB_TEXT_LABEL_LAYER INT32_MAX
/* The size labels are drawn at until one is chosen, in logical units. */
#define TB_TEXT_DEFAULT_LABEL_SIZE 14.0f
/* A point this near the camera's plane, or behind it, has no label. */
#define TB_TEXT_NEAREST_W 0.000001f

tb_rhi_result tb_renderer_text_create(tb_renderer *renderer, uint32_t side, tb_rhi_error *error)
{
    tb_rhi_result result = TB_RHI_OK;

    side = side == 0 ? TB_FONT_ATLAS_DEFAULT_SIDE : side;
    TB_ASSERT_MSG(side >= TB_TEXT_SMALLEST_ATLAS && side <= TB_FONT_RASTER_MAX_SIDE &&
                      side <= renderer->info.max_texture_size,
                  "a glyph atlas of a size the device makes");
    renderer->glyph_side = side;
    renderer->glyph_atlas = tb_font_atlas_create(
        &(tb_font_atlas_options){
            .width = side, .height = side, .subpixel_bins = TB_TEXT_SUBPIXEL_BINS},
        NULL);
    renderer->text_arena = tb_arena_create(0);
    result = tb_rhi_texture_create(renderer->device,
                                   &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8_UNORM,
                                                          .width = side,
                                                          .height = side,
                                                          .usage = TB_RHI_TEXTURE_SAMPLED,
                                                          .name = "glyph atlas"},
                                   &renderer->glyph_texture, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    renderer->glyph_texture_index = tb_rhi_texture_index(renderer->device, renderer->glyph_texture);
    return TB_RHI_OK;
}

void tb_renderer_text_destroy(tb_renderer *renderer)
{
    tb_rhi_texture_destroy(renderer->device, renderer->glyph_texture);
    renderer->glyph_texture = (tb_rhi_texture){0};
    tb_font_atlas_destroy(renderer->glyph_atlas);
    renderer->glyph_atlas = NULL;
    tb_arena_destroy(renderer->text_arena);
    renderer->text_arena = NULL;
    tb_font_close(renderer->default_debug_font);
    renderer->default_debug_font = NULL;
    tb_font_catalog_destroy(renderer->default_catalog);
    renderer->default_catalog = NULL;
    tb_free(renderer->glyphs);
    tb_free(renderer->labels);
    tb_free(renderer->label_bytes);
    renderer->glyphs = NULL;
    renderer->labels = NULL;
    renderer->label_bytes = NULL;
}

void tb_renderer_text_begin_frame(tb_renderer *renderer)
{
    renderer->glyph_count = 0;
    renderer->glyphs_hidden = 0;
    tb_mutex_lock(&renderer->lines_lock);
    renderer->label_count = 0;
    renderer->label_bytes_used = 0;
    tb_mutex_unlock(&renderer->lines_lock);
}

/* ---------------------------------------------------------------------------
 * Drawing.
 * ------------------------------------------------------------------------- */

/* The texture coordinates of a rectangle of the atlas. */
static void tb_text_uv(const tb_renderer *renderer, const tb_font_atlas_rect *rect, float *uv)
{
    const float side = (float)renderer->glyph_side;

    uv[0] = (float)rect->x / side;
    uv[1] = (float)rect->y / side;
    uv[2] = (float)(rect->x + rect->width) / side;
    uv[3] = (float)(rect->y + rect->height) / side;
}

static void tb_text_remember(tb_renderer *renderer, const tb_canvas_glyph *glyph)
{
    if (renderer->glyph_count == renderer->glyph_capacity)
    {
        renderer->glyph_capacity =
            renderer->glyph_capacity == 0 ? 256 : renderer->glyph_capacity * 2;
        renderer->glyphs =
            tb_realloc_array(renderer->glyphs, renderer->glyph_capacity, sizeof(*renderer->glyphs));
    }
    renderer->glyphs[renderer->glyph_count] = *glyph;
    renderer->glyph_count++;
}

/* Draw one glyph with its pen at a pixel of the frame, on the baseline's row. */
static void tb_text_draw_glyph(tb_renderer *renderer, const tb_font *font,
                               const tb_font_layout *layout, uint16_t glyph, float pen,
                               float baseline, tb_vec4 color)
{
    const bool aliased = layout->mode == TB_FONT_RASTER_ALIASED;
    const float whole = aliased ? roundf(pen) : floorf(pen);
    tb_canvas_glyph kept = {.font = font, .glyph = glyph};
    tb_font_atlas_glyph placed = {0};
    tb_canvas_shape shape = {0};

    /* An aliased glyph is drawn on the whole pixel; a smooth one keeps the
     * pen's fraction, to the nearest of the atlas's bins. */
    kept.options = (tb_font_raster_options){.pixels_per_em = layout->pixels_per_em,
                                            .subpixel_x = aliased ? 0.0f : pen - whole,
                                            .mode = layout->mode};
    if (tb_font_atlas_get_glyph(renderer->glyph_atlas, font, glyph, &kept.options, &placed, NULL) !=
            TB_FONT_OK ||
        placed.rect.width == 0 || placed.rect.height == 0)
    {
        return; /* a damaged glyph, one larger than the atlas, or a blank one */
    }
    kept.generation = tb_font_atlas_generation(renderer->glyph_atlas);
    shape.position = (tb_vec2){whole + (float)placed.left, baseline - (float)placed.top};
    shape.size = (tb_vec2){(float)placed.rect.width, (float)placed.rect.height};
    shape.color = color;
    tb_text_uv(renderer, &placed.rect, shape.uv);
    shape.texture = renderer->glyph_texture_index;
    /* Each texel lands on one pixel, so the nearest is exact. */
    shape.sampler = tb_rhi_sampler_index(renderer->device, renderer->canvas_samplers[1]);
    shape.flags = TB_CANVAS_QUAD_COVERAGE;
    if (tb_canvas_batch_add(&renderer->canvas, &shape))
    {
        kept.quad = renderer->canvas.count - 1;
        tb_text_remember(renderer, &kept);
    }
}

/* Draw a layout with its box's top left at a pixel of the view's target,
 * each glyph on a whole pixel and each baseline on a whole row, clipped as
 * the view is. */
static void tb_text_draw_layout(tb_renderer *renderer, const tb_font *font,
                                const tb_font_layout *layout, tb_vec2 origin, tb_vec4 color)
{
    const tb_canvas_view view = renderer->canvas.view;

    renderer->canvas.view =
        (tb_canvas_view){.scale = 1.0f, .clip = view.clip, .target = view.target};
    for (size_t index = 0; index < layout->glyph_count; index++)
    {
        const tb_font_layout_glyph *const glyph = &layout->glyphs[index];
        const tb_font_layout_line *const line = &layout->lines[glyph->line];
        tb_text_draw_glyph(renderer, font, layout, glyph->glyph, origin.x + line->x + glyph->x,
                           roundf(origin.y + line->baseline), color);
    }
    renderer->canvas.view = view;
}

/* Options in the canvas's units made pixels at a scale. */
static tb_font_layout_options tb_text_options_in_pixels(const tb_font_layout_options *options,
                                                        float scale)
{
    tb_font_layout_options scaled = {0};

    if (options != NULL)
    {
        scaled = *options;
    }
    scaled.wrap_width *= scale;
    return scaled;
}

void tb_canvas_draw_text(tb_renderer *renderer, const tb_font *font, tb_text text, float size,
                         tb_vec2 position, tb_vec4 color, const tb_font_layout_options *options)
{
    tb_canvas_view view = {0};
    tb_font_layout_options scaled = {0};
    tb_font_layout layout = {0};
    tb_arena_mark mark = {0};

    TB_ASSERT(renderer != NULL);
    TB_ASSERT_MSG(renderer->in_frame, "the canvas drawn outside a frame");
    TB_ASSERT(font != NULL);
    if (!(color.w > 0.0f))
    {
        return;
    }
    view = renderer->canvas.view;
    scaled = tb_text_options_in_pixels(options, view.scale);
    mark = tb_arena_get_mark(renderer->text_arena);
    tb_font_layout_text(font, text, size * view.scale, &scaled, renderer->text_arena, &layout);
    tb_text_draw_layout(renderer, font, &layout, tb_canvas_view_to_pixels(&view, position), color);
    tb_arena_rewind(renderer->text_arena, mark);
}

tb_vec2 tb_canvas_measure_text(const tb_renderer *renderer, const tb_font *font, tb_text text,
                               float size, const tb_font_layout_options *options)
{
    const float scale = renderer->canvas.view.scale;
    tb_font_layout_options scaled = {0};
    tb_font_text_size measured = {0};

    TB_ASSERT(renderer != NULL && font != NULL);
    scaled = tb_text_options_in_pixels(options, scale);
    tb_font_measure_text(font, text, size * scale, &scaled, &measured);
    return (tb_vec2){measured.width / scale, measured.height / scale};
}

void tb_canvas_forget_font(tb_renderer *renderer, const tb_font *font)
{
    TB_ASSERT(renderer != NULL);
    TB_ASSERT_MSG(!renderer->in_frame, "a font forgotten inside a frame");
    (void)font; /* the atlas forgets every font at once */
    tb_font_atlas_reset(renderer->glyph_atlas);
}

/* ---------------------------------------------------------------------------
 * Labels.
 * ------------------------------------------------------------------------- */

void tb_renderer_set_debug_font(tb_renderer *renderer, const struct tb_font *font, float size)
{
    TB_ASSERT(renderer != NULL);
    TB_ASSERT_MSG(size > 0.0f, "a debug font of some size");
    renderer->debug_font = font;
    renderer->debug_font_size = size;
}

/* Open the font of the UI role of the engine's catalog, the first time a
 * label needs it; NULL in a build that carries none. */
static const tb_font *tb_text_default_debug_font(tb_renderer *renderer)
{
    size_t index = 0;

    if (renderer->default_debug_font_tried)
    {
        return renderer->default_debug_font;
    }
    renderer->default_debug_font_tried = true;
    renderer->default_catalog = tb_font_catalog_create(NULL);
    if (!tb_font_catalog_find_role(renderer->default_catalog, TB_FONT_ROLE_UI, &index))
    {
        return NULL;
    }
    (void)tb_font_catalog_open(renderer->default_catalog, index, NULL,
                               &renderer->default_debug_font);
    return renderer->default_debug_font;
}

void tb_debug_text(tb_renderer *renderer, tb_vec3 position, tb_text text, tb_vec4 color)
{
    tb_renderer_label *label = NULL;

    TB_ASSERT(renderer != NULL);
    TB_ASSERT(text.bytes != NULL || text.length == 0);
    tb_mutex_lock(&renderer->lines_lock);
    if (renderer->label_count == renderer->label_capacity)
    {
        renderer->label_capacity =
            renderer->label_capacity == 0 ? 64 : renderer->label_capacity * 2;
        renderer->labels =
            tb_realloc_array(renderer->labels, renderer->label_capacity, sizeof(*renderer->labels));
    }
    TB_ASSERT_MSG(text.length <= SIZE_MAX - renderer->label_bytes_used, "labels within memory");
    if (renderer->label_bytes_used + text.length > renderer->label_bytes_capacity)
    {
        size_t capacity =
            renderer->label_bytes_capacity == 0 ? 1024 : renderer->label_bytes_capacity;
        while (capacity < renderer->label_bytes_used + text.length)
        {
            TB_ASSERT_MSG(capacity <= SIZE_MAX / 2, "labels within memory");
            capacity *= 2;
        }
        renderer->label_bytes = tb_realloc(renderer->label_bytes, capacity);
        renderer->label_bytes_capacity = capacity;
    }
    if (text.length > 0)
    {
        memcpy(&renderer->label_bytes[renderer->label_bytes_used], text.bytes, text.length);
    }
    label = &renderer->labels[renderer->label_count];
    *label = (tb_renderer_label){position, color, renderer->label_bytes_used, text.length};
    renderer->label_bytes_used += text.length;
    renderer->label_count++;
    tb_mutex_unlock(&renderer->lines_lock);
}

/* Draw each label centered on where its point is drawn, over everything,
 * in a font. */
static void tb_text_draw_labels(tb_renderer *renderer, const tb_font *font)
{
    const tb_viewport *const viewport = &renderer->viewport;
    const float aspect = tb_viewport_aspect_of(viewport);
    const tb_mat4 clip = tb_mat4_mul(tb_camera_projection_of(&renderer->camera, aspect),
                                     tb_camera_view(&renderer->camera));
    const float size = (renderer->debug_font_size > 0.0f ? renderer->debug_font_size
                                                         : TB_TEXT_DEFAULT_LABEL_SIZE) *
                       tb_canvas_view_logical(renderer).scale;
    const tb_canvas_view view = renderer->canvas.view;
    const int32_t layer = renderer->canvas.layer;

    renderer->canvas.view = tb_canvas_view_pixels();
    renderer->canvas.layer = TB_TEXT_LABEL_LAYER;
    tb_mutex_lock(&renderer->lines_lock);
    for (size_t index = 0; index < renderer->label_count; index++)
    {
        const tb_renderer_label *const label = &renderer->labels[index];
        const tb_vec4 at = tb_mat4_transform(
            clip, (tb_vec4){label->position.x, label->position.y, label->position.z, 1.0f});
        tb_font_layout layout = {0};
        tb_vec2 origin = {0};
        if (!(at.w > TB_TEXT_NEAREST_W) || !(label->color.w > 0.0f))
        {
            continue;
        }
        tb_font_layout_text(font,
                            tb_text_make(&renderer->label_bytes[label->offset], label->length),
                            size, NULL, renderer->text_arena, &layout);
        origin.x = (float)viewport->x + (at.x / at.w * 0.5f + 0.5f) * (float)viewport->width -
                   layout.width * 0.5f;
        origin.y = (float)viewport->y + (0.5f - at.y / at.w * 0.5f) * (float)viewport->height -
                   layout.height * 0.5f;
        tb_text_draw_layout(renderer, font, &layout, (tb_vec2){roundf(origin.x), roundf(origin.y)},
                            label->color);
        tb_arena_reset(renderer->text_arena);
    }
    tb_mutex_unlock(&renderer->lines_lock);
    renderer->canvas.view = view;
    renderer->canvas.layer = layer;
}

/* ---------------------------------------------------------------------------
 * The atlas on the GPU.
 * ------------------------------------------------------------------------- */

/*
 * The atlas cleared after some of the frame's glyphs were placed, so their
 * rectangles may hold other glyphs now: find each again. Finding one may
 * clear the atlas again, when the frame's glyphs do not all fit; after a few
 * tries, the glyphs that are still not in the atlas are not drawn.
 */
static void tb_text_refresh(tb_renderer *renderer)
{
    for (int attempt = 0; attempt < TB_TEXT_MOST_REFRESHES; attempt++)
    {
        bool stale = false;
        for (size_t index = 0; index < renderer->glyph_count; index++)
        {
            tb_canvas_glyph *const glyph = &renderer->glyphs[index];
            tb_font_atlas_glyph placed = {0};
            if (glyph->generation == tb_font_atlas_generation(renderer->glyph_atlas))
            {
                continue;
            }
            stale = true;
            /* It was placed before, so it fits an empty atlas. */
            (void)tb_font_atlas_get_glyph(renderer->glyph_atlas, glyph->font, glyph->glyph,
                                          &glyph->options, &placed, NULL);
            glyph->generation = tb_font_atlas_generation(renderer->glyph_atlas);
            tb_text_uv(renderer, &placed.rect, renderer->canvas.quads[glyph->quad].uv);
        }
        if (!stale)
        {
            return;
        }
    }
    for (size_t index = 0; index < renderer->glyph_count; index++)
    {
        const tb_canvas_glyph *const glyph = &renderer->glyphs[index];
        if (glyph->generation != tb_font_atlas_generation(renderer->glyph_atlas))
        {
            memset(renderer->canvas.quads[glyph->quad].color, 0,
                   sizeof(renderer->canvas.quads[glyph->quad].color));
            renderer->glyphs_hidden++;
        }
    }
}

/* Bytes from a row of a rectangle to the next in the staging buffer. */
static size_t tb_text_pitch(const tb_font_atlas_rect *rect)
{
    return ((size_t)rect->width + TB_RHI_TEXTURE_COPY_PITCH_ALIGNMENT - 1u) &
           ~(size_t)(TB_RHI_TEXTURE_COPY_PITCH_ALIGNMENT - 1u);
}

static size_t tb_text_align_offset(size_t offset)
{
    return (offset + TB_RHI_TEXTURE_COPY_OFFSET_ALIGNMENT - 1u) &
           ~(size_t)(TB_RHI_TEXTURE_COPY_OFFSET_ALIGNMENT - 1u);
}

/* Copy the rectangles of the atlas that changed to its texture, in one list
 * submitted before the frame that draws them. */
static void tb_text_upload(tb_renderer *renderer)
{
    size_t count = 0;
    const tb_font_atlas_rect *const rects = tb_font_atlas_get_dirty(renderer->glyph_atlas, &count);
    tb_font_atlas_image image = {0};
    size_t size = 0;
    uint8_t *staged = NULL;
    tb_rhi_buffer staging = {0};
    tb_rhi_command_list *list = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    size_t offset = 0;

    tb_font_atlas_get_image(renderer->glyph_atlas, &image);
    if (count == 0 || image.pixels == NULL)
    {
        return; /* nothing changed; an atlas always has its pixels */
    }
    for (size_t index = 0; index < count; index++)
    {
        size = tb_text_align_offset(size) + tb_text_pitch(&rects[index]) * rects[index].height;
    }
    staged = tb_calloc(1, size);
    for (size_t index = 0; index < count; index++)
    {
        const tb_font_atlas_rect *const rect = &rects[index];
        const size_t pitch = tb_text_pitch(rect);
        offset = tb_text_align_offset(offset);
        for (uint32_t y = 0; y < rect->height; y++)
        {
            memcpy(&staged[offset + pitch * y],
                   &image.pixels[image.stride * (rect->y + y) + rect->x], rect->width);
        }
        offset += pitch * rect->height;
    }
    if (tb_rhi_buffer_create(renderer->device,
                             &(tb_rhi_buffer_desc){.size = size,
                                                   .memory = TB_RHI_MEMORY_UPLOAD,
                                                   .name = "glyph atlas pixels"},
                             &staging, &error) != TB_RHI_OK)
    {
        tb_fatal("renderer: no memory for the glyph atlas's pixels: %s", error.message);
    }
    tb_rhi_buffer_write(renderer->device, staging, 0, staged, size);
    tb_free(staged);
    list = tb_rhi_command_list_begin(renderer->device);
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = renderer->glyph_texture,
                                               .before = renderer->glyph_texture_written
                                                             ? TB_RHI_STATE_SHADER_READ
                                                             : TB_RHI_STATE_COMMON,
                                               .after = TB_RHI_STATE_COPY_DEST});
    offset = 0;
    for (size_t index = 0; index < count; index++)
    {
        const tb_font_atlas_rect *const rect = &rects[index];
        offset = tb_text_align_offset(offset);
        tb_rhi_cmd_copy_buffer_to_texture(
            list, &(tb_rhi_texture_copy){
                      .buffer = staging,
                      .offset = offset,
                      .row_pitch = (uint32_t)tb_text_pitch(rect),
                      .texture = renderer->glyph_texture,
                      .region = {(int32_t)rect->x, (int32_t)rect->y, rect->width, rect->height}});
        offset += tb_text_pitch(rect) * rect->height;
    }
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = renderer->glyph_texture,
                                               .before = TB_RHI_STATE_COPY_DEST,
                                               .after = TB_RHI_STATE_SHADER_READ});
    (void)tb_rhi_submit(renderer->device, &list, 1);
    /* The staging buffer goes once the GPU has copied it. */
    tb_rhi_buffer_destroy(renderer->device, staging);
    renderer->glyph_texture_written = true;
    tb_font_atlas_clear_dirty(renderer->glyph_atlas);
}

void tb_renderer_text_end_frame(tb_renderer *renderer)
{
    size_t labels = 0;
    const tb_font *font = renderer->debug_font;

    tb_mutex_lock(&renderer->lines_lock);
    labels = renderer->label_count;
    tb_mutex_unlock(&renderer->lines_lock);
    if (labels > 0 && font == NULL)
    {
        font = tb_text_default_debug_font(renderer);
    }
    if (labels > 0 && font != NULL)
    {
        tb_text_draw_labels(renderer, font);
    }
    tb_text_refresh(renderer);
    tb_text_upload(renderer);
}
