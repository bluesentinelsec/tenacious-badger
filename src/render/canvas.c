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
    tb_rhi_texture texture;
    uint32_t width;
    uint32_t height;
    uint32_t index;   /* the texture's bindless index */
    uint32_t sampler; /* its sampler's bindless index */
    bool premultiplied;
};

/* Each quad's pixels are corner + u * axis_x + v * axis_y, grown by a pixel
 * on every side when its edges are smoothed; the pixel shader measures the
 * distance to its rounded box's edge in pixels, and keeps the pixels inside
 * its clip. Colors and images are premultiplied. A quad of text reads its
 * image's first channel as coverage, which multiplies its color. */
static const char tb_canvas_source[] =
    "struct CanvasConstants { float2 Target; uint Quads; uint Offset; };\n"
    "ConstantBuffer<CanvasConstants> Canvas : register(b0);\n"
    "struct VertexOut\n"
    "{\n"
    "    float4 Position : SV_Position;\n"
    "    float2 Local : TEXCOORD0;\n"
    "    float2 UV : TEXCOORD1;\n"
    "    float4 Color : COLOR0;\n"
    "    nointerpolation float4 Clip : TEXCOORD2;\n"
    "    nointerpolation float4 Shape : TEXCOORD3;\n"
    "    nointerpolation uint3 Binding : TEXCOORD4;\n"
    "};\n"
    "VertexOut MainVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)\n"
    "{\n"
    "    ByteAddressBuffer quads = ResourceDescriptorHeap[Canvas.Quads];\n"
    "    const uint base = Canvas.Offset + instance * 96;\n"
    "    const float4 geometry = quads.Load<float4>(base);\n"
    "    const float4 shape = quads.Load<float4>(base + 16);\n"
    "    const float4 uv = quads.Load<float4>(base + 32);\n"
    "    const float2 size = float2(length(geometry.zw), length(shape.xy));\n"
    "    const uint3 binding = quads.Load<uint4>(base + 80).xyz;\n"
    "    const float2 corner = float2((0x1A >> vertex) & 1, (0x34 >> vertex) & 1);\n"
    "    const float grow = (binding.z & 1) != 0 ? 1.0 : 0.0;\n"
    "    const float2 local = corner * size + (corner * 2.0 - 1.0) * grow;\n"
    "    const float2 along = local / max(size, 0.000001);\n"
    "    const float2 pixel = geometry.xy + along.x * geometry.zw + along.y * shape.xy;\n"
    "    VertexOut output;\n"
    "    output.Position = float4(pixel.x / Canvas.Target.x * 2.0 - 1.0,\n"
    "                             1.0 - pixel.y / Canvas.Target.y * 2.0, 0.0, 1.0);\n"
    "    output.Local = local;\n"
    "    output.UV = lerp(uv.xy, uv.zw, along);\n"
    "    output.Color = quads.Load<float4>(base + 48);\n"
    "    output.Clip = quads.Load<float4>(base + 64);\n"
    "    output.Shape = float4(size, shape.zw);\n"
    "    output.Binding = binding;\n"
    "    return output;\n"
    "}\n"
    "float RoundedBox(float2 p, float2 half_size, float radius)\n"
    "{\n"
    "    const float2 q = abs(p) - half_size + radius;\n"
    "    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;\n"
    "}\n"
    "float4 MainPS(VertexOut input) : SV_Target\n"
    "{\n"
    "    Texture2D<float4> image = "
    "ResourceDescriptorHeap[NonUniformResourceIndex(input.Binding.x)];\n"
    "    SamplerState filter = SamplerDescriptorHeap[NonUniformResourceIndex(input.Binding.y)];\n"
    "    const float4 texel = image.Sample(filter, input.UV);\n"
    "    const float4 color =\n"
    "        (input.Binding.z & 2) != 0 ? input.Color * texel.r : texel * input.Color;\n"
    "    float coverage = 1.0;\n"
    "    if ((input.Binding.z & 1) != 0)\n"
    "    {\n"
    "        const float2 half_size = input.Shape.xy * 0.5;\n"
    "        const float radius = min(input.Shape.z, min(half_size.x, half_size.y));\n"
    "        float edge = RoundedBox(input.Local - half_size, half_size, radius);\n"
    "        if (input.Shape.w > 0.0)\n"
    "        {\n"
    "            edge = abs(edge + input.Shape.w * 0.5) - input.Shape.w * 0.5;\n"
    "        }\n"
    "        coverage = saturate(0.5 - edge);\n"
    "    }\n"
    "    const float2 position = input.Position.xy;\n"
    "    const bool inside = all(position >= input.Clip.xy) && all(position < input.Clip.zw);\n"
    "    return color * (inside ? coverage : 0.0);\n"
    "}\n";

/* ---------------------------------------------------------------------------
 * Images.
 * ------------------------------------------------------------------------- */

/* Copy RGBA pixels into a region of an image's texture, premultiplied as the
 * image keeps them, and wait for nothing: the copy is submitted before any
 * frame that draws it. */
static void tb_image_upload(tb_renderer *renderer, tb_image *image, tb_rhi_rect region,
                            const uint8_t *pixels, bool first)
{
    const size_t row = (size_t)region.width * 4u;
    const size_t pitch = (row + TB_RHI_TEXTURE_COPY_PITCH_ALIGNMENT - 1u) &
                         ~(size_t)(TB_RHI_TEXTURE_COPY_PITCH_ALIGNMENT - 1u);
    uint8_t *staged = tb_calloc(region.height, pitch);
    tb_rhi_buffer staging = {0};
    tb_rhi_command_list *list = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    for (uint32_t y = 0; pixels != NULL && y < region.height; y++)
    {
        memcpy(&staged[pitch * y], &pixels[row * y], row);
        if (!image->premultiplied)
        {
            tb_image_premultiply(&staged[pitch * y], region.width);
        }
    }
    if (tb_rhi_buffer_create(renderer->device,
                             &(tb_rhi_buffer_desc){.size = (uint64_t)pitch * region.height,
                                                   .memory = TB_RHI_MEMORY_UPLOAD,
                                                   .name = "image pixels"},
                             &staging, &error) != TB_RHI_OK)
    {
        tb_fatal("renderer: no memory for an image's pixels: %s", error.message);
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
    tb_image_upload(renderer, image, (tb_rhi_rect){0, 0, desc->width, desc->height}, pixels, true);
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
    tb_image_upload(renderer, image, region, pixels, false);
}

void tb_image_destroy(tb_renderer *renderer, tb_image *image)
{
    TB_ASSERT(renderer != NULL);
    if (image == NULL)
    {
        return;
    }
    tb_rhi_texture_destroy(renderer->device, image->texture);
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
    renderer->canvas.view = *view;
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
    if ((renderer->info.features & TB_RHI_FEATURE_BINDLESS) == 0)
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
    return tb_renderer_pipeline(renderer, "/Engine/Private/Canvas.hlsl", tb_canvas_source, &desc,
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

static void tb_renderer_record_canvas(const tb_render_graph_pass_context *pass)
{
    const tb_canvas_recording *const recording = pass->context;
    const tb_renderer *const renderer = recording->renderer;
    const struct
    {
        float target[2];
        uint32_t quads;
        uint32_t offset;
    } constants = {{(float)renderer->width, (float)renderer->height},
                   tb_rhi_buffer_index(renderer->device, renderer->frames[renderer->slot].upload),
                   (uint32_t)recording->offset};

    tb_rhi_cmd_pipeline(pass->list, renderer->canvas_pipeline);
    tb_rhi_cmd_push_constants(pass->list, &constants, sizeof(constants));
    tb_rhi_cmd_draw(pass->list, TB_CANVAS_QUAD_VERTICES, recording->count, 0, 0);
}

void tb_renderer_canvas_record(tb_renderer *renderer, tb_canvas_recording *recording,
                               tb_render_graph_texture image)
{
    const size_t count = renderer->canvas.count;
    const tb_canvas_quad *quads = NULL;
    size_t fit = 0;
    tb_render_graph_pass pass = {0};

    renderer->canvas_stats = (tb_canvas_stats){0};
    *recording = (tb_canvas_recording){.renderer = renderer};
    if (count == 0)
    {
        return;
    }
    if (renderer->canvas_pipeline.generation == 0)
    {
        renderer->canvas_stats.dropped = count;
        return;
    }
    quads = tb_canvas_batch_sort(&renderer->canvas);
    /* What does not fit in the frame's uploads is not drawn: the last in
     * order, the highest layers'. */
    fit = tb_renderer_upload_room(renderer, TB_CANVAS_QUAD_ALIGNMENT) / sizeof(*quads);
    fit = fit < count ? fit : count;
    renderer->canvas_stats.dropped = count - fit;
    if (fit == 0)
    {
        return;
    }
    recording->offset =
        tb_renderer_upload(renderer, quads, fit * sizeof(*quads), TB_CANVAS_QUAD_ALIGNMENT);
    TB_ASSERT_MSG(recording->offset <= UINT32_MAX, "the frame's uploads within 4 GiB");
    recording->count = (uint32_t)fit;
    /* Glyphs the atlas could not hold are drawn with no color: not drawn. */
    renderer->canvas_stats.glyphs = renderer->glyph_count - renderer->glyphs_hidden;
    renderer->canvas_stats.quads =
        fit > renderer->glyphs_hidden ? fit - renderer->glyphs_hidden : 0;
    renderer->canvas_stats.dropped += renderer->glyphs_hidden;
    renderer->canvas_stats.draws = 1;
    pass = (tb_render_graph_pass){
        .name = "canvas", .execute = tb_renderer_record_canvas, .context = recording};
    pass.colors[0] = (tb_render_graph_color){.texture = image, .load = TB_RHI_LOAD_KEEP};
    pass.color_count = 1;
    tb_render_graph_add_pass(renderer->graph, &pass);
}
