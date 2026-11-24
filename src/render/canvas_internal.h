/* The canvas's batch of quads, apart from the GPU: what each quad is in the
 * frame's pixels, and their order by layer. renderer.c and canvas.c draw
 * the batch; tests read it. */
#ifndef TB_CANVAS_INTERNAL_H
#define TB_CANVAS_INTERNAL_H

#include "badger/canvas.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A quad's edges are anti-aliased over a pixel, measured from its shape. */
#define TB_CANVAS_QUAD_SMOOTH 1u
/* A quad of text: its image's first channel is coverage, which multiplies
 * its color. */
#define TB_CANVAS_QUAD_COVERAGE 2u

/* One quad in the frame's pixels, as the canvas shader reads it from a
 * ByteAddressBuffer: six float4s. Pixels of the quad are
 * corner + u * axis_x + v * axis_y for u and v from 0 to 1. */
typedef struct tb_canvas_quad
{
    float corner[2];  /* the pixel of its top left, before turning */
    float axis_x[2];  /* its top edge, in pixels */
    float axis_y[2];  /* its left edge, in pixels */
    float radius;     /* of its corners, in pixels */
    float thickness;  /* of its outline, in pixels; 0 fills it */
    float uv[4];      /* the texture coordinates of its top left and bottom right */
    float color[4];   /* premultiplied */
    float clip[4];    /* the pixels it may draw: left, top, right, bottom */
    uint32_t texture; /* the bindless index of its image */
    uint32_t sampler; /* the bindless index of its sampler */
    uint32_t flags;   /* TB_CANVAS_QUAD_ bits */
    uint32_t unused;
} tb_canvas_quad;

/* A quad as a caller describes it, in the canvas's units. */
typedef struct tb_canvas_shape
{
    tb_vec2 position; /* where its pivot is */
    tb_vec2 size;     /* across and down; negative mirrors */
    tb_vec2 pivot;    /* a fraction of its size */
    float rotation;   /* radians, clockwise on the screen */
    float radius;     /* of its corners */
    float thickness;  /* of its outline; 0 fills */
    tb_vec4 color;    /* straight alpha */
    float uv[4];
    uint32_t texture;
    uint32_t sampler;
    uint32_t flags;
} tb_canvas_shape;

/* A frame's quads, in the order they were drawn, and their layers. */
typedef struct tb_canvas_batch
{
    tb_canvas_quad *quads;
    uint64_t *keys; /* each quad's layer, then its place: the order to draw them */
    size_t count;
    size_t capacity;
    tb_canvas_quad *sorted; /* the quads in order, after tb_canvas_batch_sort() */
    size_t sorted_capacity;
    int32_t layer;
    tb_canvas_view view;
    uint32_t width; /* of the frame, in pixels */
    uint32_t height;
} tb_canvas_batch;

/* Empty the batch for a frame of a size, in pixels, on layer 0 in pixels. */
void tb_canvas_batch_begin(tb_canvas_batch *batch, uint32_t width, uint32_t height);

/* Release what the batch holds. */
void tb_canvas_batch_free(tb_canvas_batch *batch);

/* Add a shape in the batch's view, on its layer. A shape of no size, or no
 * color, adds nothing. Returns whether it added a quad. */
bool tb_canvas_batch_add(tb_canvas_batch *batch, const tb_canvas_shape *shape);

/* The quads in the order to draw them: by layer, then as they were added.
 * Good until the batch changes. */
const tb_canvas_quad *tb_canvas_batch_sort(tb_canvas_batch *batch);

/* Make the quad of a shape in a view, for a frame of a size. */
tb_canvas_quad tb_canvas_quad_of(const tb_canvas_shape *shape, const tb_canvas_view *view,
                                 uint32_t width, uint32_t height);

/* Premultiply `count` RGBA pixels by their alpha, rounding to nearest. */
void tb_image_premultiply(uint8_t *pixels, size_t count);

#endif
