/** @file engine_settings.h
 * @brief The settings the engine defines, in the families players know from
 * the options menus of AAA games.
 *
 * tb_engine_settings_add() adds the families and settings below to a
 * registry, and fills in a structure with a pointer to each, which the
 * engine's subsystems read and an options menu changes. A game adds its own
 * settings beside them, in these families or in families of its own.
 *
 * | Family | For players | What it holds |
 * | --- | --- | --- |
 * | `gameplay` | yes | How the game behaves around play; games add difficulty and the like |
 * | `display` | yes | The window, the monitor, frame pacing and vsync, brightness and HDR |
 * | `graphics` | yes | Render resolution, upscaling, lighting, quality, field of view |
 * | `audio` | yes | Volumes, the output device and speakers, dynamic range |
 * | `controls` | yes | Mouse, gamepad and touch; key bindings belong to the input module |
 * | `interface` | yes | Interface scale, font, HUD, performance overlay |
 * | `accessibility` | yes | Subtitles, text size, color blindness, camera shake, flashing |
 * | `language` | yes | The language of text and of speech |
 * | `engine` | no | Tuning a game ships with: the tick rate, the longest frame |
 * | `developer` | no | Switches for developing the game |
 *
 * Every setting is declared here, so that the options of a game and its
 * files are complete from the start, but a subsystem that does not exist yet
 * honors none of its settings: docs/settings.md lists which settings each
 * subsystem honors.
 *
 * **Developer and release builds.** A few defaults differ: a developer build
 * starts in a window, and Escape quits it at once; a release build starts in
 * a borderless window that fills the display, and Escape asks before it
 * quits.
 *
 * @par Threading
 * tb_engine_settings_add() is thread-safe, as adding to a registry is. The
 * settings themselves are as settings.h describes.
 */
#ifndef TB_ENGINE_SETTINGS_H
#define TB_ENGINE_SETTINGS_H

#include "badger/api.h"
#include "badger/settings.h"
#include "badger/window.h"

#include <stdbool.h>

struct tb_rhi_device_options;

/** @brief The choices of `display.vsync`. */
typedef enum tb_vsync
{
    TB_VSYNC_OFF,     /**< Show each frame at once, tearing if it must. */
    TB_VSYNC_ON,      /**< Wait for the display between frames. */
    TB_VSYNC_ADAPTIVE /**< Wait for the display, unless a frame is late. */
} tb_vsync;

/** @brief The levels of a quality group and of the overall quality. */
typedef enum tb_graphics_quality
{
    TB_GRAPHICS_QUALITY_LOW,
    TB_GRAPHICS_QUALITY_MEDIUM,
    TB_GRAPHICS_QUALITY_HIGH,
    TB_GRAPHICS_QUALITY_ULTRA,
    /** The groups are at different levels: the overall quality only. */
    TB_GRAPHICS_QUALITY_CUSTOM
} tb_graphics_quality;

/**
 * @brief The quality groups: each a setting `graphics.<group>_quality` of
 * low, medium, high or ultra, which the overall quality sets together, as
 * Unreal's scalability groups are.
 */
typedef enum tb_quality_group
{
    TB_QUALITY_GROUP_TEXTURES,              /**< `graphics.texture_quality` */
    TB_QUALITY_GROUP_GEOMETRY,              /**< `graphics.geometry_quality` */
    TB_QUALITY_GROUP_SHADING,               /**< `graphics.shading_quality` */
    TB_QUALITY_GROUP_SHADOWS,               /**< `graphics.shadow_quality` */
    TB_QUALITY_GROUP_REFLECTIONS,           /**< `graphics.reflection_quality` */
    TB_QUALITY_GROUP_LIGHTS,                /**< `graphics.light_quality` */
    TB_QUALITY_GROUP_DIRECTIONAL_OCCLUSION, /**< `graphics.directional_occlusion_quality` */
    TB_QUALITY_GROUP_PARTICLES,             /**< `graphics.particle_quality` */
    TB_QUALITY_GROUP_DECALS,                /**< `graphics.decal_quality` */
    TB_QUALITY_GROUP_WATER,                 /**< `graphics.water_quality` */
    TB_QUALITY_GROUP_VOLUMETRICS,           /**< `graphics.volumetric_quality` */
    TB_QUALITY_GROUP_COUNT                  /**< The number of groups. */
} tb_quality_group;

/** @brief How the engine's settings are set up. */
typedef struct tb_engine_settings_options
{
    /** Whether the game is a developer build, which changes a few defaults;
     * see the header's description. */
    bool developer;
} tb_engine_settings_options;

/** @brief The engine's settings, as tb_engine_settings_add() fills it in.
 * Each pointer is owned by the registry. */
typedef struct tb_engine_settings
{
    /** Family `gameplay`. */
    struct
    {
        tb_setting *pause_when_unfocused; /**< Pause while the window does not have the focus. */
    } gameplay;
    /** Family `display`. */
    struct
    {
        tb_setting *window_mode; /**< A tb_window_mode. */
        tb_setting *monitor; /**< The display to use: 0 for the primary, then the system's order. */
        tb_setting *window_width; /**< A window's width, in logical units; 0 to fit the display. */
        tb_setting
            *window_height; /**< A window's height, in logical units; 0 to fit the display. */
        tb_setting *fullscreen_width;  /**< Fullscreen width in pixels; 0 for the display's own. */
        tb_setting *fullscreen_height; /**< Fullscreen height in pixels; 0 for the display's own. */
        tb_setting *aspect_ratio;      /**< auto, 16_9, 16_10, 21_9, 32_9 or 4_3. */
        tb_setting *refresh_rate; /**< Fullscreen refresh rate in hertz; 0 for the display's. */
        tb_setting *vsync;        /**< A tb_vsync. */
        tb_setting *frame_limit;  /**< Frames a second at most; 0 for no limit. */
        tb_setting *background_frame_limit; /**< The same without the focus; 0 for no limit. */
        tb_setting *brightness;             /**< Brightness, 1 as the game was made. */
        tb_setting *hdr; /**< High dynamic range output, where the display has it. */
    } display;
    /** Family `graphics`. */
    struct
    {
        /** The graphics API: auto, d3d12, d3d11, vulkan, opengl, metal,
         * software or null, in the order of tb_rhi_backend. */
        tb_setting *renderer;
        tb_setting *render_scale;       /**< Render resolution over the output's. */
        tb_setting *dynamic_resolution; /**< Frames a second to hold; 0 for a fixed resolution. */
        tb_setting *upscaler;           /**< native, dlss, fsr, xess or metalfx. */
        /** native, quality, balanced, performance, ultra_performance, or custom
         *  for `render_scale`. */
        tb_setting *upscaler_mode;
        tb_setting *sharpening;       /**< 0 to 1. */
        tb_setting *frame_generation; /**< Generated frames between rendered ones. */
        tb_setting *low_latency;      /**< off, on or boost: Reflex, Anti-Lag. */
        tb_setting *lighting;         /**< raster, hybrid or path_tracing. */
        tb_setting *anti_aliasing;    /**< off, fxaa or taa. */
        /** Each quality group, low to ultra, by its tb_quality_group. */
        tb_setting *quality[TB_QUALITY_GROUP_COUNT];
        tb_setting *anisotropic_filtering; /**< off to 16x. */
        tb_setting *field_of_view;         /**< Horizontal, in degrees, at 16:9. */
        tb_setting *motion_blur;           /**< off, low, medium or high. */
        tb_setting *depth_of_field;        /**< Blur out of focus. */
        tb_setting *chromatic_aberration;  /**< Fringe colors at the edges. */
        tb_setting *film_grain;            /**< 0 to 1, 1 as the game was made. */
    } graphics;
    /** Family `audio`. */
    struct
    {
        tb_setting *master_volume;       /**< Everything, 0 to 1. */
        tb_setting *music_volume;        /**< Music, 0 to 1. */
        tb_setting *effects_volume;      /**< Sound effects, 0 to 1. */
        tb_setting *ambient_volume;      /**< Ambience, 0 to 1. */
        tb_setting *voice_volume;        /**< Speech, 0 to 1. */
        tb_setting *interface_volume;    /**< The interface's sounds, 0 to 1. */
        tb_setting *mute_when_unfocused; /**< Silence while the window does not have the focus. */
        tb_setting *output_device;       /**< The device's name; empty for the system's. */
        tb_setting *speaker_mode; /**< automatic, stereo, headphones, surround_5_1, surround_7_1. */
        tb_setting *dynamic_range; /**< wide, standard or night. */
    } audio;
    /** Family `controls`. */
    struct
    {
        tb_setting *mouse_sensitivity; /**< `controls.mouse.sensitivity`. */
        tb_setting *mouse_invert_y;    /**< `controls.mouse.invert_y`. */
        tb_setting *mouse_raw_input;   /**< `controls.mouse.raw_input`. */
        tb_setting *mouse_smoothing;   /**< `controls.mouse.smoothing`. */
        tb_setting
            *gamepad_horizontal_sensitivity;      /**< `controls.gamepad.horizontal_sensitivity`. */
        tb_setting *gamepad_vertical_sensitivity; /**< `controls.gamepad.vertical_sensitivity`. */
        tb_setting *gamepad_invert_y;             /**< `controls.gamepad.invert_y`. */
        tb_setting *gamepad_vibration;            /**< `controls.gamepad.vibration`. */
        tb_setting *gamepad_left_dead_zone;       /**< `controls.gamepad.left_dead_zone`. */
        tb_setting *gamepad_right_dead_zone;      /**< `controls.gamepad.right_dead_zone`. */
        tb_setting *touch_sensitivity;            /**< `controls.touch.sensitivity`. */
    } controls;
    /** Family `interface`; `user_interface` here, because Windows headers make
     * `interface` a macro. */
    struct
    {
        tb_setting *scale;       /**< A factor on the scale the engine picks for the display. */
        tb_setting *font;        /**< The font's name; empty for the game's own. */
        tb_setting *hud_opacity; /**< 0 hides the HUD. */
        tb_setting *crosshair;   /**< Show a crosshair. */
        tb_setting *performance_overlay; /**< off, frame_rate or detailed. */
    } user_interface;
    /** Family `accessibility`. */
    struct
    {
        tb_setting *subtitles;            /**< off, dialogue or captions. */
        tb_setting *subtitle_size;        /**< small, medium, large or largest. */
        tb_setting *subtitle_background;  /**< The opacity behind subtitles, 0 to 1. */
        tb_setting *text_size;            /**< standard, large or largest. */
        tb_setting *color_blind_mode;     /**< off, protanopia, deuteranopia or tritanopia. */
        tb_setting *color_blind_strength; /**< 0 to 1. */
        tb_setting *camera_shake;         /**< 0 to 1. */
        tb_setting *reduce_flashing;      /**< Soften flashes and strobing. */
    } accessibility;
    /** Family `language`. */
    struct
    {
        tb_setting *text;  /**< A language tag, "fr-FR"; empty for the system's language. */
        tb_setting *voice; /**< A language tag; empty for the text's language. */
    } language;
    /** Family `engine`. */
    struct
    {
        tb_setting *tick_rate;      /**< Simulation ticks a second. */
        tb_setting *max_frame_time; /**< The longest a frame counts for, in seconds. */
    } engine;
    /** Family `developer`. */
    struct
    {
        tb_setting *quit_without_asking; /**< Escape and closing quit at once. */
        tb_setting *frame_count;         /**< Frames to run, then quit; 0 to run until asked. */
        tb_setting *time_scale;          /**< How fast simulated time passes, 1 for real time. */
    } developer;
} tb_engine_settings;

/**
 * @brief Add the engine's families and settings to a registry.
 * @param settings Required non-null. It must have none of the families
 * already.
 * @param options How to set them up; NULL for a release build.
 * @param[out] engine Receives a pointer to each setting; required.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API void tb_engine_settings_add(tb_settings *settings, const tb_engine_settings_options *options,
                                   tb_engine_settings *engine);

/**
 * @brief The window that the `display` settings describe: its mode, display,
 * windowed size, and fullscreen resolution and refresh rate.
 *
 * What a game creates its window with, and what the game loop configures it
 * with whenever those settings change.
 *
 * @param engine The engine's settings; required.
 * @param title The window's title, which the settings do not hold; NULL for
 * none, or, configuring a window, for the title it has.
 * @param[out] options Receives the options; required.
 */
TB_API void tb_engine_settings_window_options(const tb_engine_settings *engine, const char *title,
                                              tb_window_options *options);

/**
 * @brief The device that the `graphics` settings describe: the backend that
 * `graphics.renderer` names, as rhi.h's tb_rhi_device_create() takes it.
 *
 * Sets only what the settings describe, so a caller fills in the rest of the
 * options (the adapter, validation, the log) before or after. A backend that
 * this platform does not build is still a value the setting holds, since
 * settings files travel between machines: tb_rhi_device_create() reports it.
 *
 * @param engine The engine's settings; required.
 * @param[in,out] options The device's options; required. Its backend is set.
 * @par Threading Thread-safe.
 */
TB_API void tb_engine_settings_device_options(const tb_engine_settings *engine,
                                              struct tb_rhi_device_options *options);

/**
 * @brief Set the overall quality: every quality group to `quality`, and
 * anisotropic filtering from 2x at low to 16x at ultra, in the runtime
 * layer, as an options menu's overall quality does.
 *
 * As in Unreal, the overall quality is not a setting of its own: it is what
 * the groups are, so a player who changes one group has a custom quality,
 * and their file keeps each group as they chose it.
 *
 * @param engine The engine's settings; required.
 * @param quality A level from low to ultra; not custom.
 * @par Threading Thread-safe, as setting each setting is; a reader may see
 * some groups changed before the others.
 */
TB_API void tb_engine_settings_set_quality(const tb_engine_settings *engine,
                                           tb_graphics_quality quality);

/**
 * @brief The overall quality: the level every quality group is at, with
 * anisotropic filtering at that level's, or custom when they differ.
 * @param engine The engine's settings; required.
 * @return A tb_graphics_quality.
 * @par Threading Thread-safe.
 */
TB_API tb_graphics_quality tb_engine_settings_quality(const tb_engine_settings *engine);

#endif
