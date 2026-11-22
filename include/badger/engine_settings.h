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

#include <stdbool.h>

/** @brief The choices of `display.window_mode`. */
typedef enum tb_window_mode
{
    TB_WINDOW_MODE_WINDOWED,   /**< A window with a frame, that the player moves and sizes. */
    TB_WINDOW_MODE_BORDERLESS, /**< A window without a frame that fills its display. */
    TB_WINDOW_MODE_FULLSCREEN  /**< The display in a mode of the game's own, the game's alone. */
} tb_window_mode;

/** @brief The choices of `display.vsync`. */
typedef enum tb_vsync
{
    TB_VSYNC_OFF,     /**< Show each frame at once, tearing if it must. */
    TB_VSYNC_ON,      /**< Wait for the display between frames. */
    TB_VSYNC_ADAPTIVE /**< Wait for the display, unless a frame is late. */
} tb_vsync;

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
        tb_setting *render_scale;          /**< Render resolution over the output's. */
        tb_setting *upscaler;              /**< native, dlss, fsr, xess or metalfx. */
        tb_setting *frame_generation;      /**< Generated frames between rendered ones. */
        tb_setting *low_latency;           /**< off, on or boost: Reflex, Anti-Lag. */
        tb_setting *lighting;              /**< raster, hybrid or path_tracing. */
        tb_setting *anti_aliasing;         /**< off, fxaa or taa. */
        tb_setting *texture_quality;       /**< low to ultra. */
        tb_setting *shadow_quality;        /**< low to ultra. */
        tb_setting *effects_quality;       /**< low to ultra. */
        tb_setting *anisotropic_filtering; /**< off to 16x. */
        tb_setting *field_of_view;         /**< Horizontal, in degrees, at 16:9. */
        tb_setting *motion_blur;           /**< Blur with motion. */
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
        tb_setting *quit_on_escape; /**< Escape quits at once, without asking. */
        tb_setting *frame_count;    /**< Frames to run, then quit; 0 to run until asked. */
        tb_setting *time_scale;     /**< How fast simulated time passes, 1 for real time. */
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

#endif
