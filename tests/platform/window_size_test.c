/* The arithmetic of a window's limits, which every backend applies: a
 * windowed size is raised to its least size and fitted to its aspect, the
 * room on the display winning when they disagree; and a frame dragged by an
 * edge or a corner keeps its client area's aspect, moving only the edges
 * that the drag and the aspect call for. Windows applies the drag in
 * WM_SIZING; checking it here needs no window. */
#include "platform/shared/window.h"

#include "tb_test.h"

/* Room enough for any size the cases ask for. */
#define ROOMY 10000u

static void fit(const tb_window_options *options, uint32_t room_width, uint32_t room_height,
                uint32_t width, uint32_t height, uint32_t expected_width, uint32_t expected_height)
{
    uint32_t fitted_width = width;
    uint32_t fitted_height = height;

    TB_TEST_CONTEXT("%ux%u in %ux%u", width, height, room_width, room_height);
    tb_window_fit_size(options, room_width, room_height, &fitted_width, &fitted_height);
    TB_TEST_EXPECT_UINT(fitted_width, ==, expected_width);
    TB_TEST_EXPECT_UINT(fitted_height, ==, expected_height);
}

TB_TEST(platform_window_size, the_least_size_is_the_minimum_or_the_smallest)
{
    uint32_t width = 0;
    uint32_t height = 0;

    tb_window_least_size(&(tb_window_options){0}, &width, &height);
    TB_TEST_EXPECT_UINT(width, ==, TB_WINDOW_SMALLEST);
    TB_TEST_EXPECT_UINT(height, ==, TB_WINDOW_SMALLEST);
    tb_window_least_size(&(tb_window_options){.min_width = 640, .min_height = 360}, &width,
                         &height);
    TB_TEST_EXPECT_UINT(width, ==, 640);
    TB_TEST_EXPECT_UINT(height, ==, 360);
    /* A minimum under the smallest is the smallest. */
    tb_window_least_size(&(tb_window_options){.min_width = 10, .min_height = 1000}, &width,
                         &height);
    TB_TEST_EXPECT_UINT(width, ==, TB_WINDOW_SMALLEST);
    TB_TEST_EXPECT_UINT(height, ==, 1000);
}

/* Only a resizable windowed window keeps an aspect, and only one it was
 * given whole. */
TB_TEST(platform_window_size, only_a_resizable_window_keeps_an_aspect)
{
    TB_TEST_EXPECT(
        tb_window_wants_aspect(&(tb_window_options){.aspect_width = 16, .aspect_height = 9}));
    TB_TEST_EXPECT(!tb_window_wants_aspect(&(tb_window_options){0}));
    TB_TEST_EXPECT(!tb_window_wants_aspect(&(tb_window_options){.aspect_width = 16}));
    TB_TEST_EXPECT(!tb_window_wants_aspect(&(tb_window_options){.aspect_height = 9}));
    TB_TEST_EXPECT(!tb_window_wants_aspect(
        &(tb_window_options){.aspect_width = 16, .aspect_height = 9, .fixed_size = true}));
    TB_TEST_EXPECT(!tb_window_wants_aspect(&(tb_window_options){
        .mode = TB_WINDOW_MODE_BORDERLESS, .aspect_width = 16, .aspect_height = 9}));
}

/* A size under the minimum is raised to it, one over the room is lowered to
 * it, and the room wins over the minimum. */
TB_TEST(platform_window_size, a_size_is_raised_to_its_minimum_within_the_room)
{
    const tb_window_options options = {.min_width = 640, .min_height = 360};

    fit(&options, ROOMY, ROOMY, 320, 180, 640, 360);
    fit(&options, ROOMY, ROOMY, 1280, 720, 1280, 720);
    fit(&options, ROOMY, ROOMY, 1280, 200, 1280, 360);
    fit(&options, 1000, 500, 2000, 2000, 1000, 500);
    fit(&options, 600, 300, 320, 180, 600, 300);
    /* Without a minimum, the smallest. */
    fit(&(tb_window_options){0}, ROOMY, ROOMY, 1, 1, TB_WINDOW_SMALLEST, TB_WINDOW_SMALLEST);
}

/* With an aspect, the width decides the height; a height under the least
 * size raises the width, and a display too small lowers both. */
TB_TEST(platform_window_size, a_size_is_fitted_to_its_aspect)
{
    const tb_window_options options = {
        .min_width = 640, .min_height = 360, .aspect_width = 16, .aspect_height = 9};

    fit(&options, ROOMY, ROOMY, 1280, 1000, 1280, 720);
    fit(&options, ROOMY, ROOMY, 800, 400, 800, 450);
    /* Rounded to the nearest whole unit. */
    fit(&options, ROOMY, ROOMY, 1001, 100, 1001, 563);
    /* 640 wide is 360 high: the least size is of the aspect. */
    fit(&options, ROOMY, ROOMY, 100, 100, 640, 360);
    /* A taller least size widens the window. */
    fit(&(tb_window_options){.min_height = 450, .aspect_width = 16, .aspect_height = 9}, ROOMY,
        ROOMY, 320, 180, 800, 450);
    /* A display too narrow or too short for the size. */
    fit(&options, 1000, ROOMY, 1600, 900, 1000, 563);
    fit(&options, ROOMY, 540, 1600, 900, 960, 540);
    /* A display too small for the least size keeps the aspect within it. */
    fit(&options, 500, 500, 100, 100, 500, 281);
    /* Any aspect, such as 4:3 and a portrait 9:16. */
    fit(&(tb_window_options){.aspect_width = 4, .aspect_height = 3}, ROOMY, ROOMY, 800, 800, 800,
        600);
    fit(&(tb_window_options){.aspect_width = 9, .aspect_height = 16}, ROOMY, ROOMY, 450, 450, 450,
        800);
    /* A fixed size keeps the size it asked for. */
    fit(&(tb_window_options){.aspect_width = 16, .aspect_height = 9, .fixed_size = true}, ROOMY,
        ROOMY, 800, 800, 800, 800);
}

typedef struct drag_case
{
    const char *name;
    tb_window_edge edge;
    tb_window_rect proposed;
    tb_window_rect expected;
} drag_case;

/* A frame with a border of 16 by 39 pixels, as Windows draws one at 100%,
 * dragged to a client area of 1000 by 500 from one of 800 by 450 whose
 * corner was at 100, 100. Every case keeps 16:9. */
TB_TEST(platform_window_size, a_dragged_frame_keeps_its_aspect)
{
    static const drag_case cases[] = {
        /* A side sets the width, 1000, which calls for a height of 563; the
         * bottom follows. */
        {"right", TB_WINDOW_EDGE_RIGHT, {100, 100, 1116, 639}, {100, 100, 1116, 702}},
        {"left", TB_WINDOW_EDGE_LEFT, {100, 100, 1116, 639}, {100, 100, 1116, 702}},
        /* The top or the bottom sets the height, 500, which calls for a
         * width of 889; the right side follows. */
        {"bottom", TB_WINDOW_EDGE_BOTTOM, {100, 100, 1116, 639}, {100, 100, 1005, 639}},
        {"top", TB_WINDOW_EDGE_TOP, {100, 100, 1116, 639}, {100, 100, 1005, 639}},
        /* A corner follows the edge that makes the larger window: here the
         * width. The corner across stays. */
        {"bottom right", TB_WINDOW_EDGE_BOTTOM_RIGHT, {100, 100, 1116, 639}, {100, 100, 1116, 702}},
        {"top left", TB_WINDOW_EDGE_TOP_LEFT, {100, 100, 1116, 639}, {100, 37, 1116, 639}},
        {"top right", TB_WINDOW_EDGE_TOP_RIGHT, {100, 100, 1116, 639}, {100, 37, 1116, 639}},
        {"bottom left", TB_WINDOW_EDGE_BOTTOM_LEFT, {100, 100, 1116, 639}, {100, 100, 1116, 702}},
        /* A corner dragged taller than wide follows the height: 900 high is
         * 1600 wide. */
        {"bottom right, tall",
         TB_WINDOW_EDGE_BOTTOM_RIGHT,
         {100, 100, 916, 1039},
         {100, 100, 1716, 1039}},
        {"top left, tall", TB_WINDOW_EDGE_TOP_LEFT, {100, 100, 916, 1039}, {-700, 100, 916, 1039}},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        tb_window_rect frame = cases[index].proposed;
        TB_TEST_CONTEXT("%s", cases[index].name);
        tb_window_drag_to_aspect(&frame, cases[index].edge, 16, 39, 16, 9, 64, 64);
        TB_TEST_EXPECT_INT(frame.left, ==, cases[index].expected.left);
        TB_TEST_EXPECT_INT(frame.top, ==, cases[index].expected.top);
        TB_TEST_EXPECT_INT(frame.right, ==, cases[index].expected.right);
        TB_TEST_EXPECT_INT(frame.bottom, ==, cases[index].expected.bottom);
    }
}

/* A frame dragged under the least size stops at it, still of its aspect,
 * the edge across from the drag in place; a frame dragged inside out is the
 * least size too. */
TB_TEST(platform_window_size, a_dragged_frame_stops_at_its_least_size)
{
    tb_window_rect frame = {100, 100, 216, 139};

    tb_window_drag_to_aspect(&frame, TB_WINDOW_EDGE_RIGHT, 16, 39, 16, 9, 640, 360);
    TB_TEST_EXPECT_INT(frame.left, ==, 100);
    TB_TEST_EXPECT_INT(frame.top, ==, 100);
    TB_TEST_EXPECT_INT(frame.right, ==, 100 + 640 + 16);
    TB_TEST_EXPECT_INT(frame.bottom, ==, 100 + 360 + 39);

    /* A least size taller than the aspect raises the height, and the width
     * with it. */
    frame = (tb_window_rect){100, 100, 216, 139};
    tb_window_drag_to_aspect(&frame, TB_WINDOW_EDGE_TOP_LEFT, 16, 39, 16, 9, 64, 450);
    TB_TEST_EXPECT_INT(frame.right, ==, 216);
    TB_TEST_EXPECT_INT(frame.bottom, ==, 139);
    TB_TEST_EXPECT_INT(frame.left, ==, 216 - 800 - 16);
    TB_TEST_EXPECT_INT(frame.top, ==, 139 - 450 - 39);

    frame = (tb_window_rect){500, 500, 100, 100};
    tb_window_drag_to_aspect(&frame, TB_WINDOW_EDGE_BOTTOM_RIGHT, 16, 39, 4, 3, 64, 48);
    TB_TEST_EXPECT_INT(frame.left, ==, 500);
    TB_TEST_EXPECT_INT(frame.top, ==, 500);
    TB_TEST_EXPECT_INT(frame.right, ==, 500 + 64 + 16);
    TB_TEST_EXPECT_INT(frame.bottom, ==, 500 + 48 + 39);
}

/* A frame at the far edge of the coordinates is held within them, not
 * wrapped around. */
TB_TEST(platform_window_size, a_dragged_frame_stays_within_the_coordinates)
{
    tb_window_rect frame = {INT32_MAX - 100, INT32_MAX - 50, INT32_MAX, INT32_MAX};

    tb_window_drag_to_aspect(&frame, TB_WINDOW_EDGE_BOTTOM_RIGHT, 0, 0, 1, 1000, 1, 1);
    TB_TEST_EXPECT_INT(frame.right, ==, INT32_MAX);
    TB_TEST_EXPECT_INT(frame.bottom, ==, INT32_MAX);

    frame = (tb_window_rect){INT32_MIN, INT32_MIN + 10, INT32_MIN + 100, INT32_MIN + 20};
    tb_window_drag_to_aspect(&frame, TB_WINDOW_EDGE_TOP_LEFT, 0, 0, 1000, 1, 1, 1);
    TB_TEST_EXPECT_INT(frame.left, ==, INT32_MIN);
    TB_TEST_EXPECT_INT(frame.top, ==, INT32_MIN + 10);
}
