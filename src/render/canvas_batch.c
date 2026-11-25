/* The canvas's batch: views from the canvas's units to the frame's pixels,
 * each quad placed in pixels, and the quads ordered by layer. Nothing here
 * touches the GPU. */
#include "canvas_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/sort.h"

#include <math.h>

/* Keys put layers in order as unsigned numbers: the sign bit flipped. */
#define TB_CANVAS_LAYER_BIAS 0x80000000u
/* Quads the batch holds before it first grows. */
#define TB_CANVAS_FIRST_CAPACITY 1024u

tb_canvas_view tb_canvas_view_pixels(void)
{
    return (tb_canvas_view){.scale = 1.0f};
}

tb_canvas_view tb_canvas_view_fit(uint32_t frame_width, uint32_t frame_height, float width,
                                  float height, bool whole_pixels)
{
    tb_canvas_view view = {0};
    float scale = 0.0f;
    float shown_width = 0.0f;
    float shown_height = 0.0f;

    TB_ASSERT_MSG(width > 0.0f && height > 0.0f, "a view of some size");
    scale = fminf((float)frame_width / width, (float)frame_height / height);
    if (whole_pixels)
    {
        scale = fmaxf(floorf(scale), 1.0f);
    }
    if (!(scale > 0.0f))
    {
        scale = 1.0f; /* a frame of no size */
    }
    shown_width = width * scale;
    shown_height = height * scale;
    view.scale = scale;
    view.origin.x = ((float)frame_width - shown_width) * 0.5f;
    view.origin.y = ((float)frame_height - shown_height) * 0.5f;
    if (whole_pixels)
    {
        view.origin.x = floorf(view.origin.x);
        view.origin.y = floorf(view.origin.y);
    }
    view.clip.x = (int32_t)floorf(view.origin.x);
    view.clip.y = (int32_t)floorf(view.origin.y);
    view.clip.width = (uint32_t)ceilf(shown_width);
    view.clip.height = (uint32_t)ceilf(shown_height);
    return view;
}

tb_canvas_view tb_canvas_view_viewport(const tb_viewport *viewport)
{
    tb_canvas_view view = {0};

    TB_ASSERT(viewport != NULL);
    view.origin = (tb_vec2){(float)viewport->x, (float)viewport->y};
    view.scale = tb_viewport_uniform_scale(viewport);
    if (viewport->scale.x != viewport->scale.y)
    {
        view.stretch = viewport->scale;
    }
    view.clip = (tb_rhi_rect){(int32_t)viewport->x, (int32_t)viewport->y, viewport->width,
                              viewport->height};
    if (view.clip.width == 0 || view.clip.height == 0)
    {
        /* A frame of no size: nothing drawn is seen, as the whole frame. */
        view.clip = (tb_rhi_rect){0};
    }
    return view;
}

/* A view's pixels a unit across and down. */
static tb_vec2 tb_canvas_axes(const tb_canvas_view *view)
{
    if (view->stretch.x > 0.0f && view->stretch.y > 0.0f)
    {
        return view->stretch;
    }
    return (tb_vec2){view->scale, view->scale};
}

tb_vec2 tb_canvas_view_to_pixels(const tb_canvas_view *view, tb_vec2 point)
{
    TB_ASSERT(view != NULL);
    const tb_vec2 axes = tb_canvas_axes(view);
    return (tb_vec2){view->origin.x + point.x * axes.x, view->origin.y + point.y * axes.y};
}

tb_vec2 tb_canvas_view_from_pixels(const tb_canvas_view *view, tb_vec2 pixel)
{
    TB_ASSERT(view != NULL && view->scale > 0.0f);
    const tb_vec2 axes = tb_canvas_axes(view);
    return (tb_vec2){(pixel.x - view->origin.x) / axes.x, (pixel.y - view->origin.y) / axes.y};
}

/* A vector turned clockwise on a screen whose y points down. */
static tb_vec2 tb_canvas_turn(tb_vec2 vector, float cosine, float sine)
{
    return (tb_vec2){vector.x * cosine - vector.y * sine, vector.x * sine + vector.y * cosine};
}

tb_canvas_quad tb_canvas_quad_of(const tb_canvas_shape *shape, const tb_canvas_view *view,
                                 uint32_t width, uint32_t height)
{
    const float scale = view->scale;
    const tb_vec2 axes = tb_canvas_axes(view);
    const float cosine = cosf(shape->rotation);
    const float sine = sinf(shape->rotation);
    const float across = fabsf(shape->size.x);
    const float down = fabsf(shape->size.y);
    const tb_vec2 offset =
        tb_canvas_turn((tb_vec2){-shape->pivot.x * across, -shape->pivot.y * down}, cosine, sine);
    const tb_vec2 top = tb_canvas_turn((tb_vec2){across, 0.0f}, cosine, sine);
    const tb_vec2 left = tb_canvas_turn((tb_vec2){0.0f, down}, cosine, sine);
    tb_canvas_quad quad = {0};

    quad.corner[0] = view->origin.x + (shape->position.x + offset.x) * axes.x;
    quad.corner[1] = view->origin.y + (shape->position.y + offset.y) * axes.y;
    quad.axis_x[0] = top.x * axes.x;
    quad.axis_x[1] = top.y * axes.y;
    quad.axis_y[0] = left.x * axes.x;
    quad.axis_y[1] = left.y * axes.y;
    quad.radius = shape->radius * scale;
    quad.thickness = shape->thickness * scale;
    /* A negative size mirrors the image across the quad. */
    quad.uv[0] = shape->size.x < 0.0f ? shape->uv[2] : shape->uv[0];
    quad.uv[2] = shape->size.x < 0.0f ? shape->uv[0] : shape->uv[2];
    quad.uv[1] = shape->size.y < 0.0f ? shape->uv[3] : shape->uv[1];
    quad.uv[3] = shape->size.y < 0.0f ? shape->uv[1] : shape->uv[3];
    quad.color[0] = shape->color.x * shape->color.w;
    quad.color[1] = shape->color.y * shape->color.w;
    quad.color[2] = shape->color.z * shape->color.w;
    quad.color[3] = shape->color.w;
    if (view->clip.width == 0 || view->clip.height == 0)
    {
        quad.clip[2] = (float)width;
        quad.clip[3] = (float)height;
    }
    else
    {
        quad.clip[0] = (float)view->clip.x;
        quad.clip[1] = (float)view->clip.y;
        quad.clip[2] = (float)view->clip.x + (float)view->clip.width;
        quad.clip[3] = (float)view->clip.y + (float)view->clip.height;
    }
    quad.texture = shape->texture;
    quad.sampler = shape->sampler;
    quad.flags = shape->flags;
    return quad;
}

void tb_canvas_batch_begin(tb_canvas_batch *batch, uint32_t width, uint32_t height)
{
    TB_ASSERT(batch != NULL);
    batch->count = 0;
    batch->layer = 0;
    batch->view = tb_canvas_view_pixels();
    batch->width = width;
    batch->height = height;
}

void tb_canvas_batch_free(tb_canvas_batch *batch)
{
    TB_ASSERT(batch != NULL);
    tb_free(batch->quads);
    tb_free(batch->keys);
    tb_free(batch->sorted);
    *batch = (tb_canvas_batch){0};
}

bool tb_canvas_batch_add(tb_canvas_batch *batch, const tb_canvas_shape *shape)
{
    TB_ASSERT(batch != NULL && shape != NULL);
    if (shape->size.x == 0.0f || shape->size.y == 0.0f || !(shape->color.w > 0.0f))
    {
        return false;
    }
    TB_ASSERT_MSG(batch->count < UINT32_MAX, "fewer than 2^32 quads a frame");
    if (batch->count == batch->capacity)
    {
        batch->capacity = batch->capacity == 0 ? TB_CANVAS_FIRST_CAPACITY : batch->capacity * 2;
        batch->quads = tb_realloc_array(batch->quads, batch->capacity, sizeof(*batch->quads));
        batch->keys = tb_realloc_array(batch->keys, batch->capacity, sizeof(*batch->keys));
    }
    batch->quads[batch->count] =
        tb_canvas_quad_of(shape, &batch->view, batch->width, batch->height);
    batch->keys[batch->count] =
        (uint64_t)((uint32_t)batch->layer ^ TB_CANVAS_LAYER_BIAS) << 32 | (uint64_t)batch->count;
    batch->count++;
    return true;
}

static int tb_canvas_key_compare(const void *left, const void *right, void *context)
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;

    (void)context;
    if (a < b)
    {
        return -1;
    }
    return a > b ? 1 : 0;
}

const tb_canvas_quad *tb_canvas_batch_sort(tb_canvas_batch *batch)
{
    TB_ASSERT(batch != NULL);
    /* Every key differs, by the quad's place, so the order is total. */
    tb_sort(batch->keys, batch->count, sizeof(*batch->keys), tb_canvas_key_compare, NULL);
    if (batch->sorted_capacity < batch->count)
    {
        batch->sorted_capacity = batch->capacity;
        tb_free(batch->sorted);
        batch->sorted = tb_calloc(batch->sorted_capacity, sizeof(*batch->sorted));
    }
    for (size_t index = 0; index < batch->count; index++)
    {
        batch->sorted[index] = batch->quads[(uint32_t)batch->keys[index]];
    }
    return batch->sorted;
}
