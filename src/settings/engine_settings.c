/* The engine's catalog of settings. Families and settings are added in the
 * order an options menu shows them; the descriptions are what its help and a
 * console say about each. */
#include "badger/engine_settings.h"

#include "badger/assert.h"
#include "badger/rhi.h"

#include <stddef.h>

#define TB_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* The longest language tag accepted: BCP 47 allows 35 characters in
 * practice. */
#define TB_ENGINE_LANGUAGE_TAG_LIMIT ((size_t)35)

/* The highest resolution and refresh rate a setting accepts: well beyond any
 * display, low enough that no arithmetic on them overflows. */
#define TB_ENGINE_MOST_PIXELS ((int64_t)16384)
#define TB_ENGINE_MOST_HERTZ ((int64_t)1000)

/* The display policy's built-in design size, and the longest bar color,
 * `#rrggbb`. */
#define TB_ENGINE_DESIGN_WIDTH ((int64_t)1920)
#define TB_ENGINE_DESIGN_HEIGHT ((int64_t)1080)
#define TB_ENGINE_COLOR_LIMIT ((size_t)7)
/* The built-in aspect, `expand`, by its index in tb_engine_stretch_aspects:
 * the design fills any frame, with more shown where the frame's shape is
 * not the design's. */
#define TB_ENGINE_STRETCH_EXPAND ((size_t)3)

/* The least size of a window by default: a third of 1080p, below which an
 * interface laid out for 1080p no longer reads. */
#define TB_ENGINE_LEAST_WINDOW_WIDTH ((int64_t)640)
#define TB_ENGINE_LEAST_WINDOW_HEIGHT ((int64_t)360)

static const tb_settings_family tb_engine_families[] = {
    {.name = "gameplay",
     .title = "Gameplay",
     .description = "How the game behaves around play.",
     .for_players = true},
    {.name = "display",
     .title = "Display",
     .description = "The window, the monitor, and how frames reach the screen.",
     .for_players = true},
    {.name = "graphics",
     .title = "Graphics",
     .description = "How the game renders: resolution, lighting, and quality.",
     .for_players = true},
    {.name = "audio",
     .title = "Audio",
     .description = "Volumes, and where the sound goes.",
     .for_players = true},
    {.name = "controls",
     .title = "Controls",
     .description = "The mouse, gamepads, and touch screens.",
     .for_players = true},
    {.name = "interface",
     .title = "Interface",
     .description = "The size of the interface, its font, and the HUD.",
     .for_players = true},
    {.name = "accessibility",
     .title = "Accessibility",
     .description = "Subtitles, readability, color, and motion.",
     .for_players = true},
    {.name = "language",
     .title = "Language",
     .description = "The language of text and speech.",
     .for_players = true},
    {.name = "engine",
     .title = "Engine",
     .description = "Tuning that a game ships with.",
     .for_players = false},
    {.name = "shaders",
     .title = "Shaders",
     .description = "Where shaders come from: the game's shader libraries, or the compiler.",
     .for_players = false},
    {.name = "developer",
     .title = "Developer",
     .description = "Switches for developing the game.",
     .for_players = false},
};

static const char *const tb_engine_window_modes[] = {"windowed", "borderless", "fullscreen"};
static const char *const tb_engine_vsync_modes[] = {"off", "on", "adaptive"};
/* The renderers, in the order of tb_rhi_backend and as tb_rhi_backend_name()
 * spells them. `null` is among them for servers and tests, which name it on
 * the command line; an options menu leaves it out, as Unreal's leave out its
 * null RHI. */
static const char *const tb_engine_renderers[] = {"auto",   "d3d12", "d3d11",    "vulkan",
                                                  "opengl", "metal", "software", "null"};
TB_STATIC_ASSERT(TB_COUNT_OF(tb_engine_renderers) == TB_RHI_BACKEND_COUNT,
                 "a renderer for each backend");
static const char *const tb_engine_upscalers[] = {"native", "dlss", "fsr", "xess", "metalfx"};
static const char *const tb_engine_low_latency[] = {"off", "on", "boost"};
static const char *const tb_engine_lighting[] = {"raster", "hybrid", "path_tracing"};
static const char *const tb_engine_anti_aliasing[] = {"off", "fxaa", "taa"};
static const char *const tb_engine_quality[] = {"low", "medium", "high", "ultra"};
static const char *const tb_engine_anisotropy[] = {"off", "2x", "4x", "8x", "16x"};
static const char *const tb_engine_aspect_ratios[] = {"auto", "16_9", "16_10",
                                                      "21_9", "32_9", "4_3"};
/* The display policy's choices, in the order of tb_viewport_aspect,
 * tb_viewport_scale_mode and tb_viewport_render_mode. */
static const char *const tb_engine_stretch_aspects[] = {"keep", "keep_width", "keep_height",
                                                        "expand", "ignore"};
static const char *const tb_engine_stretch_scale_modes[] = {"fractional", "integer"};
static const char *const tb_engine_stretch_render_modes[] = {"canvas", "viewport"};
static const char *const tb_engine_upscaler_modes[] = {
    "native", "quality", "balanced", "performance", "ultra_performance", "custom"};
static const char *const tb_engine_motion_blur[] = {"off", "low", "medium", "high"};
static const char *const tb_engine_speaker_modes[] = {"automatic", "stereo", "headphones",
                                                      "surround_5_1", "surround_7_1"};
static const char *const tb_engine_dynamic_ranges[] = {"wide", "standard", "night"};
static const char *const tb_engine_overlays[] = {"off", "frame_rate", "detailed"};
static const char *const tb_engine_subtitles[] = {"off", "dialogue", "captions"};
static const char *const tb_engine_subtitle_sizes[] = {"small", "medium", "large", "largest"};
static const char *const tb_engine_text_sizes[] = {"standard", "large", "largest"};
static const char *const tb_engine_color_blind_modes[] = {"off", "protanopia", "deuteranopia",
                                                          "tritanopia"};

/* The quality groups, in the order of tb_quality_group: each a choice of
 * tb_engine_quality, set together by tb_engine_settings_set_quality(). */
static const struct
{
    const char *name;
    const char *description;
} tb_engine_quality_groups[] = {
    {"graphics.texture_quality", "The detail of textures."},
    {"graphics.geometry_quality", "The detail of models, and how far it reaches."},
    {"graphics.shading_quality", "The detail of how surfaces take light."},
    {"graphics.shadow_quality", "The detail of shadows."},
    {"graphics.reflection_quality", "The detail of reflections."},
    {"graphics.light_quality", "How many lights cast light and shadows, and how far."},
    {"graphics.directional_occlusion_quality",
     "The detail of ambient occlusion: the shade of creases, corners and contact."},
    {"graphics.particle_quality", "The detail and number of particles."},
    {"graphics.decal_quality", "The detail of decals, and how long they last."},
    {"graphics.water_quality", "The detail of water."},
    {"graphics.volumetric_quality", "The detail of fog, smoke and light shafts."},
};
TB_STATIC_ASSERT(TB_COUNT_OF(tb_engine_quality_groups) == TB_QUALITY_GROUP_COUNT,
                 "a name for each quality group");

/* The indices of the defaults that are not the first choice. */
enum
{
    TB_ENGINE_UPSCALER_NATIVE = 0,
    TB_ENGINE_LOW_LATENCY_OFF = 0,
    TB_ENGINE_LIGHTING_RASTER = 0,
    TB_ENGINE_ANTI_ALIASING_TAA = 2,
    TB_ENGINE_QUALITY_HIGH = 2,
    TB_ENGINE_ANISOTROPY_8X = 3,
    TB_ENGINE_UPSCALER_MODE_QUALITY = 1,
    TB_ENGINE_SPEAKERS_AUTOMATIC = 0,
    TB_ENGINE_DYNAMIC_RANGE_STANDARD = 1,
    TB_ENGINE_OVERLAY_OFF = 0,
    TB_ENGINE_SUBTITLES_DIALOGUE = 1,
    TB_ENGINE_SUBTITLE_SIZE_MEDIUM = 1,
    TB_ENGINE_TEXT_SIZE_STANDARD = 0,
    TB_ENGINE_COLOR_BLIND_OFF = 0
};

static tb_setting *tb_engine_boolean(tb_settings *settings, const char *name,
                                     const char *description, bool value)
{
    return tb_settings_add(settings, &(tb_setting_definition){.name = name,
                                                              .description = description,
                                                              .type = TB_SETTING_BOOLEAN,
                                                              .boolean = {.default_value = value}});
}

static tb_setting *tb_engine_integer(tb_settings *settings, const char *name,
                                     const char *description, int64_t value, int64_t minimum,
                                     int64_t maximum)
{
    return tb_settings_add(
        settings, &(tb_setting_definition){
                      .name = name,
                      .description = description,
                      .type = TB_SETTING_INTEGER,
                      .integer = {.default_value = value, .minimum = minimum, .maximum = maximum}});
}

static tb_setting *tb_engine_number(tb_settings *settings, const char *name,
                                    const char *description, double value, double minimum,
                                    double maximum)
{
    return tb_settings_add(
        settings, &(tb_setting_definition){
                      .name = name,
                      .description = description,
                      .type = TB_SETTING_NUMBER,
                      .number = {.default_value = value, .minimum = minimum, .maximum = maximum}});
}

static tb_setting *tb_engine_choice(tb_settings *settings, const char *name,
                                    const char *description, const char *const *choices,
                                    size_t count, size_t value)
{
    return tb_settings_add(
        settings, &(tb_setting_definition){
                      .name = name,
                      .description = description,
                      .type = TB_SETTING_CHOICE,
                      .choice = {.names = choices, .count = count, .default_value = value}});
}

static tb_setting *tb_engine_text(tb_settings *settings, const char *name, const char *description,
                                  size_t limit)
{
    return tb_settings_add(settings,
                           &(tb_setting_definition){.name = name,
                                                    .description = description,
                                                    .type = TB_SETTING_TEXT,
                                                    .text = {.default_value = "", .limit = limit}});
}

static tb_setting *tb_engine_text_of(tb_settings *settings, const char *name,
                                     const char *description, const char *value, size_t limit)
{
    return tb_settings_add(
        settings, &(tb_setting_definition){.name = name,
                                           .description = description,
                                           .type = TB_SETTING_TEXT,
                                           .text = {.default_value = value, .limit = limit}});
}

#define TB_ENGINE_CHOICE(settings, name, description, choices, value)                              \
    tb_engine_choice((settings), (name), (description), (choices), TB_COUNT_OF(choices), (value))

static void tb_engine_add_gameplay(tb_settings *settings, tb_engine_settings *engine)
{
    engine->gameplay.pause_when_unfocused =
        tb_engine_boolean(settings, "gameplay.pause_when_unfocused",
                          "Pause the game while its window does not have the focus.", true);
}

static void tb_engine_add_display(tb_settings *settings, const tb_engine_settings_options *options,
                                  tb_engine_settings *engine)
{
    engine->display.window_mode = TB_ENGINE_CHOICE(
        settings, "display.window_mode",
        "Windowed: a window with a frame. Borderless: a window without a frame that fills the "
        "display. Fullscreen: the display in the game's own resolution, where the system has it.",
        tb_engine_window_modes,
        options->developer ? (size_t)TB_WINDOW_MODE_WINDOWED : (size_t)TB_WINDOW_MODE_BORDERLESS);
    engine->display.monitor =
        tb_engine_integer(settings, "display.monitor",
                          "The display the game uses: 0 for the primary display, then the "
                          "others in the order the system lists them.",
                          0, 0, 63);
    engine->display.window_width = tb_engine_integer(
        settings, "display.window_width",
        "The width of the window in windowed mode, before the display's scaling; 0 to fit the "
        "display.",
        0, 0, TB_ENGINE_MOST_PIXELS);
    engine->display.window_height = tb_engine_integer(
        settings, "display.window_height",
        "The height of the window in windowed mode, before the display's scaling; 0 to fit the "
        "display.",
        0, 0, TB_ENGINE_MOST_PIXELS);
    engine->display.window_min_width = tb_engine_integer(
        settings, "display.window_min_width",
        "The least width the window may be resized to in windowed mode, before the display's "
        "scaling.",
        TB_ENGINE_LEAST_WINDOW_WIDTH, 0, TB_ENGINE_MOST_PIXELS);
    engine->display.window_min_height = tb_engine_integer(
        settings, "display.window_min_height",
        "The least height the window may be resized to in windowed mode, before the display's "
        "scaling.",
        TB_ENGINE_LEAST_WINDOW_HEIGHT, 0, TB_ENGINE_MOST_PIXELS);
    engine->display.window_aspect_width =
        tb_engine_integer(settings, "display.window_aspect_width",
                          "The aspect the window keeps while it is resized in windowed mode, "
                          "width to height, as 16 to 9; 0 for any aspect.",
                          0, 0, TB_ENGINE_MOST_PIXELS);
    engine->display.window_aspect_height =
        tb_engine_integer(settings, "display.window_aspect_height",
                          "The height part of the aspect the window keeps while it is resized; "
                          "0 for any aspect.",
                          0, 0, TB_ENGINE_MOST_PIXELS);
    engine->display.fullscreen_width =
        tb_engine_integer(settings, "display.fullscreen_width",
                          "The width of the fullscreen resolution in pixels; 0 for the display's "
                          "own.",
                          0, 0, TB_ENGINE_MOST_PIXELS);
    engine->display.fullscreen_height =
        tb_engine_integer(settings, "display.fullscreen_height",
                          "The height of the fullscreen resolution in pixels; 0 for the "
                          "display's own.",
                          0, 0, TB_ENGINE_MOST_PIXELS);
    engine->display.aspect_ratio = TB_ENGINE_CHOICE(
        settings, "display.aspect_ratio",
        "The shape of the picture: auto for the display's own; another letterboxes or "
        "pillarboxes it, and narrows the resolutions offered to that shape.",
        tb_engine_aspect_ratios, 0);
    engine->display.refresh_rate =
        tb_engine_integer(settings, "display.refresh_rate",
                          "The fullscreen refresh rate in hertz; 0 for the display's own.", 0, 0,
                          TB_ENGINE_MOST_HERTZ);
    engine->display.vsync = TB_ENGINE_CHOICE(
        settings, "display.vsync",
        "Wait for the display between frames, which stops tearing. Adaptive waits unless a frame "
        "is late.",
        tb_engine_vsync_modes, (size_t)TB_VSYNC_ON);
    engine->display.frame_limit = tb_engine_integer(
        settings, "display.frame_limit",
        "The most frames a second the game renders; 0 for no limit.", 0, 0, TB_ENGINE_MOST_HERTZ);
    engine->display.background_frame_limit = tb_engine_integer(
        settings, "display.background_frame_limit",
        "The most frames a second while the window does not have the focus; 0 for no limit.", 30, 0,
        TB_ENGINE_MOST_HERTZ);
    engine->display.brightness =
        tb_engine_number(settings, "display.brightness",
                         "How bright the picture is; 1 is as the game was made.", 1.0, 0.5, 2.0);
    engine->display.hdr = tb_engine_boolean(
        settings, "display.hdr", "Output high dynamic range, where the display has it.", false);
}

/* The display policy (badger/viewport.h): what an app gives in its default
 * file, after Godot's stretch settings. */
static void tb_engine_add_display_policy(tb_settings *settings, tb_engine_settings *engine)
{
    engine->display.design_width =
        tb_engine_integer(settings, "display.design_width",
                          "The width the game is designed at, in its own units; 0, with the "
                          "height, for none: units are then the display's logical pixels.",
                          TB_ENGINE_DESIGN_WIDTH, 0, TB_ENGINE_MOST_PIXELS);
    engine->display.design_height = tb_engine_integer(
        settings, "display.design_height",
        "The height the game is designed at, in its own units; 0, with the width, for none.",
        TB_ENGINE_DESIGN_HEIGHT, 0, TB_ENGINE_MOST_PIXELS);
    engine->display.stretch_aspect = TB_ENGINE_CHOICE(
        settings, "display.stretch_aspect",
        "When the window's shape is not the design's: keep fits it whole between bars, "
        "keep_width and keep_height keep one axis and show more of the other, expand shows more "
        "of either, ignore stretches it.",
        tb_engine_stretch_aspects, TB_ENGINE_STRETCH_EXPAND);
    engine->display.stretch_scale_mode = TB_ENGINE_CHOICE(
        settings, "display.stretch_scale_mode",
        "Fractional scales the design as large as the window allows; integer by whole pixels, "
        "for pixel art.",
        tb_engine_stretch_scale_modes, 0);
    engine->display.stretch_render_mode = TB_ENGINE_CHOICE(
        settings, "display.stretch_render_mode",
        "Canvas draws at the window's own pixels, so text stays crisp; viewport renders at the "
        "design size and upscales.",
        tb_engine_stretch_render_modes, 0);
    engine->display.stretch_scale = tb_engine_number(
        settings, "display.stretch_scale",
        "A factor on the design's scale: 2 draws everything twice as large.", 1.0, 0.25, 8.0);
    engine->display.bar_color = tb_engine_text_of(
        settings, "display.bar_color", "The color of the bars beside the design, as #rrggbb.",
        "#000000", TB_ENGINE_COLOR_LIMIT);
}

static void tb_engine_add_graphics(tb_settings *settings, tb_engine_settings *engine)
{
    engine->graphics.renderer = TB_ENGINE_CHOICE(
        settings, "graphics.renderer",
        "The graphics API the game draws through; auto takes the platform's best that the "
        "machine has. One the platform does not have stops the game, saying so.",
        tb_engine_renderers, (size_t)TB_RHI_BACKEND_AUTOMATIC);
    engine->graphics.render_scale = tb_engine_number(
        settings, "graphics.render_scale",
        "The resolution the game renders at, as a fraction of the output's; the upscaler makes "
        "up the rest.",
        1.0, 0.25, 2.0);
    engine->graphics.dynamic_resolution = tb_engine_integer(
        settings, "graphics.dynamic_resolution",
        "Lower the render resolution while frames are slow, to hold this many frames a second; 0 "
        "to keep it.",
        0, 0, TB_ENGINE_MOST_HERTZ);
    engine->graphics.upscaler = TB_ENGINE_CHOICE(
        settings, "graphics.upscaler",
        "What upscales and anti-aliases the rendered picture: the engine's own, or a vendor's "
        "where the hardware has it.",
        tb_engine_upscalers, TB_ENGINE_UPSCALER_NATIVE);
    engine->graphics.upscaler_mode = TB_ENGINE_CHOICE(
        settings, "graphics.upscaler_mode",
        "How far the upscaler upscales: native renders at the output's resolution, quality to "
        "ultra_performance at less and less of it, as the vendors name them; custom at the "
        "render scale.",
        tb_engine_upscaler_modes, TB_ENGINE_UPSCALER_MODE_QUALITY);
    engine->graphics.sharpening = tb_engine_number(
        settings, "graphics.sharpening",
        "How much the picture is sharpened, by the upscaler where one runs.", 0.5, 0.0, 1.0);
    engine->graphics.frame_generation =
        tb_engine_boolean(settings, "graphics.frame_generation",
                          "Generate frames between rendered ones, where the hardware can.", false);
    engine->graphics.low_latency = TB_ENGINE_CHOICE(
        settings, "graphics.low_latency",
        "Shorten the time from input to the screen, with the GPU vendor's low-latency mode.",
        tb_engine_low_latency, TB_ENGINE_LOW_LATENCY_OFF);
    engine->graphics.lighting =
        TB_ENGINE_CHOICE(settings, "graphics.lighting",
                         "Raster, hybrid ray tracing, or path tracing, as the hardware allows.",
                         tb_engine_lighting, TB_ENGINE_LIGHTING_RASTER);
    engine->graphics.anti_aliasing = TB_ENGINE_CHOICE(
        settings, "graphics.anti_aliasing", "How edges are smoothed when nothing upscales.",
        tb_engine_anti_aliasing, TB_ENGINE_ANTI_ALIASING_TAA);
    for (size_t index = 0; index < TB_COUNT_OF(tb_engine_quality_groups); index++)
    {
        engine->graphics.quality[index] = TB_ENGINE_CHOICE(
            settings, tb_engine_quality_groups[index].name,
            tb_engine_quality_groups[index].description, tb_engine_quality, TB_ENGINE_QUALITY_HIGH);
    }
    engine->graphics.anisotropic_filtering = TB_ENGINE_CHOICE(
        settings, "graphics.anisotropic_filtering", "The sharpness of textures seen at an angle.",
        tb_engine_anisotropy, TB_ENGINE_ANISOTROPY_8X);
    engine->graphics.field_of_view = tb_engine_number(
        settings, "graphics.field_of_view",
        "How wide the view is: horizontal degrees on a 16:9 display.", 90.0, 60.0, 120.0);
    engine->graphics.motion_blur =
        TB_ENGINE_CHOICE(settings, "graphics.motion_blur", "How much what moves quickly blurs.",
                         tb_engine_motion_blur, 0);
    engine->graphics.depth_of_field = tb_engine_boolean(
        settings, "graphics.depth_of_field", "Blur what is out of focus, as a camera does.", true);
    engine->graphics.chromatic_aberration =
        tb_engine_boolean(settings, "graphics.chromatic_aberration",
                          "Fringe colors at the edges of the picture, as a lens does.", true);
    engine->graphics.film_grain = tb_engine_number(
        settings, "graphics.film_grain",
        "How much film grain the picture has: 1 as the game was made, 0 for none.", 1.0, 0.0, 1.0);
}

static void tb_engine_add_audio(tb_settings *settings, tb_engine_settings *engine)
{
    engine->audio.master_volume =
        tb_engine_number(settings, "audio.master_volume",
                         "How loud everything is, from silent to full.", 1.0, 0.0, 1.0);
    engine->audio.music_volume =
        tb_engine_number(settings, "audio.music_volume", "How loud the music is.", 1.0, 0.0, 1.0);
    engine->audio.effects_volume = tb_engine_number(settings, "audio.effects_volume",
                                                    "How loud sound effects are.", 1.0, 0.0, 1.0);
    engine->audio.ambient_volume =
        tb_engine_number(settings, "audio.ambient_volume", "How loud ambience is.", 1.0, 0.0, 1.0);
    engine->audio.voice_volume =
        tb_engine_number(settings, "audio.voice_volume", "How loud speech is.", 1.0, 0.0, 1.0);
    engine->audio.interface_volume = tb_engine_number(
        settings, "audio.interface_volume", "How loud the interface's sounds are.", 1.0, 0.0, 1.0);
    engine->audio.mute_when_unfocused =
        tb_engine_boolean(settings, "audio.mute_when_unfocused",
                          "Silence the game while its window does not have the focus.", false);
    engine->audio.output_device =
        tb_engine_text(settings, "audio.output_device",
                       "The audio device to play through; empty for the system's default.", 0);
    engine->audio.speaker_mode =
        TB_ENGINE_CHOICE(settings, "audio.speaker_mode",
                         "The speakers the sound is mixed for; automatic asks the system.",
                         tb_engine_speaker_modes, TB_ENGINE_SPEAKERS_AUTOMATIC);
    engine->audio.dynamic_range = TB_ENGINE_CHOICE(
        settings, "audio.dynamic_range",
        "The difference between quiet and loud: wide for a home theater, night to keep it "
        "small.",
        tb_engine_dynamic_ranges, TB_ENGINE_DYNAMIC_RANGE_STANDARD);
}

static void tb_engine_add_controls(tb_settings *settings, tb_engine_settings *engine)
{
    engine->controls.mouse_sensitivity =
        tb_engine_number(settings, "controls.mouse.sensitivity",
                         "How far the view turns as the mouse moves.", 1.0, 0.01, 10.0);
    engine->controls.mouse_invert_y = tb_engine_boolean(
        settings, "controls.mouse.invert_y", "Look down when the mouse moves forward.", false);
    engine->controls.mouse_raw_input =
        tb_engine_boolean(settings, "controls.mouse.raw_input",
                          "Read the mouse without the system's acceleration.", true);
    engine->controls.mouse_smoothing = tb_engine_boolean(
        settings, "controls.mouse.smoothing", "Smooth the mouse's movement over frames.", false);
    engine->controls.gamepad_horizontal_sensitivity =
        tb_engine_number(settings, "controls.gamepad.horizontal_sensitivity",
                         "How fast the view turns left and right with the stick.", 1.0, 0.1, 10.0);
    engine->controls.gamepad_vertical_sensitivity =
        tb_engine_number(settings, "controls.gamepad.vertical_sensitivity",
                         "How fast the view turns up and down with the stick.", 1.0, 0.1, 10.0);
    engine->controls.gamepad_invert_y = tb_engine_boolean(
        settings, "controls.gamepad.invert_y", "Look down when the stick is pushed up.", false);
    engine->controls.gamepad_vibration =
        tb_engine_number(settings, "controls.gamepad.vibration",
                         "How strongly the gamepad vibrates; 0 for not at all.", 1.0, 0.0, 1.0);
    engine->controls.gamepad_left_dead_zone =
        tb_engine_number(settings, "controls.gamepad.left_dead_zone",
                         "How far the left stick moves before it counts.", 0.1, 0.0, 0.9);
    engine->controls.gamepad_right_dead_zone =
        tb_engine_number(settings, "controls.gamepad.right_dead_zone",
                         "How far the right stick moves before it counts.", 0.1, 0.0, 0.9);
    engine->controls.touch_sensitivity =
        tb_engine_number(settings, "controls.touch.sensitivity",
                         "How far the view turns as a finger moves.", 1.0, 0.1, 10.0);
}

static void tb_engine_add_interface(tb_settings *settings, tb_engine_settings *engine)
{
    engine->user_interface.scale = tb_engine_number(
        settings, "interface.scale",
        "The size of the interface, as a factor on the size the game picks for the display.", 1.0,
        0.5, 2.0);
    engine->user_interface.font = tb_engine_text(
        settings, "interface.font", "The interface's font; empty for the game's own.", 0);
    engine->user_interface.hud_opacity = tb_engine_number(
        settings, "interface.hud_opacity", "How opaque the HUD is; 0 hides it.", 1.0, 0.0, 1.0);
    engine->user_interface.crosshair =
        tb_engine_boolean(settings, "interface.crosshair", "Show a crosshair.", true);
    engine->user_interface.performance_overlay =
        TB_ENGINE_CHOICE(settings, "interface.performance_overlay",
                         "Show the frame rate, or the frame rate and frame times.",
                         tb_engine_overlays, TB_ENGINE_OVERLAY_OFF);
}

static void tb_engine_add_accessibility(tb_settings *settings, tb_engine_settings *engine)
{
    engine->accessibility.subtitles =
        TB_ENGINE_CHOICE(settings, "accessibility.subtitles",
                         "Subtitles for speech, or captions for speech and sounds.",
                         tb_engine_subtitles, TB_ENGINE_SUBTITLES_DIALOGUE);
    engine->accessibility.subtitle_size =
        TB_ENGINE_CHOICE(settings, "accessibility.subtitle_size", "The size of subtitles.",
                         tb_engine_subtitle_sizes, TB_ENGINE_SUBTITLE_SIZE_MEDIUM);
    engine->accessibility.subtitle_background =
        tb_engine_number(settings, "accessibility.subtitle_background",
                         "How opaque the background behind subtitles is.", 0.5, 0.0, 1.0);
    engine->accessibility.text_size =
        TB_ENGINE_CHOICE(settings, "accessibility.text_size", "The size of the interface's text.",
                         tb_engine_text_sizes, TB_ENGINE_TEXT_SIZE_STANDARD);
    engine->accessibility.color_blind_mode = TB_ENGINE_CHOICE(
        settings, "accessibility.color_blind_mode", "Adjust colors for a kind of color blindness.",
        tb_engine_color_blind_modes, TB_ENGINE_COLOR_BLIND_OFF);
    engine->accessibility.color_blind_strength =
        tb_engine_number(settings, "accessibility.color_blind_strength",
                         "How strongly colors are adjusted for color blindness.", 1.0, 0.0, 1.0);
    engine->accessibility.camera_shake =
        tb_engine_number(settings, "accessibility.camera_shake",
                         "How much the camera shakes; 0 for not at all.", 1.0, 0.0, 1.0);
    engine->accessibility.reduce_flashing = tb_engine_boolean(
        settings, "accessibility.reduce_flashing", "Soften flashes and strobing lights.", false);
}

static void tb_engine_add_language(tb_settings *settings, tb_engine_settings *engine)
{
    engine->language.text = tb_engine_text(
        settings, "language.text",
        "The language of text, as a tag such as \"fr-FR\"; empty for the system's language.",
        TB_ENGINE_LANGUAGE_TAG_LIMIT);
    engine->language.voice =
        tb_engine_text(settings, "language.voice",
                       "The language of speech, as a tag; empty for the language of text.",
                       TB_ENGINE_LANGUAGE_TAG_LIMIT);
}

static void tb_engine_add_engine(tb_settings *settings, tb_engine_settings *engine)
{
    engine->engine.tick_rate =
        tb_engine_integer(settings, "engine.tick_rate",
                          "Simulation ticks a second; rendering interpolates between them.", 60, 1,
                          TB_ENGINE_MOST_HERTZ);
    engine->engine.max_frame_time = tb_engine_number(
        settings, "engine.max_frame_time",
        "The longest time, in seconds, that one frame counts for: after a longer stall the game "
        "carries on rather than racing to catch up.",
        0.25, 0.01, 1.0);
}

static void tb_engine_add_shaders(tb_settings *settings, const tb_engine_settings_options *options,
                                  tb_engine_settings *engine)
{
    engine->shaders.runtime_compile = tb_engine_boolean(
        settings, "shaders.runtime_compile",
        "Compile at run time the shaders that the game's shader libraries lack, where a compiler "
        "is installed; off, the game draws with its libraries' shaders alone. On in developer "
        "builds.",
        options->developer);
}

static void tb_engine_add_developer(tb_settings *settings,
                                    const tb_engine_settings_options *options,
                                    tb_engine_settings *engine)
{
    engine->developer.quit_without_asking = tb_engine_boolean(
        settings, "developer.quit_without_asking",
        "Escape and the window's close button quit at once, without asking whether to. On in "
        "developer builds.",
        options->developer);
    engine->developer.frame_count = tb_engine_integer(
        settings, "developer.frame_count",
        "Frames to run before quitting; 0 to run until asked to quit.", 0, 0, INT64_MAX);
    engine->developer.time_scale =
        tb_engine_number(settings, "developer.time_scale",
                         "How fast simulated time passes; 1 for real time.", 1.0, 0.0, 16.0);
}

/* An integer setting as a window option; every display setting's range fits. */
static uint32_t tb_engine_unsigned(const tb_setting *setting)
{
    return (uint32_t)tb_setting_integer(setting);
}

void tb_engine_settings_window_options(const tb_engine_settings *engine, const char *title,
                                       tb_window_options *options)
{
    TB_ASSERT(engine != NULL);
    TB_ASSERT(options != NULL);
    *options = (tb_window_options){
        .title = title,
        .mode = (tb_window_mode)tb_setting_choice(engine->display.window_mode),
        .display = (size_t)tb_setting_integer(engine->display.monitor),
        .width = tb_engine_unsigned(engine->display.window_width),
        .height = tb_engine_unsigned(engine->display.window_height),
        .min_width = tb_engine_unsigned(engine->display.window_min_width),
        .min_height = tb_engine_unsigned(engine->display.window_min_height),
        .aspect_width = tb_engine_unsigned(engine->display.window_aspect_width),
        .aspect_height = tb_engine_unsigned(engine->display.window_aspect_height),
        .fullscreen_width = tb_engine_unsigned(engine->display.fullscreen_width),
        .fullscreen_height = tb_engine_unsigned(engine->display.fullscreen_height),
        .refresh_rate = tb_engine_unsigned(engine->display.refresh_rate)};
}

void tb_engine_settings_add(tb_settings *settings, const tb_engine_settings_options *options,
                            tb_engine_settings *engine)
{
    static const tb_engine_settings_options release = {.developer = false};

    TB_ASSERT(settings != NULL);
    TB_ASSERT(engine != NULL);
    if (options == NULL)
    {
        options = &release;
    }
    *engine = (tb_engine_settings){0};
    for (size_t index = 0; index < TB_COUNT_OF(tb_engine_families); index++)
    {
        tb_settings_add_family(settings, &tb_engine_families[index]);
    }
    tb_engine_add_gameplay(settings, engine);
    tb_engine_add_display(settings, options, engine);
    tb_engine_add_display_policy(settings, engine);
    tb_engine_add_graphics(settings, engine);
    tb_engine_add_audio(settings, engine);
    tb_engine_add_controls(settings, engine);
    tb_engine_add_interface(settings, engine);
    tb_engine_add_accessibility(settings, engine);
    tb_engine_add_language(settings, engine);
    tb_engine_add_engine(settings, engine);
    tb_engine_add_shaders(settings, options, engine);
    tb_engine_add_developer(settings, options, engine);
}

void tb_engine_settings_device_options(const tb_engine_settings *engine,
                                       tb_rhi_device_options *options)
{
    TB_ASSERT(engine != NULL);
    TB_ASSERT(options != NULL);
    options->backend = (tb_rhi_backend)tb_setting_choice(engine->graphics.renderer);
}

uint32_t tb_engine_settings_anisotropy(const tb_engine_settings *engine)
{
    TB_ASSERT(engine != NULL);
    /* The choices are off, then 2x to 16x: each doubles the one before. */
    return 1u << tb_setting_choice(engine->graphics.anisotropic_filtering);
}

/* Anisotropic filtering follows the texture quality in a preset: 2x at low
 * to 16x at ultra, as the choices after `off` go. */
static size_t tb_engine_anisotropy_of(tb_graphics_quality quality)
{
    return (size_t)quality + 1u;
}

void tb_engine_settings_set_quality(const tb_engine_settings *engine, tb_graphics_quality quality)
{
    TB_ASSERT(engine != NULL);
    TB_ASSERT(quality < TB_GRAPHICS_QUALITY_CUSTOM);
    for (size_t index = 0; index < TB_QUALITY_GROUP_COUNT; index++)
    {
        (void)tb_setting_set_choice(engine->graphics.quality[index], (size_t)quality);
    }
    (void)tb_setting_set_choice(engine->graphics.anisotropic_filtering,
                                tb_engine_anisotropy_of(quality));
}

tb_graphics_quality tb_engine_settings_quality(const tb_engine_settings *engine)
{
    TB_ASSERT(engine != NULL);
    const size_t first = tb_setting_choice(engine->graphics.quality[0]);
    for (size_t index = 1; index < TB_QUALITY_GROUP_COUNT; index++)
    {
        if (tb_setting_choice(engine->graphics.quality[index]) != first)
        {
            return TB_GRAPHICS_QUALITY_CUSTOM;
        }
    }
    const tb_graphics_quality quality = (tb_graphics_quality)first;
    if (tb_setting_choice(engine->graphics.anisotropic_filtering) !=
        tb_engine_anisotropy_of(quality))
    {
        return TB_GRAPHICS_QUALITY_CUSTOM;
    }
    return quality;
}
