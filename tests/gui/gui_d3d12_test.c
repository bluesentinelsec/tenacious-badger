/* The retained GUI drawn on D3D12, on WARP, where Windows builds it: a row
 * of cells of equal weight across a width no scale divides evenly lands on
 * whole pixels, each pixel of one cell's color or the next's, with neither
 * a gap nor a blended edge between them, at a scale of 1 and of 1.5. */
#include "badger/canvas.h"
#include "badger/gui.h"
#include "badger/window.h"

#include "tb_test.h"

#include <math.h>
#include <stddef.h>

/* The backend the case runs on: each runs on D3D12, then on D3D11. */
static tb_rhi_backend tb_test_backend = TB_RHI_BACKEND_D3D12;

/* Cells across the row. */
#define CELLS 7

/* A kind filled with its color. */
typedef struct swatch
{
    tb_vec4 color;
} swatch;

static void swatch_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const swatch *const filled = data;

    (void)widget;
    tb_gui_paint_fill(paint, tb_gui_paint_rect(paint), 0.0f, filled->color);
}

static const tb_gui_property swatch_properties[] = {
    {.name = "color", .type = TB_GUI_TYPE_COLOR, .offset = offsetof(swatch, color)},
};

static const tb_widget_class swatch_kind = {.name = "swatch",
                                            .data_size = sizeof(swatch),
                                            .properties = swatch_properties,
                                            .property_count = 1,
                                            .draw = swatch_draw};

static uint32_t pixel_at(const uint8_t *pixels, uint32_t width, uint32_t x, uint32_t y)
{
    const uint8_t *const bgra = &pixels[((size_t)y * width + x) * 4u];
    return (uint32_t)bgra[0] | (uint32_t)bgra[1] << 8 | (uint32_t)bgra[2] << 16 |
           (uint32_t)bgra[3] << 24;
}

/* Draw the row at a scale and check its pixels. Returns false when the
 * device cannot draw the canvas. */
static bool draw_row(tb_renderer *renderer, float scale)
{
    const tb_camera camera = tb_camera_orthographic(10.0f);
    tb_gui *const gui = tb_gui_create(NULL);
    const tb_widget root = tb_gui_root(gui);
    const uint32_t red = 0xFFFF0000u;
    const uint32_t green = 0xFF00FF00u;
    uint32_t width = 0;
    uint32_t height = 0;
    tb_canvas_stats stats = {0};

    tb_renderer_size(renderer, &width, &height);
    tb_gui_resize(gui, width, height, scale);
    for (size_t index = 0; index < CELLS; index++)
    {
        const tb_widget cell = tb_widget_create(root, &swatch_kind);
        const tb_gui_value color =
            tb_gui_color(index % 2 == 0 ? (tb_vec4){1, 0, 0, 1} : (tb_vec4){0, 1, 0, 1});
        TB_TEST_REQUIRE(tb_widget_set(cell, "color", &color));
        TB_TEST_REQUIRE(
            tb_grid(cell, &(tb_grid_options){.column = (int32_t)index, .sticky = TB_STICKY_NSEW}));
        TB_TEST_REQUIRE(tb_grid_column_configure(root, index, &(tb_grid_track){.weight = 1}));
    }
    TB_TEST_REQUIRE(tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1}));
    tb_gui_update(gui, 0.0);

    tb_renderer_capture_next_frame(renderer);
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_gui_draw(gui, renderer);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
    tb_canvas_stats_of(renderer, &stats);
    if (stats.draws == 0)
    {
        tb_gui_destroy(gui);
        return false;
    }
    const uint8_t *const pixels = tb_renderer_captured(renderer, &width, &height);
    TB_TEST_REQUIRE(pixels != NULL);

    /* Every pixel across the middle row is one cell's color: no black of a
     * gap, no blend of an edge between pixels. The colors change once at
     * each edge, where the cell's rectangle says, on a whole pixel. */
    size_t changes = 0;
    uint32_t previous = pixel_at(pixels, width, 0, height / 2);
    TB_TEST_EXPECT_HEX(previous, ==, red);
    for (uint32_t x = 0; x < width; x++)
    {
        const uint32_t here = pixel_at(pixels, width, x, height / 2);
        TB_TEST_CONTEXT("scale %.2f, pixel %u", (double)scale, (unsigned)x);
        TB_TEST_EXPECT(here == red || here == green);
        if (here != previous)
        {
            changes++;
            const tb_widget cell = tb_widget_first_child(root);
            tb_widget at = cell;
            for (size_t index = 0; index < changes; index++)
            {
                at = tb_widget_next_sibling(at);
            }
            TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(at).x * scale, (float)x, 1e-3f);
        }
        previous = here;
    }
    TB_TEST_EXPECT_UINT(changes, ==, CELLS - 1);
    /* The top and bottom rows too: the cells fill the frame. */
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 0, 0), ==, red);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, width - 1, height - 1), ==, red);
    tb_gui_destroy(gui);
    return true;
}

static void cells_land_on_whole_pixels_with_no_gaps_case(void)
{
    tb_window_system *system = NULL;
    tb_window *window = NULL;
    tb_renderer *renderer = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};

    if (tb_window_system_open(NULL, &system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(
            system,
            &(tb_window_options){.title = "gui", .width = 160, .height = 120, .fixed_size = true},
            &window, NULL),
        ==, TB_WINDOW_OK);
    options.window = window;
    options.device = (tb_rhi_device_options){
        .backend = tb_test_backend, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
    if (tb_renderer_create(&options, &renderer, &error) != TB_RHI_OK)
    {
        tb_window_destroy(window);
        tb_window_system_close(system);
        TB_TEST_SKIP("no renderer: %s", error.message);
    }
    const bool drawn = draw_row(renderer, 1.0f) && draw_row(renderer, 1.5f);
    tb_renderer_destroy(renderer);
    tb_window_destroy(window);
    tb_window_system_close(system);
    if (!drawn)
    {
        TB_TEST_SKIP("no bindless resources on this device");
    }
}

TB_TEST(gui_frame, cells_land_on_whole_pixels_with_no_gaps_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    cells_land_on_whole_pixels_with_no_gaps_case();
}

TB_TEST(gui_frame, cells_land_on_whole_pixels_with_no_gaps_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    cells_land_on_whole_pixels_with_no_gaps_case();
}
