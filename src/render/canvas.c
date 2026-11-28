/* The canvas on the GPU: images as textures, the shader that draws every
 * quad of a frame in one instanced draw, and the pass that draws them over
 * the frame. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/fatal.h"
#include "badger/log.h"
#include "badger/memory.h"
#include "badger/window.h"

#include <math.h>
#include <string.h>

/* Bytes of each quad's instance, as the shader reads it. */
#define TB_CANVAS_QUAD_BYTES 96u
/* Alignment of the quads in the frame's uploads: a float4's. */
#define TB_CANVAS_QUAD_ALIGNMENT 16u
/* Vertices of a quad: two triangles. */
#define TB_CANVAS_QUAD_VERTICES 6u

TB_STATIC_ASSERT(sizeof(tb_canvas_quad) == TB_CANVAS_QUAD_BYTES, "a quad as the shader reads it");

struct tb_image
{
    tb_renderer_source source;
    uint8_t *pixels; /* packed RGBA, in the caller's alpha convention */
    tb_rhi_texture texture;
    uint32_t width;
    uint32_t height;
    uint32_t index;   /* the texture's bindless index */
    uint32_t sampler; /* its sampler's bindless index */
    bool premultiplied;
};

/* ---------------------------------------------------------------------------
 * Images.
 * ------------------------------------------------------------------------- */

/* Copy RGBA pixels into a region of an image's texture, premultiplied as the
 * image keeps them, and wait for nothing: the copy is submitted before any
 * frame that draws it. */
static tb_rhi_result tb_image_upload(tb_renderer *renderer, tb_image *image, tb_rhi_rect region,
                                     const uint8_t *pixels, bool first, tb_rhi_error *error)
{
    const size_t row = (size_t)region.width * 4u;
    const size_t pitch = (row + TB_RHI_TEXTURE_COPY_PITCH_ALIGNMENT - 1u) &
                         ~(size_t)(TB_RHI_TEXTURE_COPY_PITCH_ALIGNMENT - 1u);
    uint8_t *staged = tb_calloc(region.height, pitch);
    tb_rhi_buffer staging = {0};
    tb_rhi_command_list *list = NULL;

    for (uint32_t y = 0; pixels != NULL && y < region.height; y++)
    {
        memcpy(&staged[pitch * y], &pixels[row * y], row);
        if (!image->premultiplied)
        {
            tb_image_premultiply(&staged[pitch * y], region.width);
        }
    }
    const tb_rhi_result result =
        tb_rhi_buffer_create(renderer->device,
                             &(tb_rhi_buffer_desc){.size = (uint64_t)pitch * region.height,
                                                   .memory = TB_RHI_MEMORY_UPLOAD,
                                                   .name = "image pixels"},
                             &staging, error);
    if (result != TB_RHI_OK)
    {
        tb_free(staged);
        return result;
    }
    tb_rhi_buffer_write(renderer->device, staging, 0, staged, pitch * region.height);
    tb_free(staged);
    list = tb_rhi_command_list_begin(renderer->device);
    tb_rhi_cmd_barrier(
        list, &(tb_rhi_barrier){.texture = image->texture,
                                .before = first ? TB_RHI_STATE_COMMON : TB_RHI_STATE_SHADER_READ,
                                .after = TB_RHI_STATE_COPY_DEST});
    tb_rhi_cmd_copy_buffer_to_texture(list, &(tb_rhi_texture_copy){.buffer = staging,
                                                                   .row_pitch = (uint32_t)pitch,
                                                                   .texture = image->texture,
                                                                   .region = region});
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = image->texture,
                                               .before = TB_RHI_STATE_COPY_DEST,
                                               .after = TB_RHI_STATE_SHADER_READ});
    (void)tb_rhi_submit(renderer->device, &list, 1);
    /* The staging buffer goes once the GPU has copied it. */
    tb_rhi_buffer_destroy(renderer->device, staging);
    return TB_RHI_OK;
}

static tb_rhi_result tb_image_restore(tb_renderer *renderer, tb_renderer_source *source,
                                      tb_rhi_error *error)
{
    tb_image *const image = TB_LIST_ENTRY(source, tb_image, source);
    return tb_image_upload(renderer, image, (tb_rhi_rect){0, 0, image->width, image->height},
                           image->pixels, true, error);
}

tb_image *tb_image_create(tb_renderer *renderer, const tb_image_desc *desc, const uint8_t *pixels,
                          size_t size)
{
    tb_image *image = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    TB_ASSERT(renderer != NULL && desc != NULL);
    TB_ASSERT_MSG(desc->width > 0 && desc->height > 0 &&
                      desc->width <= renderer->info.max_texture_size &&
                      desc->height <= renderer->info.max_texture_size,
                  "an image of a size the device makes");
    TB_ASSERT_MSG(pixels == NULL ? size == 0 : size == (size_t)desc->width * desc->height * 4u,
                  "width * height RGBA pixels, or none");
    image = tb_calloc(1, sizeof(*image));
    image->width = desc->width;
    image->height = desc->height;
    image->premultiplied = desc->premultiplied;
    if (tb_rhi_texture_create(
            renderer->device,
            &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                   .width = desc->width,
                                   .height = desc->height,
                                   .usage = TB_RHI_TEXTURE_SAMPLED,
                                   .name = desc->name != NULL ? desc->name : "image"},
            &image->texture, &error) != TB_RHI_OK)
    {
        tb_fatal("renderer: no memory for an image: %s", error.message);
    }
    image->index = tb_rhi_texture_index(renderer->device, image->texture);
    image->sampler = tb_rhi_sampler_index(
        renderer->device, renderer->canvas_samplers[desc->filter == TB_IMAGE_NEAREST ? 1 : 0]);
    if (tb_image_upload(renderer, image, (tb_rhi_rect){0, 0, desc->width, desc->height}, pixels,
                        true, &error) != TB_RHI_OK)
    {
        tb_fatal("renderer: no memory for an image's pixels: %s", error.message);
    }
    if ((renderer->info.features & TB_RHI_FEATURE_DEVICE_RECOVERY) != 0)
    {
        image->pixels = tb_calloc((size_t)desc->width * desc->height, 4u);
        if (pixels != NULL)
        {
            memcpy(image->pixels, pixels, size);
        }
        image->source.upload = tb_image_restore;
        tb_list_push_back(&renderer->sources, &image->source.link);
    }
    return image;
}

void tb_image_update(tb_renderer *renderer, tb_image *image, tb_rhi_rect region,
                     const uint8_t *pixels, size_t size)
{
    TB_ASSERT(renderer != NULL && image != NULL && pixels != NULL);
    if (region.width == 0 || region.height == 0)
    {
        region = (tb_rhi_rect){0, 0, image->width, image->height};
    }
    TB_ASSERT_MSG(region.x >= 0 && region.y >= 0 && (uint32_t)region.x <= image->width &&
                      region.width <= image->width - (uint32_t)region.x &&
                      (uint32_t)region.y <= image->height &&
                      region.height <= image->height - (uint32_t)region.y,
                  "a region inside the image");
    TB_ASSERT_MSG(size == (size_t)region.width * region.height * 4u, "the region's RGBA pixels");
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    if (tb_image_upload(renderer, image, region, pixels, false, &error) != TB_RHI_OK)
    {
        tb_fatal("renderer: no memory for an image's pixels: %s", error.message);
    }
    if (image->pixels != NULL)
    {
        const size_t row = (size_t)region.width * 4u;
        for (uint32_t y = 0; y < region.height; y++)
        {
            const size_t start = ((size_t)region.y + y) * image->width + (size_t)region.x;
            memcpy(image->pixels + start * 4u, pixels + row * y, row);
        }
    }
}

void tb_image_destroy(tb_renderer *renderer, tb_image *image)
{
    TB_ASSERT(renderer != NULL);
    if (image == NULL)
    {
        return;
    }
    tb_rhi_texture_destroy(renderer->device, image->texture);
    if (tb_list_link_is_linked(&image->source.link))
    {
        tb_list_remove(&image->source.link);
        tb_free(image->pixels);
    }
    tb_free(image);
}

void tb_image_size(const tb_image *image, uint32_t *width, uint32_t *height)
{
    TB_ASSERT(image != NULL && width != NULL && height != NULL);
    *width = image->width;
    *height = image->height;
}

/* ---------------------------------------------------------------------------
 * Drawing.
 * ------------------------------------------------------------------------- */

tb_canvas_view tb_canvas_view_logical(const tb_renderer *renderer)
{
    tb_window_state state = {0};

    TB_ASSERT(renderer != NULL);
    tb_window_state_of(renderer->window, &state);
    return (tb_canvas_view){.scale = state.content_scale > 0.0 ? (float)state.content_scale : 1.0f};
}

static void tb_canvas_check_frame(const tb_renderer *renderer)
{
    TB_ASSERT(renderer != NULL);
    TB_ASSERT_MSG(renderer->in_frame, "the canvas drawn outside a frame");
}

void tb_canvas_set_view(tb_renderer *renderer, const tb_canvas_view *view)
{
    tb_canvas_check_frame(renderer);
    TB_ASSERT(view != NULL);
    TB_ASSERT_MSG(view->scale > 0.0f, "a view of some scale");
    tb_canvas_batch_set_view(&renderer->canvas, view);
}

void tb_canvas_set_layer(tb_renderer *renderer, int32_t layer)
{
    tb_canvas_check_frame(renderer);
    renderer->canvas.layer = layer;
}

/* A shape of the canvas's own: solid, its edges smoothed. */
static tb_canvas_shape tb_canvas_solid(const tb_renderer *renderer, tb_vec2 position, tb_vec2 size,
                                       tb_vec4 color)
{
    tb_canvas_shape shape = {0};

    shape.position = position;
    shape.size = size;
    shape.color = color;
    shape.uv[2] = 1.0f;
    shape.uv[3] = 1.0f;
    shape.texture = renderer->canvas_white->index;
    shape.sampler = renderer->canvas_white->sampler;
    shape.flags = TB_CANVAS_QUAD_SMOOTH;
    return shape;
}

static void tb_canvas_add(tb_renderer *renderer, const tb_canvas_shape *shape)
{
    (void)tb_canvas_batch_add(&renderer->canvas, shape);
}

void tb_canvas_fill_rect(tb_renderer *renderer, tb_canvas_rect rect, tb_vec4 color)
{
    tb_canvas_stroke_rounded_rect(renderer, rect, 0.0f, 0.0f, color);
}

void tb_canvas_stroke_rect(tb_renderer *renderer, tb_canvas_rect rect, float thickness,
                           tb_vec4 color)
{
    tb_canvas_stroke_rounded_rect(renderer, rect, 0.0f, thickness, color);
}

void tb_canvas_fill_rounded_rect(tb_renderer *renderer, tb_canvas_rect rect, float radius,
                                 tb_vec4 color)
{
    tb_canvas_stroke_rounded_rect(renderer, rect, radius, 0.0f, color);
}

void tb_canvas_stroke_rounded_rect(tb_renderer *renderer, tb_canvas_rect rect, float radius,
                                   float thickness, tb_vec4 color)
{
    tb_canvas_shape shape = {0};

    tb_canvas_check_frame(renderer);
    TB_ASSERT_MSG(radius >= 0.0f && thickness >= 0.0f, "a radius and a thickness of 0 or more");
    if (rect.width <= 0.0f || rect.height <= 0.0f)
    {
        return;
    }
    shape = tb_canvas_solid(renderer, (tb_vec2){rect.x, rect.y}, (tb_vec2){rect.width, rect.height},
                            color);
    shape.radius = radius;
    shape.thickness = thickness;
    tb_canvas_add(renderer, &shape);
}

void tb_canvas_fill_circle(tb_renderer *renderer, tb_vec2 center, float radius, tb_vec4 color)
{
    tb_canvas_stroke_circle(renderer, center, radius, 0.0f, color);
}

void tb_canvas_stroke_circle(tb_renderer *renderer, tb_vec2 center, float radius, float thickness,
                             tb_vec4 color)
{
    tb_canvas_shape shape = {0};

    tb_canvas_check_frame(renderer);
    TB_ASSERT_MSG(thickness >= 0.0f, "a thickness of 0 or more");
    if (radius <= 0.0f)
    {
        return;
    }
    shape = tb_canvas_solid(renderer, center, (tb_vec2){radius * 2.0f, radius * 2.0f}, color);
    shape.pivot = (tb_vec2){0.5f, 0.5f};
    shape.radius = radius;
    shape.thickness = thickness;
    tb_canvas_add(renderer, &shape);
}

void tb_canvas_line(tb_renderer *renderer, tb_vec2 a, tb_vec2 b, float thickness, tb_vec4 color)
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    tb_canvas_shape shape = {0};

    tb_canvas_check_frame(renderer);
    TB_ASSERT_MSG(thickness >= 0.0f, "a thickness of 0 or more");
    shape = tb_canvas_solid(renderer, a, (tb_vec2){sqrtf(dx * dx + dy * dy), thickness}, color);
    shape.pivot = (tb_vec2){0.0f, 0.5f};
    shape.rotation = atan2f(dy, dx);
    tb_canvas_add(renderer, &shape);
}

void tb_canvas_draw_image(tb_renderer *renderer, const tb_image *image, tb_vec2 position,
                          tb_vec4 tint)
{
    TB_ASSERT(image != NULL);
    tb_canvas_draw_sprite(renderer,
                          &(tb_canvas_sprite){.image = image, .position = position, .tint = tint});
}

void tb_canvas_draw_sprite(tb_renderer *renderer, const tb_canvas_sprite *sprite)
{
    const tb_image *image = NULL;
    tb_rhi_rect source = {0};
    tb_canvas_shape shape = {0};

    tb_canvas_check_frame(renderer);
    TB_ASSERT(sprite != NULL);
    image = sprite->image != NULL ? sprite->image : renderer->canvas_white;
    source = sprite->source;
    if (source.width == 0 || source.height == 0)
    {
        source = (tb_rhi_rect){0, 0, image->width, image->height};
    }
    shape.position = sprite->position;
    shape.size = sprite->size;
    if (shape.size.x == 0.0f && shape.size.y == 0.0f)
    {
        shape.size = (tb_vec2){(float)source.width, (float)source.height};
    }
    shape.pivot = sprite->pivot;
    shape.rotation = sprite->rotation;
    shape.color = sprite->tint;
    shape.uv[0] = (float)source.x / (float)image->width;
    shape.uv[1] = (float)source.y / (float)image->height;
    shape.uv[2] = ((float)source.x + (float)source.width) / (float)image->width;
    shape.uv[3] = ((float)source.y + (float)source.height) / (float)image->height;
    shape.texture = image->index;
    shape.sampler = image->sampler;
    tb_canvas_add(renderer, &shape);
}

void tb_canvas_draw_nine_slice(tb_renderer *renderer, const tb_canvas_nine_slice *slice,
                               tb_canvas_rect rect, tb_vec4 tint)
{
    tb_canvas_slice slices[TB_CANVAS_NINE_SLICES] = {0};

    tb_canvas_check_frame(renderer);
    TB_ASSERT(slice != NULL);
    if (slice->image == NULL)
    {
        return;
    }
    const size_t count =
        tb_canvas_nine_slice_layout(slice, slice->image->width, slice->image->height, rect,
                                    renderer->canvas.view.scale, slices);
    for (size_t index = 0; index < count; index++)
    {
        tb_canvas_draw_sprite(
            renderer,
            &(tb_canvas_sprite){.image = slice->image,
                                .source = slices[index].source,
                                .position = {slices[index].rect.x, slices[index].rect.y},
                                .size = {slices[index].rect.width, slices[index].rect.height},
                                .tint = tint});
    }
}

void tb_canvas_stats_of(const tb_renderer *renderer, tb_canvas_stats *stats)
{
    TB_ASSERT(renderer != NULL && stats != NULL);
    *stats = renderer->canvas_stats;
}

/* ---------------------------------------------------------------------------
 * The renderer's part.
 * ------------------------------------------------------------------------- */

tb_rhi_result tb_renderer_canvas_create(tb_renderer *renderer, uint32_t glyph_atlas_size,
                                        tb_rhi_error *error)
{
    static const uint8_t white[4] = {UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX};
    tb_rhi_graphics_pipeline_desc desc = {0};
    tb_rhi_result result = TB_RHI_OK;

    result = tb_rhi_sampler_create(renderer->device,
                                   &(tb_rhi_sampler_desc){.address_u = TB_RHI_ADDRESS_CLAMP,
                                                          .address_v = TB_RHI_ADDRESS_CLAMP,
                                                          .address_w = TB_RHI_ADDRESS_CLAMP,
                                                          .name = "canvas linear"},
                                   &renderer->canvas_samplers[0], error);
    if (result == TB_RHI_OK)
    {
        result = tb_rhi_sampler_create(renderer->device,
                                       &(tb_rhi_sampler_desc){.min_filter = TB_RHI_FILTER_NEAREST,
                                                              .mag_filter = TB_RHI_FILTER_NEAREST,
                                                              .mip_filter = TB_RHI_FILTER_NEAREST,
                                                              .address_u = TB_RHI_ADDRESS_CLAMP,
                                                              .address_v = TB_RHI_ADDRESS_CLAMP,
                                                              .address_w = TB_RHI_ADDRESS_CLAMP,
                                                              .name = "canvas nearest"},
                                       &renderer->canvas_samplers[1], error);
    }
    if (result != TB_RHI_OK)
    {
        return result;
    }
    renderer->canvas_white = tb_image_create(
        renderer,
        &(tb_image_desc){.width = 1, .height = 1, .filter = TB_IMAGE_NEAREST, .name = "white"},
        white, sizeof(white));
    result = tb_renderer_text_create(renderer, glyph_atlas_size, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    if ((renderer->info.features & TB_RHI_FEATURE_HEAP_INDICES) == 0)
    {
        /* The canvas reaches its quads and images by index; without, it
         * draws nothing, and its statistics say so. */
        return TB_RHI_OK;
    }
    desc.topology = TB_RHI_TOPOLOGY_TRIANGLES;
    desc.blend[0] = (tb_rhi_blend){.enabled = true,
                                   .color_source = TB_RHI_BLEND_ONE,
                                   .color_dest = TB_RHI_BLEND_ONE_MINUS_SOURCE_ALPHA,
                                   .color_op = TB_RHI_BLEND_ADD,
                                   .alpha_source = TB_RHI_BLEND_ONE,
                                   .alpha_dest = TB_RHI_BLEND_ONE_MINUS_SOURCE_ALPHA,
                                   .alpha_op = TB_RHI_BLEND_ADD};
    desc.colors[0] = TB_RENDERER_COLOR_FORMAT;
    desc.color_count = 1;
    desc.name = "canvas";
    return tb_renderer_pipeline(renderer, "/Engine/Private/Canvas.hlsl", NULL, &desc,
                                &renderer->canvas_pipeline, error);
}

void tb_renderer_canvas_destroy(tb_renderer *renderer)
{
    tb_image_destroy(renderer, renderer->canvas_white);
    renderer->canvas_white = NULL;
    tb_renderer_text_destroy(renderer);
    tb_rhi_pipeline_destroy(renderer->device, renderer->canvas_pipeline);
    tb_rhi_sampler_destroy(renderer->device, renderer->canvas_samplers[0]);
    tb_rhi_sampler_destroy(renderer->device, renderer->canvas_samplers[1]);
    tb_canvas_batch_free(&renderer->canvas);
}

/* Where the run of quads that starts at `first` ends: at the first quad of
 * another image or another filter, or at `count`. A run is one draw, whose
 * push constants name its image and its sampler, so that the shader reads
 * every index from its constants, as D3D11 needs. */
static uint32_t tb_canvas_run_end(const tb_canvas_quad *quads, uint32_t first, uint32_t count)
{
    uint32_t end = first + 1;
    while (end < count && quads[end].texture == quads[first].texture &&
           quads[end].sampler == quads[first].sampler)
    {
        end++;
    }
    return end;
}

static uint32_t tb_canvas_count_runs(const tb_canvas_quad *quads, uint32_t count)
{
    uint32_t runs = 0;
    for (uint32_t first = 0; first < count; first = tb_canvas_run_end(quads, first, count))
    {
        runs++;
    }
    return runs;
}

static void tb_renderer_record_canvas(const tb_render_graph_pass_context *pass)
{
    const tb_canvas_draw *const draw = pass->context;
    const tb_renderer *const renderer = draw->renderer;
    const uint32_t quads =
        tb_rhi_buffer_index(renderer->device, renderer->frames[renderer->slot].upload);

    tb_rhi_cmd_pipeline(pass->list, renderer->canvas_pipeline);
    for (uint32_t first = 0; first < draw->count;)
    {
        const uint32_t end = tb_canvas_run_end(draw->quads, first, draw->count);
        const struct
        {
            float target[2];
            uint32_t quads;
            uint32_t offset;
            uint32_t image;
            uint32_t sampler;
        } constants = {{(float)draw->width, (float)draw->height},
                       quads,
                       (uint32_t)(draw->offset + (uint64_t)first * sizeof(tb_canvas_quad)),
                       draw->quads[first].texture,
                       draw->quads[first].sampler};
        tb_rhi_cmd_push_constants(pass->list, &constants, sizeof(constants));
        tb_rhi_cmd_draw(pass->list, TB_CANVAS_QUAD_VERTICES, end - first, 0, 0);
        first = end;
    }
}

void tb_renderer_canvas_upload(tb_renderer *renderer, tb_canvas_recording *recording)
{
    const size_t count = renderer->canvas.count;
    size_t counts[TB_CANVAS_TARGET_COUNT] = {0};
    const tb_canvas_quad *quads = NULL;
    size_t fit = 0;
    uint64_t offset = 0;

    renderer->canvas_stats = (tb_canvas_stats){0};
    *recording = (tb_canvas_recording){0};
    if (count == 0)
    {
        return;
    }
    if (renderer->canvas_pipeline.generation == 0)
    {
        renderer->canvas_stats.dropped = count;
        return;
    }
    quads = tb_canvas_batch_sort(&renderer->canvas, counts);
    /* What does not fit in the frame's uploads is not drawn: the last in
     * order, the frame's highest layers'. */
    fit = tb_renderer_upload_room(renderer, TB_CANVAS_QUAD_ALIGNMENT) / sizeof(*quads);
    fit = fit < count ? fit : count;
    renderer->canvas_stats.dropped = count - fit;
    if (fit == 0)
    {
        return;
    }
    offset = tb_renderer_upload(renderer, quads, fit * sizeof(*quads), TB_CANVAS_QUAD_ALIGNMENT);
    TB_ASSERT_MSG(offset <= UINT32_MAX, "the frame's uploads within 4 GiB");
    /* The viewport target's quads come first, then the frame's. */
    const size_t first =
        counts[TB_CANVAS_TARGET_VIEWPORT] < fit ? counts[TB_CANVAS_TARGET_VIEWPORT] : fit;
    recording->targets[TB_CANVAS_TARGET_VIEWPORT] = (tb_canvas_draw){
        .renderer = renderer, .offset = offset, .count = (uint32_t)first, .quads = quads};
    recording->targets[TB_CANVAS_TARGET_FRAME] =
        (tb_canvas_draw){.renderer = renderer,
                         .offset = offset + first * sizeof(*quads),
                         .count = (uint32_t)(fit - first),
                         .quads = quads + first};
    /* Glyphs the atlas could not hold are drawn with no color: not drawn. */
    renderer->canvas_stats.glyphs = renderer->glyph_count - renderer->glyphs_hidden;
    renderer->canvas_stats.quads =
        fit > renderer->glyphs_hidden ? fit - renderer->glyphs_hidden : 0;
    renderer->canvas_stats.dropped += renderer->glyphs_hidden;
}

void tb_renderer_canvas_record(tb_renderer *renderer, tb_canvas_recording *recording,
                               tb_canvas_target target, tb_render_graph_texture image,
                               uint32_t width, uint32_t height)
{
    tb_canvas_draw *const draw = &recording->targets[target];
    tb_render_graph_pass pass = {.execute = tb_renderer_record_canvas, .context = draw};

    if (draw->count == 0)
    {
        return;
    }
    draw->width = width;
    draw->height = height;
    renderer->canvas_stats.draws += tb_canvas_count_runs(draw->quads, draw->count);
    pass.name = target == TB_CANVAS_TARGET_VIEWPORT ? "canvas in the viewport" : "canvas";
    pass.colors[0] = (tb_render_graph_color){.texture = image, .load = TB_RHI_LOAD_KEEP};
    pass.color_count = 1;
    tb_render_graph_add_pass(renderer->graph, &pass);
}
