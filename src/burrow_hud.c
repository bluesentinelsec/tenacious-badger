/* Burrow's HUD: the frame rate, the controls, and the crosshair. */
#include "burrow_hud.h"

#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/font.h"
#include "badger/font_catalog.h"
#include "badger/memory.h"
#include "badger/settings.h"
#include "badger/str.h"

/* How much of each frame's time goes into the average. */
#define BURROW_RATE_WEIGHT 0.1

/* In logical units: the text's size, the margin from the window's edges and
 * around the text, and the rounding of the panels behind it. */
#define BURROW_HUD_TEXT_SIZE 14.0f
#define BURROW_HUD_MARGIN 10.0f
#define BURROW_HUD_PADDING 6.0f
#define BURROW_HUD_ROUNDING 6.0f
/* The crosshair: each arm's length and thickness, and the gap in its
 * middle. */
#define BURROW_CROSSHAIR_ARM 8.0f
#define BURROW_CROSSHAIR_THICKNESS 2.0f
#define BURROW_CROSSHAIR_GAP 4.0f
/* The canvas layer the HUD is drawn on, over anything else 2D. */
#define BURROW_HUD_LAYER 1000
/* The panels' opacity at a `hud_opacity` of 1. */
#define BURROW_HUD_PANEL_ALPHA 0.55f

#define BURROW_HUD_LINE_CAPACITY 96

static const char burrow_controls_looking[] =
    "WASD move    Space / Ctrl up and down    Shift sprint    Esc frees the mouse";
static const char burrow_controls_free[] = "Click to look around    Esc quits";

struct burrow_hud
{
    tb_font_catalog *catalog; /* Holds the font's bytes. */
    tb_font *font;            /* The UI role's; NULL in a build without fonts. */
    burrow_frame_rate rate;
    char rate_line[BURROW_HUD_LINE_CAPACITY];
};

bool burrow_frame_rate_add(burrow_frame_rate *rate, double seconds)
{
    if (rate->average == 0.0)
    {
        rate->average = seconds;
    }
    else
    {
        rate->average += (seconds - rate->average) * BURROW_RATE_WEIGHT;
    }
    rate->since += seconds;
    if (rate->has_shown && rate->since < BURROW_RATE_SHOWN_EVERY)
    {
        return false;
    }
    rate->shown = rate->average;
    rate->since = 0.0;
    rate->has_shown = true;
    return true;
}

double burrow_frame_rate_per_second(const burrow_frame_rate *rate)
{
    if (rate->shown <= 0.0)
    {
        return 0.0;
    }
    return 1.0 / rate->shown;
}

burrow_hud *burrow_hud_create(void)
{
    burrow_hud *const hud = tb_calloc(1, sizeof(*hud));
    const void *bytes = NULL;
    size_t length = 0;
    size_t index = 0;

    hud->catalog = tb_font_catalog_create(NULL);
    if (!tb_font_catalog_find_role(hud->catalog, TB_FONT_ROLE_UI, &index))
    {
        (void)tb_fmt_print("burrow: this build of the engine carries no UI font; the HUD has no "
                           "text\n");
        return hud;
    }
    tb_font_catalog_get_bytes(hud->catalog, index, &bytes, &length);
    /* coverage-off: the catalog's own fonts open; only a damaged build's would not */
    if (tb_font_open(bytes, length, NULL, &hud->font, NULL) != TB_FONT_OK)
    {
        hud->font = NULL;
    }
    /* coverage-on */
    return hud;
}

void burrow_hud_destroy(burrow_hud *hud, tb_renderer *renderer)
{
    if (hud == NULL)
    {
        return;
    }
    if (hud->font != NULL)
    {
        tb_canvas_forget_font(renderer, hud->font);
        tb_font_close(hud->font);
    }
    tb_font_catalog_destroy(hud->catalog);
    tb_free(hud);
}

void burrow_hud_count_frame(burrow_hud *hud, double seconds)
{
    (void)burrow_frame_rate_add(&hud->rate, seconds);
}

/* A line of text on a panel, its top left at `at`. */
static void burrow_hud_text(const burrow_hud *hud, tb_renderer *renderer, const char *line,
                            tb_vec2 at, float opacity)
{
    const tb_text text = tb_text_from_cstr(line);
    const tb_vec2 size =
        tb_canvas_measure_text(renderer, hud->font, text, BURROW_HUD_TEXT_SIZE, NULL);
    const tb_canvas_rect panel = {at.x - BURROW_HUD_PADDING, at.y - BURROW_HUD_PADDING,
                                  size.x + BURROW_HUD_PADDING * 2.0f,
                                  size.y + BURROW_HUD_PADDING * 2.0f};

    tb_canvas_fill_rounded_rect(renderer, panel, BURROW_HUD_ROUNDING,
                                (tb_vec4){0.0f, 0.0f, 0.0f, BURROW_HUD_PANEL_ALPHA * opacity});
    tb_canvas_draw_text(renderer, hud->font, text, BURROW_HUD_TEXT_SIZE, at,
                        (tb_vec4){0.92f, 0.95f, 1.0f, opacity}, NULL);
}

/* The frame rate's line as `interface.performance_overlay` asks; empty for
 * none. */
static void burrow_hud_rate_line(burrow_hud *hud, const tb_engine_settings *engine)
{
    const tb_setting *const overlay = engine->user_interface.performance_overlay;
    const char *const choice = tb_setting_choice_name(overlay, tb_setting_choice(overlay));
    const double per_second = burrow_frame_rate_per_second(&hud->rate);

    hud->rate_line[0] = '\0';
    if (tb_str_equal(choice, "frame_rate"))
    {
        (void)tb_fmt_buffer(hud->rate_line, sizeof(hud->rate_line), "%.0f fps", per_second);
    }
    else if (tb_str_equal(choice, "detailed"))
    {
        (void)tb_fmt_buffer(hud->rate_line, sizeof(hud->rate_line), "%.0f fps    %.2f ms",
                            per_second, hud->rate.shown * 1000.0);
    }
}

static void burrow_hud_crosshair(tb_renderer *renderer, tb_vec2 middle, float opacity)
{
    const tb_vec4 color = {1.0f, 1.0f, 1.0f, 0.85f * opacity};
    const float reach = BURROW_CROSSHAIR_GAP + BURROW_CROSSHAIR_ARM;
    const float half = BURROW_CROSSHAIR_THICKNESS * 0.5f;

    tb_canvas_fill_rect(renderer,
                        (tb_canvas_rect){middle.x - reach, middle.y - half, BURROW_CROSSHAIR_ARM,
                                         BURROW_CROSSHAIR_THICKNESS},
                        color);
    tb_canvas_fill_rect(renderer,
                        (tb_canvas_rect){middle.x + BURROW_CROSSHAIR_GAP, middle.y - half,
                                         BURROW_CROSSHAIR_ARM, BURROW_CROSSHAIR_THICKNESS},
                        color);
    tb_canvas_fill_rect(renderer,
                        (tb_canvas_rect){middle.x - half, middle.y - reach,
                                         BURROW_CROSSHAIR_THICKNESS, BURROW_CROSSHAIR_ARM},
                        color);
    tb_canvas_fill_rect(renderer,
                        (tb_canvas_rect){middle.x - half, middle.y + BURROW_CROSSHAIR_GAP,
                                         BURROW_CROSSHAIR_THICKNESS, BURROW_CROSSHAIR_ARM},
                        color);
}

void burrow_hud_draw(burrow_hud *hud, tb_renderer *renderer, const tb_engine_settings *engine,
                     bool captured)
{
    const float opacity = (float)tb_setting_number(engine->user_interface.hud_opacity);
    const tb_canvas_view view = tb_canvas_view_logical(renderer);
    const char *const controls = captured ? burrow_controls_looking : burrow_controls_free;
    uint32_t width = 0;
    uint32_t height = 0;
    tb_vec2 extent = {0};

    if (opacity <= 0.0f)
    {
        return;
    }
    tb_renderer_size(renderer, &width, &height);
    extent = (tb_vec2){(float)width / view.scale, (float)height / view.scale};
    tb_canvas_set_view(renderer, &view);
    tb_canvas_set_layer(renderer, BURROW_HUD_LAYER);
    if (tb_setting_boolean(engine->user_interface.crosshair))
    {
        burrow_hud_crosshair(renderer, tb_vec2_scale(extent, 0.5f), opacity);
    }
    if (hud->font == NULL)
    {
        return;
    }
    burrow_hud_rate_line(hud, engine);
    if (hud->rate_line[0] != '\0')
    {
        burrow_hud_text(hud, renderer, hud->rate_line,
                        (tb_vec2){BURROW_HUD_MARGIN + BURROW_HUD_PADDING,
                                  BURROW_HUD_MARGIN + BURROW_HUD_PADDING},
                        opacity);
    }
    const tb_vec2 size = tb_canvas_measure_text(renderer, hud->font, tb_text_from_cstr(controls),
                                                BURROW_HUD_TEXT_SIZE, NULL);
    burrow_hud_text(hud, renderer, controls,
                    (tb_vec2){BURROW_HUD_MARGIN + BURROW_HUD_PADDING,
                              extent.y - BURROW_HUD_MARGIN - BURROW_HUD_PADDING - size.y},
                    opacity);
}
