/* The canvas's batch, apart from the GPU: each quad lands on the pixels its
 * view puts it at, turned clockwise on a screen whose y points down;
 * quads are ordered by layer, then as they were drawn; and colors and
 * images are premultiplied as the canvas's blending needs. */
#include "render/canvas_internal.h"

#include "tb_test.h"

static tb_canvas_shape solid(tb_vec2 position, tb_vec2 size, tb_vec4 color)
{
    tb_canvas_shape shape = {0};
    shape.position = position;
    shape.size = size;
    shape.color = color;
    shape.uv[2] = 1.0f;
    shape.uv[3] = 1.0f;
    return shape;
}

/* A view's origin and scale apply to a quad's corner and edges alike; the
 * pivot is where the position is. */
TB_TEST(canvas_batch, a_quad_lands_where_its_view_and_pivot_put_it)
{
    const tb_canvas_view view = {.origin = {1, 1}, .scale = 2.0f};
    tb_canvas_shape shape = solid((tb_vec2){10, 20}, (tb_vec2){4, 2}, (tb_vec4){1, 1, 1, 1});
    tb_canvas_quad quad = {0};

    shape.pivot = (tb_vec2){0.5f, 0.5f};
    shape.radius = 1.5f;
    shape.thickness = 0.5f;
    quad = tb_canvas_quad_of(&shape, &view, 100, 50);
    TB_TEST_EXPECT_FLOAT(quad.corner[0], ==, 17.0f);
    TB_TEST_EXPECT_FLOAT(quad.corner[1], ==, 39.0f);
    TB_TEST_EXPECT_FLOAT(quad.axis_x[0], ==, 8.0f);
    TB_TEST_EXPECT_FLOAT(quad.axis_x[1], ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(quad.axis_y[0], ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(quad.axis_y[1], ==, 4.0f);
    /* Radii and outlines are in the view's units too. */
    TB_TEST_EXPECT_FLOAT(quad.radius, ==, 3.0f);
    TB_TEST_EXPECT_FLOAT(quad.thickness, ==, 1.0f);
    /* No clip of the view's own is the whole frame. */
    TB_TEST_EXPECT_FLOAT(quad.clip[0], ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(quad.clip[1], ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(quad.clip[2], ==, 100.0f);
    TB_TEST_EXPECT_FLOAT(quad.clip[3], ==, 50.0f);
}

/* A stretched view places a quad's corner and edges by its scale each
 * way, and keeps its radii and outlines at the view's scale. */
TB_TEST(canvas_batch, a_stretched_view_stretches_edges_but_not_radii)
{
    const tb_canvas_view view = {.origin = {0, 0}, .scale = 1.0f, .stretch = {2.0f, 1.0f}};
    tb_canvas_shape shape = solid((tb_vec2){10, 20}, (tb_vec2){4, 2}, (tb_vec4){1, 1, 1, 1});
    tb_canvas_quad quad = {0};

    shape.radius = 1.5f;
    quad = tb_canvas_quad_of(&shape, &view, 100, 50);
    TB_TEST_EXPECT_FLOAT(quad.corner[0], ==, 20.0f);
    TB_TEST_EXPECT_FLOAT(quad.corner[1], ==, 20.0f);
    TB_TEST_EXPECT_FLOAT(quad.axis_x[0], ==, 8.0f);
    TB_TEST_EXPECT_FLOAT(quad.axis_y[1], ==, 2.0f);
    TB_TEST_EXPECT_FLOAT(quad.radius, ==, 1.5f);
}

/* A quarter turn is clockwise on the screen: the top edge points down and
 * the left edge points left, around the pivot. */
TB_TEST(canvas_batch, a_positive_rotation_turns_clockwise_on_the_screen)
{
    const tb_canvas_view view = tb_canvas_view_pixels();
    tb_canvas_shape shape = solid((tb_vec2){50, 50}, (tb_vec2){20, 10}, (tb_vec4){1, 1, 1, 1});
    tb_canvas_quad quad = {0};

    shape.rotation = TB_PI * 0.5f;
    quad = tb_canvas_quad_of(&shape, &view, 100, 100);
    TB_TEST_EXPECT_FLOAT_NEAR(quad.corner[0], 50.0f, 1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(quad.corner[1], 50.0f, 1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(quad.axis_x[0], 0.0f, 1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(quad.axis_x[1], 20.0f, 1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(quad.axis_y[0], -10.0f, 1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(quad.axis_y[1], 0.0f, 1e-4f);

    /* Turning around its middle keeps the middle where it was. */
    shape.pivot = (tb_vec2){0.5f, 0.5f};
    quad = tb_canvas_quad_of(&shape, &view, 100, 100);
    TB_TEST_EXPECT_FLOAT_NEAR(quad.corner[0] + (quad.axis_x[0] + quad.axis_y[0]) * 0.5f, 50.0f,
                              1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(quad.corner[1] + (quad.axis_x[1] + quad.axis_y[1]) * 0.5f, 50.0f,
                              1e-4f);
}

/* A negative size mirrors the image: the texture coordinates swap, and the
 * quad keeps a positive size. Colors are premultiplied. */
TB_TEST(canvas_batch, a_negative_size_mirrors_and_colors_are_premultiplied)
{
    const tb_canvas_view view = tb_canvas_view_pixels();
    tb_canvas_shape shape = solid((tb_vec2){0, 0}, (tb_vec2){-8, -4}, (tb_vec4){1, 0.5f, 0, 0.5f});
    tb_canvas_quad quad = {0};

    shape.uv[0] = 0.25f;
    shape.uv[1] = 0.5f;
    shape.uv[2] = 0.75f;
    shape.uv[3] = 1.0f;
    quad = tb_canvas_quad_of(&shape, &view, 10, 10);
    TB_TEST_EXPECT_FLOAT(quad.axis_x[0], ==, 8.0f);
    TB_TEST_EXPECT_FLOAT(quad.axis_y[1], ==, 4.0f);
    TB_TEST_EXPECT_FLOAT(quad.uv[0], ==, 0.75f);
    TB_TEST_EXPECT_FLOAT(quad.uv[2], ==, 0.25f);
    TB_TEST_EXPECT_FLOAT(quad.uv[1], ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(quad.uv[3], ==, 0.5f);
    TB_TEST_EXPECT_FLOAT(quad.color[0], ==, 0.5f);
    TB_TEST_EXPECT_FLOAT(quad.color[1], ==, 0.25f);
    TB_TEST_EXPECT_FLOAT(quad.color[2], ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(quad.color[3], ==, 0.5f);
}

/* A view's clip is in pixels, from its top left to its bottom right. */
TB_TEST(canvas_batch, a_view_clips_to_its_rectangle)
{
    const tb_canvas_view view = {.scale = 1.0f, .clip = {-5, 10, 20, 30}};
    const tb_canvas_shape shape = solid((tb_vec2){0, 0}, (tb_vec2){1, 1}, (tb_vec4){1, 1, 1, 1});
    const tb_canvas_quad quad = tb_canvas_quad_of(&shape, &view, 100, 100);

    TB_TEST_EXPECT_FLOAT(quad.clip[0], ==, -5.0f);
    TB_TEST_EXPECT_FLOAT(quad.clip[1], ==, 10.0f);
    TB_TEST_EXPECT_FLOAT(quad.clip[2], ==, 15.0f);
    TB_TEST_EXPECT_FLOAT(quad.clip[3], ==, 40.0f);
}

/* Higher layers are drawn over lower ones, negative layers under 0, and
 * quads of a layer in the order they were added. */
TB_TEST(canvas_batch, quads_are_ordered_by_layer_then_as_they_were_drawn)
{
    static const int32_t layers[] = {2, -1, 0, 2, -1, INT32_MIN, INT32_MAX, 0};
    static const float expected[] = {5, 1, 4, 2, 7, 0, 3, 6};
    tb_canvas_batch batch = {0};
    const tb_canvas_quad *sorted = NULL;
    size_t counts[TB_CANVAS_TARGET_COUNT] = {0};

    tb_canvas_batch_begin(&batch, 64, 64, 0, 0);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(layers); index++)
    {
        /* Each quad's place in the drawing order is its x. */
        const tb_canvas_shape shape =
            solid((tb_vec2){(float)index, 0}, (tb_vec2){1, 1}, (tb_vec4){1, 1, 1, 1});
        batch.layer = layers[index];
        TB_TEST_REQUIRE(tb_canvas_batch_add(&batch, &shape));
    }
    TB_TEST_REQUIRE_UINT(batch.count, ==, TB_TEST_COUNT_OF(layers));
    sorted = tb_canvas_batch_sort(&batch, counts);
    TB_TEST_EXPECT_UINT(counts[TB_CANVAS_TARGET_FRAME], ==, TB_TEST_COUNT_OF(layers));
    TB_TEST_EXPECT_UINT(counts[TB_CANVAS_TARGET_VIEWPORT], ==, 0);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected); index++)
    {
        TB_TEST_CONTEXT("place %zu", index);
        TB_TEST_EXPECT_FLOAT(sorted[index].corner[0], ==, expected[index]);
    }
    tb_canvas_batch_free(&batch);
}

/* A shape of no size or no alpha draws nothing; a batch grows past its
 * first capacity and is emptied by a new frame. */
TB_TEST(canvas_batch, empty_shapes_add_nothing_and_a_batch_grows)
{
    tb_canvas_batch batch = {0};
    const tb_canvas_quad *sorted = NULL;
    size_t counts[TB_CANVAS_TARGET_COUNT] = {0};

    tb_canvas_batch_begin(&batch, 64, 64, 0, 0);
    TB_TEST_EXPECT(
        !tb_canvas_batch_add(&batch, &(tb_canvas_shape){.size = {0, 4}, .color = {1, 1, 1, 1}}));
    TB_TEST_EXPECT(
        !tb_canvas_batch_add(&batch, &(tb_canvas_shape){.size = {4, 4}, .color = {1, 1, 1, 0}}));
    TB_TEST_EXPECT_UINT(batch.count, ==, 0);
    for (size_t index = 0; index < 3000; index++)
    {
        const tb_canvas_shape shape =
            solid((tb_vec2){(float)index, 0}, (tb_vec2){1, 1}, (tb_vec4){1, 1, 1, 1});
        TB_TEST_REQUIRE(tb_canvas_batch_add(&batch, &shape));
    }
    sorted = tb_canvas_batch_sort(&batch, counts);
    TB_TEST_EXPECT_FLOAT(sorted[2999].corner[0], ==, 2999.0f);
    tb_canvas_batch_begin(&batch, 32, 32, 0, 0);
    TB_TEST_EXPECT_UINT(batch.count, ==, 0);
    TB_TEST_EXPECT_INT(batch.layer, ==, 0);
    TB_TEST_EXPECT_FLOAT(batch.view.scale, ==, 1.0f);
    tb_canvas_batch_free(&batch);
}

/* In the viewport render mode, the viewport target's quads are drawn
 * first, into it, before it is upscaled, and the frame's after, over it:
 * the sort puts them first, each target's in layer order, whatever layers
 * the frame's are on. A quad of the target's view with no clip is clipped
 * to the target, not to the frame. */
TB_TEST(canvas_batch, the_viewport_targets_quads_come_before_the_frames)
{
    tb_canvas_batch batch = {0};
    const tb_canvas_quad *sorted = NULL;
    size_t counts[TB_CANVAS_TARGET_COUNT] = {0};
    const tb_canvas_view target = {.scale = 1.0f, .target = TB_CANVAS_TARGET_VIEWPORT};
    const tb_canvas_view frame = {.scale = 1.0f};

    tb_canvas_batch_begin(&batch, 1280, 720, 320, 180);
    /* Each quad's x is its place in the order expected. */
    tb_canvas_batch_set_view(&batch, &frame);
    batch.layer = -5;
    TB_TEST_REQUIRE(tb_canvas_batch_add(
        &batch, &(tb_canvas_shape){.position = {2, 0}, .size = {1, 1}, .color = {1, 1, 1, 1}}));
    tb_canvas_batch_set_view(&batch, &target);
    batch.layer = 7;
    TB_TEST_REQUIRE(tb_canvas_batch_add(
        &batch, &(tb_canvas_shape){.position = {1, 0}, .size = {1, 1}, .color = {1, 1, 1, 1}}));
    batch.layer = 3;
    TB_TEST_REQUIRE(tb_canvas_batch_add(
        &batch, &(tb_canvas_shape){.position = {0, 0}, .size = {1, 1}, .color = {1, 1, 1, 1}}));
    tb_canvas_batch_set_view(&batch, &frame);
    batch.layer = 0;
    TB_TEST_REQUIRE(tb_canvas_batch_add(
        &batch, &(tb_canvas_shape){.position = {3, 0}, .size = {1, 1}, .color = {1, 1, 1, 1}}));
    sorted = tb_canvas_batch_sort(&batch, counts);
    TB_TEST_EXPECT_UINT(counts[TB_CANVAS_TARGET_VIEWPORT], ==, 2);
    TB_TEST_EXPECT_UINT(counts[TB_CANVAS_TARGET_FRAME], ==, 2);
    for (size_t index = 0; index < 4; index++)
    {
        TB_TEST_CONTEXT("place %zu", index);
        TB_TEST_EXPECT_FLOAT(sorted[index].corner[0], ==, (float)index);
        TB_TEST_EXPECT_UINT(sorted[index].target, ==,
                            index < 2 ? TB_CANVAS_TARGET_VIEWPORT : TB_CANVAS_TARGET_FRAME);
    }
    TB_TEST_EXPECT_FLOAT(sorted[0].clip[2], ==, 320.0f);
    TB_TEST_EXPECT_FLOAT(sorted[0].clip[3], ==, 180.0f);
    TB_TEST_EXPECT_FLOAT(sorted[2].clip[2], ==, 1280.0f);
    TB_TEST_EXPECT_FLOAT(sorted[2].clip[3], ==, 720.0f);
    tb_canvas_batch_free(&batch);
}

/* A frame without the viewport's target, in the canvas mode, draws a view
 * of the target over the frame, as one of the frame: nothing drawn in it is
 * lost. */
TB_TEST(canvas_batch, a_view_of_a_target_there_is_not_draws_over_the_frame)
{
    tb_canvas_batch batch = {0};
    size_t counts[TB_CANVAS_TARGET_COUNT] = {0};
    const tb_canvas_view target = {.scale = 1.0f, .target = TB_CANVAS_TARGET_VIEWPORT};

    tb_canvas_batch_begin(&batch, 64, 32, 0, 0);
    tb_canvas_batch_set_view(&batch, &target);
    TB_TEST_EXPECT_INT(batch.view.target, ==, TB_CANVAS_TARGET_FRAME);
    TB_TEST_REQUIRE(
        tb_canvas_batch_add(&batch, &(tb_canvas_shape){.size = {1, 1}, .color = {1, 1, 1, 1}}));
    (void)tb_canvas_batch_sort(&batch, counts);
    TB_TEST_EXPECT_UINT(counts[TB_CANVAS_TARGET_FRAME], ==, 1);
    TB_TEST_EXPECT_UINT(counts[TB_CANVAS_TARGET_VIEWPORT], ==, 0);
    TB_TEST_EXPECT_FLOAT(batch.sorted[0].clip[2], ==, 64.0f);
    tb_canvas_batch_free(&batch);
}

/* Premultiplying rounds to the nearest: a half-transparent white is a
 * half gray, and an opaque pixel is unchanged. */
TB_TEST(canvas_batch, premultiplying_rounds_to_the_nearest)
{
    uint8_t pixels[] = {255, 128, 0, 128, 10, 20, 30, 255, 200, 200, 200, 0};

    tb_image_premultiply(pixels, 3);
    TB_TEST_EXPECT_UINT(pixels[0], ==, 128);
    TB_TEST_EXPECT_UINT(pixels[1], ==, 64);
    TB_TEST_EXPECT_UINT(pixels[2], ==, 0);
    TB_TEST_EXPECT_UINT(pixels[3], ==, 128);
    TB_TEST_EXPECT_UINT(pixels[4], ==, 10);
    TB_TEST_EXPECT_UINT(pixels[6], ==, 30);
    TB_TEST_EXPECT_UINT(pixels[8], ==, 0);
    tb_image_premultiply(NULL, 0);
}
