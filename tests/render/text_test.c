/* Text on the canvas, on the null backend: a glyph quad for each glyph
 * with ink, measured as the font module measures, labels at points in the
 * world, and the glyph atlas cleared partway through a frame or too small
 * for a frame's glyphs. text_d3d12_test.c checks the pixels. */
#include "badger/assert.h"
#include "badger/canvas.h"
#include "badger/font.h"
#include "badger/font_catalog.h"
#include "badger/font_layout.h"
#include "badger/str.h"
#include "badger/window.h"

#include "tb_test.h"
#include "test_data.h"

extern const tb_test_file tb_font_files[];
extern const size_t tb_font_files_count;

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
    tb_font *font;
} screen;

static tb_font *open_font(const char *name)
{
    const tb_test_file *found = NULL;
    for (size_t index = 0; index < tb_font_files_count; index++)
    {
        if (tb_str_equal(tb_font_files[index].name, name))
        {
            found = &tb_font_files[index];
        }
    }
    TB_TEST_REQUIRE_POINTER(found, !=, NULL);
    tb_font *font = NULL;
    TB_TEST_REQUIRE_UINT(tb_font_open(found->bytes, found->length, NULL, &font, NULL), ==,
                         TB_FONT_OK);
    return font;
}

/* A window, a null renderer on it with a glyph atlas of a size, and Roboto;
 * or a skipped case where there is no window. */
static void open_screen(screen *opened, uint32_t atlas)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};

    *opened = (screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(
            opened->system,
            &(tb_window_options){.title = "text", .width = 160, .height = 120, .fixed_size = true},
            &opened->window, NULL),
        ==, TB_WINDOW_OK);
    options.window = opened->window;
    options.device = (tb_rhi_device_options){.backend = TB_RHI_BACKEND_NULL};
    options.glyph_atlas_size = atlas;
    TB_TEST_REQUIRE_UINT(tb_renderer_create(&options, &opened->renderer, &error), ==, TB_RHI_OK);
    opened->font = open_font("Roboto-Regular.ttf");
}

static void close_screen(screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    tb_font_close(opened->font);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

static void begin(tb_renderer *renderer)
{
    tb_camera camera = tb_camera_perspective(1.2f);
    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 0, 5}, (tb_vec3){0, 0, 0}));
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
}

static tb_canvas_stats end(tb_renderer *renderer)
{
    tb_canvas_stats stats = {0};
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
    tb_canvas_stats_of(renderer, &stats);
    return stats;
}

/* Each glyph with ink is one quad; spaces, and text of no alpha or no
 * size, draw nothing. */
TB_TEST(canvas_text, each_glyph_with_ink_is_a_quad)
{
    screen opened = {0};
    tb_canvas_stats stats = {0};
    const tb_font_layout_options wrapped = {.wrap_width = 30.0f, .align = TB_FONT_ALIGN_RIGHT};

    open_screen(&opened, 0);
    begin(opened.renderer);
    tb_canvas_draw_text(opened.renderer, opened.font, TB_TEXT("Hi there"), 16.0f, (tb_vec2){4, 4},
                        (tb_vec4){1, 1, 1, 1}, NULL);
    tb_canvas_draw_text(opened.renderer, opened.font, TB_TEXT("wrap these words"), 12.0f,
                        (tb_vec2){4, 40}, (tb_vec4){1, 1, 1, 1}, &wrapped);
    tb_canvas_draw_text(opened.renderer, opened.font, TB_TEXT("unseen"), 16.0f, (tb_vec2){4, 4},
                        (tb_vec4){1, 1, 1, 0}, NULL);
    tb_canvas_draw_text(opened.renderer, opened.font, TB_TEXT("unseen"), 0.0f, (tb_vec2){4, 4},
                        (tb_vec4){1, 1, 1, 1}, NULL);
    tb_canvas_fill_rect(opened.renderer, (tb_canvas_rect){0, 0, 4, 4}, (tb_vec4){1, 0, 0, 1});
    stats = end(opened.renderer);
    TB_TEST_EXPECT_UINT(stats.glyphs, ==, 7 + 14);
    TB_TEST_EXPECT_UINT(stats.quads, ==, 7 + 14 + 1);
    TB_TEST_EXPECT_UINT(stats.dropped, ==, 0);
    TB_TEST_EXPECT_UINT(stats.draws, ==, 1);

    /* The next frame draws again from the atlas, as many. */
    begin(opened.renderer);
    tb_canvas_draw_text(opened.renderer, opened.font, TB_TEXT("Hi there"), 16.0f, (tb_vec2){4, 4},
                        (tb_vec4){1, 1, 1, 1}, NULL);
    stats = end(opened.renderer);
    TB_TEST_EXPECT_UINT(stats.glyphs, ==, 7);
    close_screen(&opened);
}

/* Text measures in the view's units as the font module measures it in
 * pixels, at the view's scale. */
TB_TEST(canvas_text, text_measures_in_the_view_units)
{
    screen opened = {0};
    const tb_canvas_view doubled = {.scale = 2.0f};
    const tb_font_layout_options options = {.wrap_width = 50.0f};
    tb_font_text_size pixels = {0};
    tb_vec2 units = {0};

    open_screen(&opened, 0);
    begin(opened.renderer);
    tb_canvas_set_view(opened.renderer, &doubled);
    units = tb_canvas_measure_text(opened.renderer, opened.font, TB_TEXT("Measure me, please"),
                                   10.0f, &options);
    tb_font_measure_text(opened.font, TB_TEXT("Measure me, please"), 20.0f,
                         &(tb_font_layout_options){.wrap_width = 100.0f}, &pixels);
    TB_TEST_EXPECT_FLOAT(units.x, ==, pixels.width / 2.0f);
    TB_TEST_EXPECT_FLOAT(units.y, ==, pixels.height / 2.0f);
    TB_TEST_EXPECT_UINT(pixels.line_count, >, 1);
    (void)end(opened.renderer);
    close_screen(&opened);
}

/* A label is drawn where its point is in front of the camera, in the
 * debug font, or until one is chosen in the catalog's UI font; behind the
 * camera, it is not. */
TB_TEST(canvas_text, labels_are_drawn_at_points_in_front_of_the_camera)
{
    screen opened = {0};
    tb_canvas_stats stats = {0};

    open_screen(&opened, 0);
    begin(opened.renderer);
    tb_debug_text(opened.renderer, (tb_vec3){0, 0, 0}, TB_TEXT("origin"), (tb_vec4){1, 1, 1, 1});
    stats = end(opened.renderer);
    /* A build that carries no fonts has no default to draw labels in. */
    TB_TEST_EXPECT_UINT(stats.glyphs, ==, tb_font_embedded_count() > 0 ? 6u : 0u);

    tb_renderer_set_debug_font(opened.renderer, opened.font, 12.0f);
    begin(opened.renderer);
    tb_debug_text(opened.renderer, (tb_vec3){0, 0, 0}, TB_TEXT("origin"), (tb_vec4){1, 1, 1, 1});
    tb_debug_text(opened.renderer, (tb_vec3){0, 0, 10}, TB_TEXT("behind"), (tb_vec4){1, 1, 1, 1});
    tb_debug_text(opened.renderer, (tb_vec3){1, 0, 0}, TB_TEXT("clear"), (tb_vec4){1, 1, 1, 0});
    tb_debug_text(opened.renderer, (tb_vec3){1, 0, 0}, (tb_text){0}, (tb_vec4){1, 1, 1, 1});
    stats = end(opened.renderer);
    TB_TEST_EXPECT_UINT(stats.glyphs, ==, 6);

    /* Many labels, more than their first room. */
    begin(opened.renderer);
    for (int index = 0; index < 100; index++)
    {
        tb_debug_text(opened.renderer, (tb_vec3){0, 0, 0},
                      TB_TEXT("a label long enough to fill the first kilobyte of label text"),
                      (tb_vec4){1, 1, 1, 1});
    }
    stats = end(opened.renderer);
    TB_TEST_EXPECT_UINT(stats.glyphs, >, 100);
    tb_renderer_set_debug_font(opened.renderer, NULL, 12.0f);
    begin(opened.renderer);
    tb_debug_text(opened.renderer, (tb_vec3){0, 0, 0}, TB_TEXT("default"), (tb_vec4){1, 1, 1, 1});
    stats = end(opened.renderer);
    TB_TEST_EXPECT_UINT(stats.glyphs, ==, tb_font_embedded_count() > 0 ? 7u : 0u);
    close_screen(&opened);
}

/* When the atlas clears partway through a frame, the glyphs placed before
 * are found again and drawn; when a frame's glyphs cannot all fit, those
 * that do not are not drawn, and are counted as dropped. */
TB_TEST(canvas_text, glyphs_survive_the_atlas_clearing_within_a_frame)
{
    screen opened = {0};
    tb_canvas_stats stats = {0};

    open_screen(&opened, 128);
    /* A large glyph fills most of the atlas. */
    begin(opened.renderer);
    tb_canvas_draw_text(opened.renderer, opened.font, TB_TEXT("@"), 100.0f, (tb_vec2){0, 0},
                        (tb_vec4){1, 1, 1, 1}, NULL);
    stats = end(opened.renderer);
    TB_TEST_EXPECT_UINT(stats.glyphs, ==, 1);
    /* A small glyph fits beside it; another large one does not, and clears
     * the atlas, after which the small one is placed again. */
    begin(opened.renderer);
    tb_canvas_draw_text(opened.renderer, opened.font, TB_TEXT("a"), 12.0f, (tb_vec2){0, 0},
                        (tb_vec4){1, 1, 1, 1}, NULL);
    tb_canvas_draw_text(opened.renderer, opened.font, TB_TEXT("@"), 90.0f, (tb_vec2){0, 0},
                        (tb_vec4){1, 1, 1, 1}, NULL);
    stats = end(opened.renderer);
    TB_TEST_EXPECT_UINT(stats.glyphs, ==, 2);
    TB_TEST_EXPECT_UINT(stats.dropped, ==, 0);

    /* Three glyphs that no atlas of this size holds together. */
    begin(opened.renderer);
    tb_canvas_draw_text(opened.renderer, opened.font, TB_TEXT("@#%"), 100.0f, (tb_vec2){0, 0},
                        (tb_vec4){1, 1, 1, 1}, NULL);
    stats = end(opened.renderer);
    TB_TEST_EXPECT_UINT(stats.glyphs, <, 3);
    TB_TEST_EXPECT_UINT(stats.glyphs + stats.dropped, ==, 3);

    /* Forgetting a font clears the atlas; text is drawn again after. */
    tb_canvas_forget_font(opened.renderer, opened.font);
    begin(opened.renderer);
    tb_canvas_draw_text(opened.renderer, opened.font, TB_TEXT("ok"), 16.0f, (tb_vec2){0, 0},
                        (tb_vec4){1, 1, 1, 1}, NULL);
    stats = end(opened.renderer);
    TB_TEST_EXPECT_UINT(stats.glyphs, ==, 2);
    close_screen(&opened);
}

static void forget_inside_a_frame(void *context)
{
    screen *const opened = context;
    tb_canvas_forget_font(opened->renderer, opened->font);
}

/* Forgetting a font while a frame may hold its glyphs is a contract
 * violation, caught. */
TB_TEST(canvas_text, forgetting_a_font_inside_a_frame_traps)
{
    screen opened = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    open_screen(&opened, 0);
    begin(opened.renderer);
    TB_TEST_EXPECT_TRAP(forget_inside_a_frame, &opened, "a font forgotten inside a frame");
    (void)end(opened.renderer);
    close_screen(&opened);
}
