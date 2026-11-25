/* The display policy and the player's aspect ratio, as the engine's
 * settings describe them. */
#include "badger/assert.h"
#include "badger/engine_settings.h"
#include "badger/viewport.h"

#include <stddef.h>

/* The choices of `display.aspect_ratio`, in the order engine_settings.c
 * declares them, as widths over heights; `auto` is none. 21:9 is the
 * 2560 by 1080 of those displays, 64:27. */
static const float tb_viewport_ratios[] = {0.0f,          16.0f / 9.0f, 16.0f / 10.0f,
                                           64.0f / 27.0f, 32.0f / 9.0f, 4.0f / 3.0f};
#define TB_VIEWPORT_RATIO_COUNT (sizeof(tb_viewport_ratios) / sizeof(tb_viewport_ratios[0]))

/* Room for `#rrggbb` and its terminator, and a byte to see a longer text. */
#define TB_VIEWPORT_COLOR_CAPACITY 9u

void tb_viewport_policy_from_settings(const tb_engine_settings *engine, tb_viewport_policy *policy)
{
    char color[TB_VIEWPORT_COLOR_CAPACITY] = {0};

    TB_ASSERT(engine != NULL && policy != NULL);
    *policy = (tb_viewport_policy)TB_VIEWPORT_POLICY_INIT;
    /* The settings' ranges fit in 32 bits, and their choices in the enums. */
    policy->design_width = (uint32_t)tb_setting_integer(engine->display.design_width);
    policy->design_height = (uint32_t)tb_setting_integer(engine->display.design_height);
    policy->aspect = (tb_viewport_aspect)tb_setting_choice(engine->display.stretch_aspect);
    policy->scale_mode =
        (tb_viewport_scale_mode)tb_setting_choice(engine->display.stretch_scale_mode);
    policy->render_mode =
        (tb_viewport_render_mode)tb_setting_choice(engine->display.stretch_render_mode);
    policy->scale = (float)tb_setting_number(engine->display.stretch_scale);
    (void)tb_setting_text(engine->display.bar_color, color, sizeof(color));
    if (!tb_viewport_parse_color(color, &policy->bar_color))
    {
        policy->bar_color = (tb_vec4){0.0f, 0.0f, 0.0f, 1.0f};
    }
}

float tb_viewport_aspect_ratio_from_settings(const tb_engine_settings *engine)
{
    TB_ASSERT(engine != NULL);
    const size_t choice = tb_setting_choice(engine->display.aspect_ratio);
    return choice < TB_VIEWPORT_RATIO_COUNT ? tb_viewport_ratios[choice] : 0.0f;
}
