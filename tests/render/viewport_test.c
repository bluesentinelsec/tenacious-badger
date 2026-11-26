/* The display policy's arithmetic, by table: every aspect with each scale
 * mode, on frames of the common shapes (16:9, 16:10, 21:9, 4:3, and
 * portrait), of odd sizes, and too small for the design; without a design
 * size at the display's scales; narrowed first by the player's aspect
 * ratio; and always on whole pixels, centered. Then the conversions
 * between pixels and units, the hit test, and the policy the settings
 * describe. */
#include "badger/engine_settings.h"
#include "badger/settings.h"
#include "badger/viewport.h"

#include "tb_test.h"

#include <math.h>

/* The viewport a case expects, without the viewport render mode's target,
 * which viewport_check_targets() works out from it. */
typedef struct viewport_expected
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    tb_vec2 scale;
    tb_vec2 size;
} viewport_expected;

/* A case of the table: a policy, a frame, and the viewport it gives. */
typedef struct viewport_case
{
    const char *name;
    uint32_t design_width;
    uint32_t design_height;
    tb_viewport_aspect aspect;
    tb_viewport_scale_mode mode;
    float factor;        /* the policy's scale; 0 is 1 */
    uint32_t width;      /* the frame */
    uint32_t height;     /* the frame */
    float display_scale; /* the display's */
    float aspect_ratio;  /* the player's; 0 for none */
    viewport_expected expected;
} viewport_case;

/* Pixels a unit, as a fraction of whole numbers. */
#define FIT(pixels, units) ((float)(pixels) / (float)(units))

/* Short names for the table's columns. */
#define KEEP TB_VIEWPORT_ASPECT_KEEP
#define WIDTH TB_VIEWPORT_ASPECT_KEEP_WIDTH
#define HEIGHT TB_VIEWPORT_ASPECT_KEEP_HEIGHT
#define EXPAND TB_VIEWPORT_ASPECT_EXPAND
#define IGNORE TB_VIEWPORT_ASPECT_IGNORE
#define FRACTIONAL TB_VIEWPORT_SCALE_FRACTIONAL
#define INTEGER TB_VIEWPORT_SCALE_INTEGER

/* The engine's default design, 1920 by 1080, fractional, on frames of
 * each shape and each aspect. A kept axis shows the design's units and is
 * centered between bars; an open one fills the frame and shows more. */
static const viewport_case viewport_fractional_cases[] = {
    /* keep: letterboxed or pillarboxed, the design whole. */
    {"keep 16:9 at its size",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     1920,
     1080,
     1,
     0,
     {0, 0, 1920, 1080, {1, 1}, {1920, 1080}}},
    {"keep 16:9 smaller",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     1280,
     720,
     1,
     0,
     {0, 0, 1280, 720, {FIT(2, 3), FIT(2, 3)}, {1920, 1080}}},
    {"keep 16:10 letterboxed",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     1920,
     1200,
     1,
     0,
     {0, 60, 1920, 1080, {1, 1}, {1920, 1080}}},
    {"keep 21:9 pillarboxed",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     2560,
     1080,
     1,
     0,
     {320, 0, 1920, 1080, {1, 1}, {1920, 1080}}},
    {"keep 4:3 letterboxed",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     1024,
     768,
     1,
     0,
     {0, 96, 1024, 576, {FIT(1024, 1920), FIT(1024, 1920)}, {1920, 1080}}},
    {"keep portrait",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     1080,
     1920,
     1,
     0,
     {0, 656, 1080, 608, {FIT(1080, 1920), FIT(1080, 1920)}, {1920, 1080}}},
    /* keep_width: the width whole; taller shows more height, wider is
     * pillarboxed. */
    {"keep_width 16:9",
     1920,
     1080,
     WIDTH,
     FRACTIONAL,
     0,
     1920,
     1080,
     1,
     0,
     {0, 0, 1920, 1080, {1, 1}, {1920, 1080}}},
    {"keep_width 16:10 shows more height",
     1920,
     1080,
     WIDTH,
     FRACTIONAL,
     0,
     1920,
     1200,
     1,
     0,
     {0, 0, 1920, 1200, {1, 1}, {1920, 1200}}},
    {"keep_width 21:9 pillarboxed",
     1920,
     1080,
     WIDTH,
     FRACTIONAL,
     0,
     2560,
     1080,
     1,
     0,
     {320, 0, 1920, 1080, {1, 1}, {1920, 1080}}},
    {"keep_width 4:3 shows more height",
     1920,
     1080,
     WIDTH,
     FRACTIONAL,
     0,
     1024,
     768,
     1,
     0,
     {0, 0, 1024, 768, {FIT(1024, 1920), FIT(1024, 1920)}, {1920, 1440}}},
    {"keep_width portrait",
     1920,
     1080,
     WIDTH,
     FRACTIONAL,
     0,
     1080,
     1920,
     1,
     0,
     {0, 0, 1080, 1920, {FIT(1080, 1920), FIT(1080, 1920)}, {1920, 1920.0f * 1920.0f / 1080.0f}}},
    /* keep_height: the mirror. */
    {"keep_height 16:10 letterboxed",
     1920,
     1080,
     HEIGHT,
     FRACTIONAL,
     0,
     1920,
     1200,
     1,
     0,
     {0, 60, 1920, 1080, {1, 1}, {1920, 1080}}},
    {"keep_height 21:9 shows more width",
     1920,
     1080,
     HEIGHT,
     FRACTIONAL,
     0,
     2560,
     1080,
     1,
     0,
     {0, 0, 2560, 1080, {1, 1}, {2560, 1080}}},
    {"keep_height 4:3 letterboxed",
     1920,
     1080,
     HEIGHT,
     FRACTIONAL,
     0,
     1024,
     768,
     1,
     0,
     {0, 96, 1024, 576, {FIT(1024, 1920), FIT(1024, 1920)}, {1920, 1080}}},
    {"keep_height portrait letterboxed",
     1920,
     1080,
     HEIGHT,
     FRACTIONAL,
     0,
     1080,
     1920,
     1,
     0,
     {0, 656, 1080, 608, {FIT(1080, 1920), FIT(1080, 1920)}, {1920, 1080}}},
    /* expand: the whole frame, the extra shown. */
    {"expand 16:9",
     1920,
     1080,
     EXPAND,
     FRACTIONAL,
     0,
     1280,
     720,
     1,
     0,
     {0, 0, 1280, 720, {FIT(2, 3), FIT(2, 3)}, {1920, 1080}}},
    {"expand 16:10",
     1920,
     1080,
     EXPAND,
     FRACTIONAL,
     0,
     1920,
     1200,
     1,
     0,
     {0, 0, 1920, 1200, {1, 1}, {1920, 1200}}},
    {"expand 21:9",
     1920,
     1080,
     EXPAND,
     FRACTIONAL,
     0,
     2560,
     1080,
     1,
     0,
     {0, 0, 2560, 1080, {1, 1}, {2560, 1080}}},
    {"expand 4:3",
     1920,
     1080,
     EXPAND,
     FRACTIONAL,
     0,
     1024,
     768,
     1,
     0,
     {0, 0, 1024, 768, {FIT(1024, 1920), FIT(1024, 1920)}, {1920, 1440}}},
    {"expand portrait",
     1920,
     1080,
     EXPAND,
     FRACTIONAL,
     0,
     1080,
     1920,
     1,
     0,
     {0, 0, 1080, 1920, {FIT(1080, 1920), FIT(1080, 1920)}, {1920, 1920.0f * 1920.0f / 1080.0f}}},
    /* ignore: the whole frame, each axis its own scale, the design shown. */
    {"ignore 16:9",
     1920,
     1080,
     IGNORE,
     FRACTIONAL,
     0,
     1280,
     720,
     1,
     0,
     {0, 0, 1280, 720, {FIT(2, 3), FIT(2, 3)}, {1920, 1080}}},
    {"ignore 21:9",
     1920,
     1080,
     IGNORE,
     FRACTIONAL,
     0,
     2560,
     1080,
     1,
     0,
     {0, 0, 2560, 1080, {FIT(4, 3), 1}, {1920, 1080}}},
    {"ignore 4:3",
     1920,
     1080,
     IGNORE,
     FRACTIONAL,
     0,
     1024,
     768,
     1,
     0,
     {0, 0, 1024, 768, {FIT(1024, 1920), FIT(768, 1080)}, {1920, 1080}}},
    {"ignore portrait",
     1920,
     1080,
     IGNORE,
     FRACTIONAL,
     0,
     1080,
     1920,
     1,
     0,
     {0, 0, 1080, 1920, {FIT(1080, 1920), FIT(1920, 1080)}, {1920, 1080}}},
    /* Odd sizes: the kept extent rounded to whole pixels, centered with
     * the odd pixel on the far side. */
    {"keep one pixel more each way",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     1921,
     1081,
     1,
     0,
     {0, 0, 1921, 1081, {FIT(1921, 1920), FIT(1921, 1920)}, {1920, 1080}}},
    {"keep an odd frame",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     1001,
     601,
     1,
     0,
     {0, 19, 1001, 563, {FIT(1001, 1920), FIT(1001, 1920)}, {1920, 1080}}},
    {"keep an odd tall frame",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     333,
     777,
     1,
     0,
     {0, 295, 333, 187, {FIT(333, 1920), FIT(333, 1920)}, {1920, 1080}}},
    {"keep the laptop's 1366 by 768",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     1366,
     768,
     1,
     0,
     {0, 0, 1365, 768, {FIT(768, 1080), FIT(768, 1080)}, {1920, 1080}}},
    /* The policy's factor: larger units, fewer shown, in the same pixels. */
    {"keep at twice the scale",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     2,
     1920,
     1080,
     1,
     0,
     {0, 0, 1920, 1080, {2, 2}, {960, 540}}},
    {"expand at half the scale",
     1920,
     1080,
     EXPAND,
     FRACTIONAL,
     0.5f,
     2560,
     1080,
     1,
     0,
     {0, 0, 2560, 1080, {0.5f, 0.5f}, {5120, 2160}}},
    /* The display's scale does not change a design's fit. */
    {"keep on a dense display",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     3840,
     2160,
     2,
     0,
     {0, 0, 3840, 2160, {2, 2}, {1920, 1080}}},
};

/* Whole pixels a unit, for pixel art: a design of 320 by 180. */
static const viewport_case viewport_integer_cases[] = {
    {"integer keep 16:9 by four",
     320,
     180,
     KEEP,
     INTEGER,
     0,
     1280,
     720,
     1,
     0,
     {0, 0, 1280, 720, {4, 4}, {320, 180}}},
    {"integer keep 1366 by 768 by four",
     320,
     180,
     KEEP,
     INTEGER,
     0,
     1366,
     768,
     1,
     0,
     {43, 24, 1280, 720, {4, 4}, {320, 180}}},
    {"integer keep 16:10 by six",
     320,
     180,
     KEEP,
     INTEGER,
     0,
     1920,
     1200,
     1,
     0,
     {0, 60, 1920, 1080, {6, 6}, {320, 180}}},
    {"integer keep 21:9 by six",
     320,
     180,
     KEEP,
     INTEGER,
     0,
     2560,
     1080,
     1,
     0,
     {320, 0, 1920, 1080, {6, 6}, {320, 180}}},
    {"integer keep 4:3 by three",
     320,
     180,
     KEEP,
     INTEGER,
     0,
     1024,
     768,
     1,
     0,
     {32, 114, 960, 540, {3, 3}, {320, 180}}},
    {"integer keep portrait by three",
     320,
     180,
     KEEP,
     INTEGER,
     0,
     1080,
     1920,
     1,
     0,
     {60, 690, 960, 540, {3, 3}, {320, 180}}},
    {"integer keep_width 4:3 shows more height",
     320,
     180,
     WIDTH,
     INTEGER,
     0,
     1024,
     768,
     1,
     0,
     {32, 0, 960, 768, {3, 3}, {320, 256}}},
    {"integer keep_height 21:9 shows more width",
     320,
     180,
     HEIGHT,
     INTEGER,
     0,
     2560,
     1080,
     1,
     0,
     {0, 0, 2560, 1080, {6, 6}, {2560.0f / 6.0f, 180}}},
    {"integer expand 1366 by 768",
     320,
     180,
     EXPAND,
     INTEGER,
     0,
     1366,
     768,
     1,
     0,
     {0, 0, 1366, 768, {4, 4}, {341.5f, 192}}},
    {"integer ignore 4:3, each axis whole",
     320,
     180,
     IGNORE,
     INTEGER,
     0,
     1024,
     768,
     1,
     0,
     {32, 24, 960, 720, {3, 4}, {320, 180}}},
    /* Too small for the design at 1: never less than a pixel a unit, and
     * what fits shown from the design's top left. */
    {"integer keep a tiny frame",
     320,
     180,
     KEEP,
     INTEGER,
     0,
     200,
     100,
     1,
     0,
     {0, 0, 200, 100, {1, 1}, {200, 100}}},
    {"integer keep a tiny tall frame",
     320,
     180,
     KEEP,
     INTEGER,
     0,
     100,
     300,
     1,
     0,
     {0, 60, 100, 180, {1, 1}, {100, 180}}},
    {"integer keep a frame of one pixel",
     320,
     180,
     KEEP,
     INTEGER,
     0,
     1,
     1,
     1,
     0,
     {0, 0, 1, 1, {1, 1}, {1, 1}}},
    {"integer at twice the scale",
     320,
     180,
     KEEP,
     INTEGER,
     2,
     1280,
     720,
     1,
     0,
     {0, 0, 1280, 720, {8, 8}, {160, 90}}},
};

/* Without a design size: logical pixels at the display's scale, over the
 * whole frame, whatever the aspect. */
static const viewport_case viewport_logical_cases[] = {
    {"none at 100%",
     0,
     0,
     KEEP,
     FRACTIONAL,
     0,
     1280,
     720,
     1,
     0,
     {0, 0, 1280, 720, {1, 1}, {1280, 720}}},
    {"none at 150%",
     0,
     0,
     KEEP,
     FRACTIONAL,
     0,
     1280,
     720,
     1.5f,
     0,
     {0, 0, 1280, 720, {1.5f, 1.5f}, {1280.0f / 1.5f, 480}}},
    {"none at 200%",
     0,
     0,
     EXPAND,
     FRACTIONAL,
     0,
     1280,
     720,
     2,
     0,
     {0, 0, 1280, 720, {2, 2}, {640, 360}}},
    {"none at 150%, whole pixels",
     0,
     0,
     KEEP,
     INTEGER,
     0,
     1280,
     720,
     1.5f,
     0,
     {0, 0, 1280, 720, {1, 1}, {1280, 720}}},
    {"none at 150%, twice",
     0,
     0,
     IGNORE,
     FRACTIONAL,
     2,
     1280,
     720,
     1.5f,
     0,
     {0, 0, 1280, 720, {3, 3}, {1280.0f / 3.0f, 240}}},
    {"none of one axis alone",
     1920,
     0,
     KEEP,
     FRACTIONAL,
     0,
     1280,
     720,
     1,
     0,
     {0, 0, 1280, 720, {1, 1}, {1280, 720}}},
    {"none without a display scale",
     0,
     0,
     KEEP,
     FRACTIONAL,
     0,
     1280,
     720,
     0,
     0,
     {0, 0, 1280, 720, {1, 1}, {1280, 720}}},
    {"a frame of no size",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     0,
     0,
     1,
     0,
     {0, 0, 0, 0, {1, 1}, {0, 0}}},
    {"a frame of no height", 0, 0, KEEP, FRACTIONAL, 0, 640, 0, 1, 0, {0, 0, 0, 0, {1, 1}, {0, 0}}},
};

/* The player's aspect ratio narrows the frame first, to the largest
 * centered rectangle of its shape. */
static const viewport_case viewport_ratio_cases[] = {
    {"16:9 in a 16:10 frame",
     0,
     0,
     KEEP,
     FRACTIONAL,
     0,
     1920,
     1200,
     1,
     16.0f / 9.0f,
     {0, 60, 1920, 1080, {1, 1}, {1920, 1080}}},
    {"16:9 in a 21:9 frame",
     0,
     0,
     KEEP,
     FRACTIONAL,
     0,
     2560,
     1080,
     1,
     16.0f / 9.0f,
     {320, 0, 1920, 1080, {1, 1}, {1920, 1080}}},
    {"21:9 in a 16:9 frame",
     0,
     0,
     KEEP,
     FRACTIONAL,
     0,
     1920,
     1080,
     1,
     64.0f / 27.0f,
     {0, 135, 1920, 810, {1, 1}, {1920, 810}}},
    {"4:3 then the design kept in it",
     1920,
     1080,
     KEEP,
     FRACTIONAL,
     0,
     1920,
     1080,
     1,
     4.0f / 3.0f,
     {240, 135, 1440, 810, {0.75f, 0.75f}, {1920, 1080}}},
    {"4:3 then expanded in it",
     1920,
     1080,
     EXPAND,
     FRACTIONAL,
     0,
     1920,
     1080,
     1,
     4.0f / 3.0f,
     {240, 0, 1440, 1080, {0.75f, 0.75f}, {1920, 1440}}},
    {"the frame's own shape",
     0,
     0,
     KEEP,
     FRACTIONAL,
     0,
     1920,
     1080,
     1,
     16.0f / 9.0f,
     {0, 0, 1920, 1080, {1, 1}, {1920, 1080}}},
    {"no ratio, nothing narrowed",
     1920,
     1080,
     EXPAND,
     FRACTIONAL,
     0,
     1920,
     1200,
     1,
     0,
     {0, 0, 1920, 1200, {1, 1}, {1920, 1200}}},
};

static tb_viewport_policy viewport_policy_of(const viewport_case *test)
{
    tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;

    policy.design_width = test->design_width;
    policy.design_height = test->design_height;
    policy.aspect = test->aspect;
    policy.scale_mode = test->mode;
    policy.scale = test->factor;
    return policy;
}

static void viewport_check(const viewport_case *cases, size_t count)
{
    for (size_t index = 0; index < count; index++)
    {
        const viewport_case *const test = &cases[index];
        const tb_viewport_policy policy = viewport_policy_of(test);
        const tb_viewport_frame frame = {.width = test->width,
                                         .height = test->height,
                                         .display_scale = test->display_scale,
                                         .aspect_ratio = test->aspect_ratio};
        const tb_viewport got = tb_viewport_compute(&policy, &frame);
        TB_TEST_CONTEXT("%s", test->name);
        TB_TEST_EXPECT_UINT(got.x, ==, test->expected.x);
        TB_TEST_EXPECT_UINT(got.y, ==, test->expected.y);
        TB_TEST_EXPECT_UINT(got.width, ==, test->expected.width);
        TB_TEST_EXPECT_UINT(got.height, ==, test->expected.height);
        TB_TEST_EXPECT_FLOAT_NEAR(got.scale.x, test->expected.scale.x, 1e-5f);
        TB_TEST_EXPECT_FLOAT_NEAR(got.scale.y, test->expected.scale.y, 1e-5f);
        TB_TEST_EXPECT_FLOAT_NEAR(got.size.x, test->expected.size.x, 1e-2f);
        TB_TEST_EXPECT_FLOAT_NEAR(got.size.y, test->expected.size.y, 1e-2f);
        /* The canvas mode has no target. */
        TB_TEST_EXPECT_UINT(got.target_width, ==, 0u);
        TB_TEST_EXPECT_UINT(got.target_height, ==, 0u);
    }
}

/* The viewport render mode's target of a table's cases: the same viewport
 * as the canvas mode's, the mouse placed alike, and a target of the units
 * shown, rounded up, at least a pixel. */
static void viewport_check_targets(const viewport_case *cases, size_t count)
{
    for (size_t index = 0; index < count; index++)
    {
        const viewport_case *const test = &cases[index];
        tb_viewport_policy policy = viewport_policy_of(test);
        const tb_viewport_frame frame = {.width = test->width,
                                         .height = test->height,
                                         .display_scale = test->display_scale,
                                         .aspect_ratio = test->aspect_ratio};
        const tb_viewport canvas = tb_viewport_compute(&policy, &frame);
        policy.render_mode = TB_VIEWPORT_RENDER_VIEWPORT;
        const tb_viewport got = tb_viewport_compute(&policy, &frame);
        TB_TEST_CONTEXT("%s", test->name);
        TB_TEST_EXPECT_UINT(got.x, ==, canvas.x);
        TB_TEST_EXPECT_UINT(got.y, ==, canvas.y);
        TB_TEST_EXPECT_UINT(got.width, ==, canvas.width);
        TB_TEST_EXPECT_UINT(got.height, ==, canvas.height);
        TB_TEST_EXPECT_FLOAT(got.scale.x, ==, canvas.scale.x);
        TB_TEST_EXPECT_FLOAT(got.scale.y, ==, canvas.scale.y);
        TB_TEST_EXPECT_FLOAT(got.size.x, ==, canvas.size.x);
        TB_TEST_EXPECT_FLOAT(got.size.y, ==, canvas.size.y);
        if (canvas.width == 0 || canvas.height == 0)
        {
            /* A frame of no size shows nothing, and has no target. */
            TB_TEST_EXPECT_UINT(got.target_width, ==, 0u);
            TB_TEST_EXPECT_UINT(got.target_height, ==, 0u);
            continue;
        }
        TB_TEST_EXPECT_UINT(got.target_width, ==, (uint32_t)ceilf(test->expected.size.x - 0.01f));
        TB_TEST_EXPECT_UINT(got.target_height, ==, (uint32_t)ceilf(test->expected.size.y - 0.01f));
        TB_TEST_EXPECT_UINT(got.target_width, >=, 1u);
        TB_TEST_EXPECT_UINT(got.target_height, >=, 1u);
    }
}

/* Every case of every table, in the viewport render mode: the target holds
 * the units the viewport shows, a pixel a unit, rounded up where an open
 * axis shows part of a unit. */
TB_TEST(viewport, the_viewport_render_modes_target_is_the_units_shown)
{
    viewport_check_targets(viewport_fractional_cases, TB_TEST_COUNT_OF(viewport_fractional_cases));
    viewport_check_targets(viewport_integer_cases, TB_TEST_COUNT_OF(viewport_integer_cases));
    viewport_check_targets(viewport_logical_cases, TB_TEST_COUNT_OF(viewport_logical_cases));
    viewport_check_targets(viewport_ratio_cases, TB_TEST_COUNT_OF(viewport_ratio_cases));
}

/* The targets a pixel-art game sees: 320 by 180 at whole pixels a unit is
 * 320 by 180 on any frame, the design itself; `expand` grows it by what a
 * wider or taller frame shows, as Godot's viewport mode does; the policy's
 * scale shows fewer units, so a smaller target; a frame of no size has
 * none; and a target never passes the largest texture. */
TB_TEST(viewport, the_viewport_render_modes_target_follows_the_policy)
{
    tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
    tb_viewport got = {0};

    policy.design_width = 320;
    policy.design_height = 180;
    policy.aspect = TB_VIEWPORT_ASPECT_KEEP;
    policy.scale_mode = TB_VIEWPORT_SCALE_INTEGER;
    policy.render_mode = TB_VIEWPORT_RENDER_VIEWPORT;
    got = tb_viewport_compute(&policy, &(tb_viewport_frame){.width = 1280, .height = 720});
    TB_TEST_EXPECT_UINT(got.target_width, ==, 320u);
    TB_TEST_EXPECT_UINT(got.target_height, ==, 180u);
    TB_TEST_EXPECT_FLOAT(got.scale.x, ==, 4.0f);
    got = tb_viewport_compute(&policy, &(tb_viewport_frame){.width = 1366, .height = 768});
    TB_TEST_EXPECT_UINT(got.target_width, ==, 320u);
    TB_TEST_EXPECT_UINT(got.target_height, ==, 180u);

    /* 1000 by 700 fits 320 by 180 at 3.125 by width; 700 / 3.125 = 224. */
    policy.scale_mode = TB_VIEWPORT_SCALE_FRACTIONAL;
    policy.aspect = TB_VIEWPORT_ASPECT_EXPAND;
    got = tb_viewport_compute(&policy, &(tb_viewport_frame){.width = 1000, .height = 700});
    TB_TEST_EXPECT_UINT(got.target_width, ==, 320u);
    TB_TEST_EXPECT_UINT(got.target_height, ==, 224u);
    /* 1001 by 700: 700 / (1001 / 320) is 223.78 units, a target of 224. */
    got = tb_viewport_compute(&policy, &(tb_viewport_frame){.width = 1001, .height = 700});
    TB_TEST_EXPECT_UINT(got.target_width, ==, 320u);
    TB_TEST_EXPECT_UINT(got.target_height, ==, 224u);
    policy.aspect = TB_VIEWPORT_ASPECT_KEEP_HEIGHT;
    got = tb_viewport_compute(&policy, &(tb_viewport_frame){.width = 2560, .height = 1080});
    TB_TEST_EXPECT_UINT(got.target_width, ==, 427u); /* 426.67 units */
    TB_TEST_EXPECT_UINT(got.target_height, ==, 180u);

    /* Twice as large a unit shows half as many: half the target. */
    policy.aspect = TB_VIEWPORT_ASPECT_KEEP;
    policy.scale = 2.0f;
    got = tb_viewport_compute(&policy, &(tb_viewport_frame){.width = 1280, .height = 720});
    TB_TEST_EXPECT_UINT(got.target_width, ==, 160u);
    TB_TEST_EXPECT_UINT(got.target_height, ==, 90u);

    /* Without a design, the logical pixels: 1280 by 720 at 150% shows
     * 853.33 by 480. */
    policy = (tb_viewport_policy)TB_VIEWPORT_POLICY_LOGICAL;
    policy.render_mode = TB_VIEWPORT_RENDER_VIEWPORT;
    got = tb_viewport_compute(
        &policy, &(tb_viewport_frame){.width = 1280, .height = 720, .display_scale = 1.5f});
    TB_TEST_EXPECT_UINT(got.target_width, ==, 854u);
    TB_TEST_EXPECT_UINT(got.target_height, ==, 480u);

    got = tb_viewport_compute(&policy, &(tb_viewport_frame){0});
    TB_TEST_EXPECT_UINT(got.target_width, ==, 0u);
    TB_TEST_EXPECT_UINT(got.target_height, ==, 0u);

    /* An absurd design at a quarter of a pixel a unit: the largest target. */
    policy = (tb_viewport_policy)TB_VIEWPORT_POLICY_INIT;
    policy.design_width = 16384;
    policy.design_height = 16384;
    policy.scale = 0.25f;
    policy.render_mode = TB_VIEWPORT_RENDER_VIEWPORT;
    got = tb_viewport_compute(&policy, &(tb_viewport_frame){.width = 640, .height = 640});
    TB_TEST_EXPECT_UINT(got.target_width, ==, TB_VIEWPORT_MAX_TARGET);
    TB_TEST_EXPECT_UINT(got.target_height, ==, TB_VIEWPORT_MAX_TARGET);
}

TB_TEST(viewport, every_aspect_fits_frames_of_every_shape_at_fractional_scales)
{
    viewport_check(viewport_fractional_cases, TB_TEST_COUNT_OF(viewport_fractional_cases));
}

TB_TEST(viewport, integer_scales_by_whole_pixels_and_at_least_one)
{
    viewport_check(viewport_integer_cases, TB_TEST_COUNT_OF(viewport_integer_cases));
}

TB_TEST(viewport, without_a_design_units_are_logical_pixels_over_the_whole_frame)
{
    viewport_check(viewport_logical_cases, TB_TEST_COUNT_OF(viewport_logical_cases));
}

TB_TEST(viewport, the_players_aspect_ratio_narrows_the_frame_first)
{
    viewport_check(viewport_ratio_cases, TB_TEST_COUNT_OF(viewport_ratio_cases));
}

/* Along each axis of a viewport in the canvas render mode, the design
 * rectangle starts on a whole pixel, and is centered in the units shown:
 * the units on either side of it differ by a pixel at most, the odd one
 * after it; where the design does not fit, it starts at 0. */
static void viewport_require_design_axis(float origin, float design, float units, float scale)
{
    const float pixels = origin * scale;

    TB_TEST_REQUIRE_FLOAT(origin, >=, 0.0f);
    TB_TEST_REQUIRE_FLOAT_NEAR(pixels, roundf(pixels), 1e-2f);
    if (design >= units)
    {
        TB_TEST_REQUIRE_FLOAT(origin, ==, 0.0f);
        return;
    }
    const float after = (units - design - origin) * scale;
    TB_TEST_REQUIRE_FLOAT(after - pixels, >=, -1e-2f);
    TB_TEST_REQUIRE_FLOAT(after - pixels, <, 1.0f + 1e-2f);
}

static void viewport_require_design_centered(const tb_viewport *got)
{
    viewport_require_design_axis(got->design_origin.x, got->design_size.x, got->size.x,
                                 got->scale.x);
    viewport_require_design_axis(got->design_origin.y, got->design_size.y, got->size.y,
                                 got->scale.y);
}

/* Over frames of every size up to a few hundred pixels, for every aspect
 * and scale mode: the viewport lies within the frame, its bars differ by a
 * pixel at most from side to side, it is never empty, whole pixels a unit
 * are whole, and the design rectangle is centered on whole pixels. */
TB_TEST(viewport, every_viewport_is_whole_pixels_centered_in_its_frame)
{
    static const uint32_t designs[][2] = {{1920, 1080}, {320, 180}, {7, 3}, {0, 0}};
    const float scales[] = {1.0f, 1.25f, 1.5f, 2.0f};

    for (size_t design = 0; design < TB_TEST_COUNT_OF(designs); design++)
    {
        for (int aspect = 0; aspect < TB_VIEWPORT_ASPECT_COUNT; aspect++)
        {
            for (int mode = 0; mode < TB_VIEWPORT_SCALE_COUNT; mode++)
            {
                tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
                policy.design_width = designs[design][0];
                policy.design_height = designs[design][1];
                policy.aspect = (tb_viewport_aspect)aspect;
                policy.scale_mode = (tb_viewport_scale_mode)mode;
                for (uint32_t width = 1; width < 400; width += 13)
                {
                    for (uint32_t height = 1; height < 400; height += 17)
                    {
                        const tb_viewport_frame frame = {
                            .width = width,
                            .height = height,
                            .display_scale = scales[(width + height) % TB_TEST_COUNT_OF(scales)]};
                        const tb_viewport got = tb_viewport_compute(&policy, &frame);
                        TB_TEST_CONTEXT(
                            "design %u by %u, %s, %s, frame %u by %u", policy.design_width,
                            policy.design_height, tb_viewport_aspect_name(policy.aspect),
                            tb_viewport_scale_mode_name(policy.scale_mode), width, height);
                        TB_TEST_REQUIRE_UINT(got.width, >=, 1u);
                        TB_TEST_REQUIRE_UINT(got.height, >=, 1u);
                        TB_TEST_REQUIRE_UINT(got.x + got.width, <=, width);
                        TB_TEST_REQUIRE_UINT(got.y + got.height, <=, height);
                        const uint32_t right = width - got.x - got.width;
                        const uint32_t bottom = height - got.y - got.height;
                        TB_TEST_REQUIRE_UINT(right - got.x, <=, 1u);
                        TB_TEST_REQUIRE_UINT(bottom - got.y, <=, 1u);
                        TB_TEST_REQUIRE(got.scale.x > 0.0f && got.scale.y > 0.0f);
                        if (mode == TB_VIEWPORT_SCALE_INTEGER)
                        {
                            TB_TEST_REQUIRE_FLOAT(got.scale.x, ==, floorf(got.scale.x));
                            TB_TEST_REQUIRE_FLOAT(got.scale.x, >=, 1.0f);
                        }
                        viewport_require_design_centered(&got);
                    }
                }
            }
        }
    }
}

TB_TEST(viewport, pixels_and_units_convert_both_ways_and_the_bars_are_outside)
{
    tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
    policy.aspect = TB_VIEWPORT_ASPECT_KEEP;
    const tb_viewport viewport =
        tb_viewport_compute(&policy, &(tb_viewport_frame){.width = 1920, .height = 1200});
    const tb_viewport stretched = {0, 0, 2560, 1080, {FIT(4, 3), 1.0f}, {1920, 1080}, 0, 0, {0, 0}, {0, 0}};

    /* The design's middle is the frame's. */
    const tb_vec2 middle = tb_viewport_to_pixels(&viewport, (tb_vec2){960, 540});
    TB_TEST_EXPECT_FLOAT(middle.x, ==, 960.0f);
    TB_TEST_EXPECT_FLOAT(middle.y, ==, 600.0f);
    const tb_vec2 back = tb_viewport_from_pixels(&viewport, middle);
    TB_TEST_EXPECT_FLOAT(back.x, ==, 960.0f);
    TB_TEST_EXPECT_FLOAT(back.y, ==, 540.0f);
    /* The rectangle's first row and column are in it; its last plus one,
     * and the bars, are not. */
    TB_TEST_EXPECT(tb_viewport_contains(&viewport, (tb_vec2){0, 60}));
    TB_TEST_EXPECT(tb_viewport_contains(&viewport, (tb_vec2){1919.5f, 1139.5f}));
    TB_TEST_EXPECT(!tb_viewport_contains(&viewport, (tb_vec2){960, 59.5f}));
    TB_TEST_EXPECT(!tb_viewport_contains(&viewport, (tb_vec2){960, 1140}));
    TB_TEST_EXPECT(!tb_viewport_contains(&viewport, (tb_vec2){1920, 600}));
    TB_TEST_EXPECT(!tb_viewport_contains(&viewport, (tb_vec2){-0.5f, 600}));
    /* A point on a bar is outside the design's units. */
    TB_TEST_EXPECT_FLOAT(tb_viewport_from_pixels(&viewport, (tb_vec2){10, 10}).y, <, 0.0f);
    /* Stretched, each axis by its own scale. */
    const tb_vec2 corner = tb_viewport_to_pixels(&stretched, (tb_vec2){1920, 1080});
    TB_TEST_EXPECT_FLOAT_NEAR(corner.x, 2560.0f, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(corner.y, 1080.0f, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_viewport_from_pixels(&stretched, (tb_vec2){1280, 540}).x, 960.0f,
                              1e-3f);
}

/* A case of the design rectangle's table: a policy, a frame, and where the
 * design lies among the units shown. */
typedef struct viewport_design_case
{
    const char *name;
    uint32_t design_width;
    uint32_t design_height;
    tb_viewport_aspect aspect;
    tb_viewport_scale_mode mode;
    tb_viewport_render_mode render;
    float factor;   /* the policy's scale; 0 is 1 */
    uint32_t width; /* the frame */
    uint32_t height;
    tb_vec2 size; /* the units shown */
    tb_vec2 origin;
    tb_vec2 design;
} viewport_design_case;

#define CANVAS TB_VIEWPORT_RENDER_CANVAS
#define TARGET TB_VIEWPORT_RENDER_VIEWPORT

/* A 16:9 design on frames of every common shape: in `expand`, the design
 * rectangle is centered in what the frame shows, the extra split on
 * either side on whole pixels; along a kept axis it is the units shown. */
static const viewport_design_case viewport_design_cases[] = {
    {"expand 16:9 at its size", 1920, 1080, EXPAND, FRACTIONAL, CANVAS, 0, 1920, 1080,
     {1920, 1080}, {0, 0}, {1920, 1080}},
    {"expand 16:10", 1920, 1080, EXPAND, FRACTIONAL, CANVAS, 0, 2560, 1600,
     {1920, 1200}, {0, 60}, {1920, 1080}},
    /* 360 units more, 192 pixels: 96 above, 180 units. */
    {"expand 4:3", 1920, 1080, EXPAND, FRACTIONAL, CANVAS, 0, 1024, 768,
     {1920, 1440}, {0, 180}, {1920, 1080}},
    {"expand 21:9", 1920, 1080, EXPAND, FRACTIONAL, CANVAS, 0, 2560, 1080,
     {2560, 1080}, {320, 0}, {1920, 1080}},
    {"expand 32:9", 1920, 1080, EXPAND, FRACTIONAL, CANVAS, 0, 3840, 1080,
     {3840, 1080}, {960, 0}, {1920, 1080}},
    /* 1312.5 pixels more down: 656 above, at 0.5625 pixels a unit. */
    {"expand portrait", 1920, 1080, EXPAND, FRACTIONAL, CANVAS, 0, 1080, 1920,
     {1920, FIT(1920 * 1920, 1080)}, {0, FIT(656 * 16, 9)}, {1920, 1080}},
    /* Less than a pixel more is no offset at all. */
    {"expand 1366 by 768", 1920, 1080, EXPAND, FRACTIONAL, CANVAS, 0, 1366, 768,
     {FIT(1366 * 1080, 768), 1080}, {0, 0}, {1920, 1080}},
    /* Two pixels more: one on either side. One more: after it. */
    {"expand two pixels wider", 1920, 1080, EXPAND, FRACTIONAL, CANVAS, 0, 1922, 1080,
     {1922, 1080}, {1, 0}, {1920, 1080}},
    {"expand a pixel wider", 1920, 1080, EXPAND, FRACTIONAL, CANVAS, 0, 1921, 1080,
     {1921, 1080}, {0, 0}, {1920, 1080}},
    /* Twice as large a unit: the design is half as many units. */
    {"expand 16:10 at a scale of 2", 1920, 1080, EXPAND, FRACTIONAL, CANVAS, 2, 2560, 1600,
     {960, 600}, {0, 30}, {960, 540}},
    /* Whole pixels a unit, 4: 80 pixels more down, 40 above, 10 units. */
    {"expand integer 16:10", 320, 180, EXPAND, INTEGER, CANVAS, 0, 1280, 800,
     {320, 200}, {0, 10}, {320, 180}},
    /* 86 pixels more across, 43 before it: 10.75 units, a whole pixel of
     * the frame, though not a whole unit. */
    {"expand integer 1366 by 768", 320, 180, EXPAND, INTEGER, CANVAS, 0, 1366, 768,
     {341.5f, 192}, {10.75f, 6}, {320, 180}},
    /* The viewport render mode: on the target's whole pixels, a unit. */
    {"expand integer 1366 by 768 in a target", 320, 180, EXPAND, INTEGER, TARGET, 0, 1366, 768,
     {341.5f, 192}, {10, 6}, {320, 180}},
    {"expand 4:3 in a target", 1920, 1080, EXPAND, FRACTIONAL, TARGET, 0, 1024, 768,
     {1920, 1440}, {0, 180}, {1920, 1080}},
    /* Too small for a unit a pixel: the design's top left. */
    {"expand integer too small", 320, 180, EXPAND, INTEGER, CANVAS, 0, 200, 100,
     {200, 100}, {0, 0}, {320, 180}},
    /* One axis open: centered along it alone. */
    {"keep_width 16:10", 1920, 1080, WIDTH, FRACTIONAL, CANVAS, 0, 1920, 1200,
     {1920, 1200}, {0, 60}, {1920, 1080}},
    {"keep_height 21:9", 1920, 1080, HEIGHT, FRACTIONAL, CANVAS, 0, 2560, 1080,
     {2560, 1080}, {320, 0}, {1920, 1080}},
    {"keep_height 16:10 letterboxed", 1920, 1080, HEIGHT, FRACTIONAL, CANVAS, 0, 1920, 1200,
     {1920, 1080}, {0, 0}, {1920, 1080}},
    /* Kept and stretched: the design is what is shown. */
    {"keep 16:10", 1920, 1080, KEEP, FRACTIONAL, CANVAS, 0, 1920, 1200,
     {1920, 1080}, {0, 0}, {1920, 1080}},
    {"ignore 4:3", 1920, 1080, IGNORE, FRACTIONAL, CANVAS, 0, 1024, 768,
     {1920, 1080}, {0, 0}, {1920, 1080}},
    /* Without a design, what is shown. */
    {"no design", 0, 0, EXPAND, FRACTIONAL, CANVAS, 0, 1280, 720,
     {1280, 720}, {0, 0}, {1280, 720}},
};

TB_TEST(viewport, the_design_rectangle_is_centered_in_what_is_shown)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(viewport_design_cases); index++)
    {
        const viewport_design_case *const test = &viewport_design_cases[index];
        tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
        policy.design_width = test->design_width;
        policy.design_height = test->design_height;
        policy.aspect = test->aspect;
        policy.scale_mode = test->mode;
        policy.render_mode = test->render;
        policy.scale = test->factor;
        const tb_viewport got = tb_viewport_compute(
            &policy, &(tb_viewport_frame){.width = test->width, .height = test->height});
        TB_TEST_CONTEXT("%s", test->name);
        TB_TEST_EXPECT_FLOAT_NEAR(got.size.x, test->size.x, 1e-2f);
        TB_TEST_EXPECT_FLOAT_NEAR(got.size.y, test->size.y, 1e-2f);
        TB_TEST_EXPECT_FLOAT_NEAR(got.design_origin.x, test->origin.x, 1e-2f);
        TB_TEST_EXPECT_FLOAT_NEAR(got.design_origin.y, test->origin.y, 1e-2f);
        TB_TEST_EXPECT_FLOAT_NEAR(got.design_size.x, test->design.x, 1e-2f);
        TB_TEST_EXPECT_FLOAT_NEAR(got.design_size.y, test->design.y, 1e-2f);
        if (test->render == CANVAS)
        {
            viewport_require_design_centered(&got);
        }
    }
}

/* Anchors: a HUD pins to the visible rectangle's corners, which are the
 * frame's in `expand`; content of the design pins to the design's, whose
 * middle is the frame's. */
TB_TEST(viewport, anchors_pin_to_the_visible_and_the_design_rectangles)
{
    const tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
    const tb_viewport viewport =
        tb_viewport_compute(&policy, &(tb_viewport_frame){.width = 2560, .height = 1600});
    const tb_viewport empty = {0};

    tb_vec2 at = tb_viewport_anchor(&viewport, (tb_vec2){1.0f, 0.0f});
    TB_TEST_EXPECT_FLOAT_NEAR(at.x, 1920.0f, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(at.y, 0.0f, 1e-3f);
    at = tb_viewport_anchor(&viewport, (tb_vec2){0.0f, 1.0f});
    TB_TEST_EXPECT_FLOAT_NEAR(tb_viewport_to_pixels(&viewport, at).y, 1600.0f, 1e-2f);
    at = tb_viewport_design_anchor(&viewport, (tb_vec2){0.0f, 0.0f});
    TB_TEST_EXPECT_FLOAT_NEAR(at.x, 0.0f, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(at.y, 60.0f, 1e-3f);
    at = tb_viewport_design_anchor(&viewport, (tb_vec2){1.0f, 1.0f});
    TB_TEST_EXPECT_FLOAT_NEAR(at.x, 1920.0f, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(at.y, 1140.0f, 1e-3f);
    /* The design's middle is the visible one, and the frame's. */
    const tb_vec2 middle = tb_viewport_design_anchor(&viewport, (tb_vec2){0.5f, 0.5f});
    const tb_vec2 shown = tb_viewport_anchor(&viewport, (tb_vec2){0.5f, 0.5f});
    TB_TEST_EXPECT_FLOAT_NEAR(middle.x, shown.x, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(middle.y, shown.y, 1e-3f);
    const tb_vec2 pixel = tb_viewport_to_pixels(&viewport, middle);
    TB_TEST_EXPECT_FLOAT_NEAR(pixel.x, 1280.0f, 1e-2f);
    TB_TEST_EXPECT_FLOAT_NEAR(pixel.y, 800.0f, 1e-2f);

    /* The camera's reference is the design's shape; the viewport's own
     * where there is no design rectangle. */
    TB_TEST_EXPECT_FLOAT_NEAR(tb_viewport_design_aspect(&viewport), 16.0f / 9.0f, 1e-5f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_viewport_aspect_of(&viewport), 1.6f, 1e-5f);
    TB_TEST_EXPECT_FLOAT(tb_viewport_design_aspect(&empty), ==, 1.0f);
}

/* The camera's aspect is what the viewport shows: the rectangle's, or in
 * `ignore` the design's, stretched; text's scale is the smaller axis's. */
TB_TEST(viewport, the_aspect_is_what_the_viewport_shows)
{
    const tb_viewport keep = {0, 96, 1024, 576, {FIT(1024, 1920), FIT(1024, 1920)}, {1920, 1080},
                              0, 0, {0, 0}, {0, 0}};
    const tb_viewport ignore = {0, 0, 1024, 768, {FIT(1024, 1920), FIT(768, 1080)}, {1920, 1080},
                                0, 0, {0, 0}, {0, 0}};
    const tb_viewport expand = {0, 0, 1024, 768, {FIT(1024, 1920), FIT(1024, 1920)}, {1920, 1440},
                                0, 0, {0, 0}, {0, 0}};
    const tb_viewport empty = {0, 0, 0, 0, {1, 1}, {0, 0}, 0, 0, {0, 0}, {0, 0}};

    TB_TEST_EXPECT_FLOAT_NEAR(tb_viewport_aspect_of(&keep), 16.0f / 9.0f, 1e-5f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_viewport_aspect_of(&ignore), 16.0f / 9.0f, 1e-5f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_viewport_aspect_of(&expand), 4.0f / 3.0f, 1e-5f);
    TB_TEST_EXPECT_FLOAT(tb_viewport_aspect_of(&empty), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_viewport_uniform_scale(&ignore), ==, FIT(1024, 1920));
    TB_TEST_EXPECT_FLOAT(tb_viewport_uniform_scale(&keep), ==, FIT(1024, 1920));
}

TB_TEST(viewport, a_bar_color_is_hash_and_six_hexadecimal_digits)
{
    tb_vec4 color = {0};

    TB_TEST_EXPECT(tb_viewport_parse_color("#000000", &color));
    TB_TEST_EXPECT_FLOAT(color.w, ==, 1.0f);
    TB_TEST_EXPECT(tb_viewport_parse_color("#FF8000", &color));
    TB_TEST_EXPECT_FLOAT(color.x, ==, 1.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(color.y, 128.0f / 255.0f, 1e-6f);
    TB_TEST_EXPECT_FLOAT(color.z, ==, 0.0f);
    TB_TEST_EXPECT(tb_viewport_parse_color("#0a0B0c", &color));
    TB_TEST_EXPECT_FLOAT_NEAR(color.z, 12.0f / 255.0f, 1e-6f);
    /* Anything else leaves the color as it was. */
    color = (tb_vec4){0.5f, 0.5f, 0.5f, 0.5f};
    TB_TEST_EXPECT(!tb_viewport_parse_color("ff8000", &color));
    TB_TEST_EXPECT(!tb_viewport_parse_color("#ff80", &color));
    TB_TEST_EXPECT(!tb_viewport_parse_color("#ff80001", &color));
    TB_TEST_EXPECT(!tb_viewport_parse_color("#gg0000", &color));
    TB_TEST_EXPECT(!tb_viewport_parse_color("", &color));
    TB_TEST_EXPECT_FLOAT(color.x, ==, 0.5f);
}

TB_TEST(viewport, each_choice_has_the_name_its_setting_gives_it)
{
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};

    tb_engine_settings_add(settings, NULL, &engine);
    for (int aspect = 0; aspect < TB_VIEWPORT_ASPECT_COUNT; aspect++)
    {
        TB_TEST_EXPECT_STRING(
            tb_viewport_aspect_name((tb_viewport_aspect)aspect), ==,
            tb_setting_choice_name(engine.display.stretch_aspect, (size_t)aspect));
    }
    for (int mode = 0; mode < TB_VIEWPORT_SCALE_COUNT; mode++)
    {
        TB_TEST_EXPECT_STRING(
            tb_viewport_scale_mode_name((tb_viewport_scale_mode)mode), ==,
            tb_setting_choice_name(engine.display.stretch_scale_mode, (size_t)mode));
    }
    for (int mode = 0; mode < TB_VIEWPORT_RENDER_COUNT; mode++)
    {
        TB_TEST_EXPECT_STRING(
            tb_viewport_render_mode_name((tb_viewport_render_mode)mode), ==,
            tb_setting_choice_name(engine.display.stretch_render_mode, (size_t)mode));
    }
    TB_TEST_EXPECT_STRING(tb_viewport_aspect_name(TB_VIEWPORT_ASPECT_COUNT), ==, "unknown");
    TB_TEST_EXPECT_STRING(tb_viewport_scale_mode_name(TB_VIEWPORT_SCALE_COUNT), ==, "unknown");
    TB_TEST_EXPECT_STRING(tb_viewport_render_mode_name(TB_VIEWPORT_RENDER_COUNT), ==, "unknown");
    tb_settings_destroy(settings);
}

/* The settings' built-in values are the engine's default policy; an app's
 * default file, or a player, changes each. */
TB_TEST(viewport, the_settings_describe_the_policy)
{
    static const char overrides[] = "display.design_width = 320\n"
                                    "display.design_height = 180\n"
                                    "display.stretch_aspect = keep_height\n"
                                    "display.stretch_scale_mode = integer\n"
                                    "display.stretch_render_mode = viewport\n"
                                    "display.stretch_scale = 2\n"
                                    "display.bar_color = \"#336699\"\n";
    const tb_viewport_policy defaults = TB_VIEWPORT_POLICY_INIT;
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};
    tb_viewport_policy policy = {0};

    tb_engine_settings_add(settings, NULL, &engine);
    tb_viewport_policy_from_settings(&engine, &policy);
    TB_TEST_EXPECT_UINT(policy.design_width, ==, defaults.design_width);
    TB_TEST_EXPECT_UINT(policy.design_height, ==, defaults.design_height);
    TB_TEST_EXPECT_INT(policy.aspect, ==, defaults.aspect);
    TB_TEST_EXPECT_INT(policy.scale_mode, ==, defaults.scale_mode);
    TB_TEST_EXPECT_INT(policy.render_mode, ==, defaults.render_mode);
    TB_TEST_EXPECT_FLOAT(policy.scale, ==, defaults.scale);
    TB_TEST_EXPECT_FLOAT(policy.bar_color.x, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(policy.bar_color.w, ==, 1.0f);

    TB_TEST_EXPECT_UINT(tb_settings_load_text(settings, TB_SETTING_LAYER_DEFAULT_FILE, overrides,
                                              sizeof(overrides) - 1u, NULL, NULL),
                        ==, 0u);
    tb_viewport_policy_from_settings(&engine, &policy);
    TB_TEST_EXPECT_UINT(policy.design_width, ==, 320u);
    TB_TEST_EXPECT_UINT(policy.design_height, ==, 180u);
    TB_TEST_EXPECT_INT(policy.aspect, ==, TB_VIEWPORT_ASPECT_KEEP_HEIGHT);
    TB_TEST_EXPECT_INT(policy.scale_mode, ==, TB_VIEWPORT_SCALE_INTEGER);
    TB_TEST_EXPECT_INT(policy.render_mode, ==, TB_VIEWPORT_RENDER_VIEWPORT);
    TB_TEST_EXPECT_FLOAT(policy.scale, ==, 2.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(policy.bar_color.x, 0.2f, 1e-6f);
    TB_TEST_EXPECT_FLOAT_NEAR(policy.bar_color.z, 0.6f, 1e-6f);

    /* A color that is not one is black. */
    TB_TEST_EXPECT_UINT(tb_setting_set_text(engine.display.bar_color, "red"), ==, TB_SETTING_OK);
    tb_viewport_policy_from_settings(&engine, &policy);
    TB_TEST_EXPECT_FLOAT(policy.bar_color.x, ==, 0.0f);
    tb_settings_destroy(settings);
}

TB_TEST(viewport, the_players_aspect_ratio_is_its_shape_or_none)
{
    static const float ratios[] = {0.0f,          16.0f / 9.0f, 16.0f / 10.0f,
                                   64.0f / 27.0f, 32.0f / 9.0f, 4.0f / 3.0f};
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};

    tb_engine_settings_add(settings, NULL, &engine);
    TB_TEST_REQUIRE_UINT(tb_setting_choice_count(engine.display.aspect_ratio), ==,
                         TB_TEST_COUNT_OF(ratios));
    for (size_t choice = 0; choice < TB_TEST_COUNT_OF(ratios); choice++)
    {
        TB_TEST_CONTEXT("%s", tb_setting_choice_name(engine.display.aspect_ratio, choice));
        TB_TEST_REQUIRE_UINT(tb_setting_set_choice(engine.display.aspect_ratio, choice), ==,
                             TB_SETTING_OK);
        TB_TEST_EXPECT_FLOAT(tb_viewport_aspect_ratio_from_settings(&engine), ==, ratios[choice]);
    }
    tb_settings_destroy(settings);
}
