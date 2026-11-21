/** @file game_loop.h
 * @brief The game loop: a simulation that ticks at a fixed rate, frames
 * that render between ticks as fast as the settings allow, pausing, settings
 * applied as they change, and quitting as players expect.
 *
 * @code
 * static void tick(void *context, const tb_game_time *time)   { simulate(time->tick_seconds); }
 * static void frame(void *context, const tb_game_time *time)  { render(time->alpha); }
 *
 * tb_game_loop *const loop = tb_game_loop_create(&(tb_game_loop_options){
 *     .system = system, .window = window, .settings = &engine,
 *     .callbacks = {.tick = tick, .frame = frame}});
 * tb_game_loop_run(loop);
 * tb_game_loop_destroy(loop);
 * @endcode
 *
 * **Ticks and frames.** The simulation advances in ticks of a fixed length,
 * `engine.tick_rate` of them a second, so that it behaves the same whatever
 * the frame rate: the same input gives the same game. Frames come as fast as
 * `display.frame_limit` allows, and each renders the state between the last
 * two ticks, `alpha` of the way from one to the next, so that motion is
 * smooth at any frame rate. This is Unity's `FixedUpdate` and `Update`, and
 * Godot's `_physics_process` and `_process`.
 *
 * **Long frames.** A frame counts for at most `engine.max_frame_time`
 * seconds: after a stall, a breakpoint, or a dragged window, the game
 * carries on from where it was rather than racing through the ticks it
 * missed.
 *
 * **Pausing.** A paused game ticks no more and its simulated time stands
 * still, while frames go on: an options menu draws over a frozen world. The
 * game pauses itself with tb_game_loop_set_paused(), and the loop pauses it
 * while the window does not have the focus, when
 * `gameplay.pause_when_unfocused` says to. `developer.time_scale` slows or
 * speeds simulated time.
 *
 * **Settings at any time.** Before each frame the loop looks for changed
 * settings and applies them: the frame limits and tick rate at once, and
 * the window's mode, display, and size through tb_window_configure().
 *
 * **Quitting.** The window's close button, Alt+F4 or Command+Q, and Escape
 * ask to quit. With `developer.quit_without_asking`, on in developer builds,
 * the loop quits at once. Otherwise it asks the player whether to quit,
 * warning that unsaved progress may be lost: through the game's
 * `confirm_quit` callback when it has one, else in the system's dialog. Where
 * the system has none, as on Linux, the player confirms by asking again
 * within three seconds. A game that handles Escape itself, to open its pause
 * menu, takes the key in its `event` callback.
 *
 * **Without a window.** A loop may have no window: a dedicated server, a
 * replay, or a test. It runs the same, with no events but those given to
 * tb_game_loop_inject().
 *
 * @par Threading
 * Stage-owned: every function is called on the thread that runs the loop,
 * the window system's thread, including from the loop's callbacks.
 */
#ifndef TB_GAME_LOOP_H
#define TB_GAME_LOOP_H

#include "badger/api.h"
#include "badger/engine_settings.h"
#include "badger/window.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief A game loop. Created by tb_game_loop_create(); owned by the caller;
 * released by tb_game_loop_destroy(). */
typedef struct tb_game_loop tb_game_loop;

/** @brief Where a tick or a frame stands in time. */
typedef struct tb_game_time
{
    uint64_t frame;       /**< Frames begun before this one. */
    uint64_t tick;        /**< Ticks run before this one. */
    double tick_seconds;  /**< The length of a tick: 1 over `engine.tick_rate`. */
    double frame_seconds; /**< Real time since the last frame, at most `engine.max_frame_time`. */
    /** For a frame: how far simulated time is from the last tick toward the
     * next, from 0 to 1, for rendering between them. */
    double alpha;
    double seconds; /**< Simulated time: ticks run, times their length. */
    bool paused;    /**< The game is paused: no ticks run. */
    bool focused;   /**< The window has the focus; always, without a window. */
    bool visible;   /**< Some of the window is seen: it is not minimized. */
} tb_game_time;

/** @brief Why the loop was asked to quit. */
typedef enum tb_game_quit_reason
{
    TB_GAME_QUIT_CLOSE_REQUESTED, /**< The window's close button, Alt+F4, Command+Q. */
    TB_GAME_QUIT_ESCAPE           /**< The Escape key. */
} tb_game_quit_reason;

/** @brief What the loop calls. Each is optional. */
typedef struct tb_game_loop_callbacks
{
    /** One fixed step of the simulation. */
    void (*tick)(void *context, const tb_game_time *time);
    /** One frame: render, and run what is not simulation, such as menus. */
    void (*frame)(void *context, const tb_game_time *time);
    /** An event from the window, before the loop acts on it; return true to
     * take it, so that the loop does not: Escape to open a pause menu, for
     * one. */
    bool (*event)(void *context, const tb_window_event *event);
    /** Ask the player whether to quit; return true to quit. NULL for the
     * system's dialog. */
    bool (*confirm_quit)(void *context, tb_game_quit_reason reason);
    void *context; /**< Passed to each. */
} tb_game_loop_callbacks;

/** @brief A clock, for a loop that runs on a time of its own: a test or a
 * replay that runs faster than real time. */
typedef struct tb_game_clock
{
    /** The time now, in nanoseconds, never decreasing. */
    uint64_t (*now)(void *context);
    /** Wait until `now` reaches a time. */
    void (*wait_until)(void *context, uint64_t nanoseconds);
    void *context; /**< Passed to each. */
} tb_game_clock;

/** @brief How a game loop is set up. */
typedef struct tb_game_loop_options
{
    /** The window system; NULL for a loop without a window. */
    tb_window_system *system;
    /** The window, of `system`; NULL for a loop without a window. */
    tb_window *window;
    /** The engine's settings, which the loop reads and applies; required. */
    const tb_engine_settings *settings;
    tb_game_loop_callbacks callbacks; /**< What the loop calls. */
    /** The quit dialog's question and warning; NULL for "Quit?" and "Unsaved
     * progress may be lost." */
    const char *quit_title;
    const char *quit_message; /**< See `quit_title`. */
    /** The clock; NULL for the platform's, tb_platform_clock_nanoseconds()
     * and tb_platform_wait_until(). */
    const tb_game_clock *clock;
} tb_game_loop_options;

/**
 * @brief Create a game loop, which has run no frame yet.
 * @param options How; required. The window system, window, and settings must
 * outlive the loop.
 * @return The loop; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_game_loop *tb_game_loop_create(const tb_game_loop_options *options);

/** @brief Destroy a game loop. @param loop The loop; NULL is ignored. */
TB_API void tb_game_loop_destroy(tb_game_loop *loop);

/**
 * @brief Run frames until the game quits, or `developer.frame_count` frames
 * have run.
 * @param loop Required non-null.
 */
TB_API void tb_game_loop_run(tb_game_loop *loop);

/**
 * @brief Run one frame: read events, apply changed settings, run the ticks
 * that are due, run the frame, then wait out the frame limit.
 * @param loop Required non-null.
 * @return Whether the game goes on: false once it quits.
 */
TB_API bool tb_game_loop_step(tb_game_loop *loop);

/**
 * @brief Quit after the current frame, without asking: what a menu's Quit
 * button does once the player confirmed.
 * @param loop Required non-null.
 */
TB_API void tb_game_loop_quit(tb_game_loop *loop);

/** @brief Whether the game quits: tb_game_loop_step() has returned false, or
 * will. @param loop Required non-null. */
TB_API bool tb_game_loop_quitting(const tb_game_loop *loop);

/**
 * @brief Pause or resume the game, as its menu does.
 *
 * A game the loop paused because the window lost the focus stays paused
 * until the focus returns, whatever this says.
 *
 * @param loop Required non-null.
 * @param paused Whether the game is paused.
 */
TB_API void tb_game_loop_set_paused(tb_game_loop *loop, bool paused);

/** @brief Whether the game is paused, by itself or by losing the focus.
 * @param loop Required non-null. */
TB_API bool tb_game_loop_paused(const tb_game_loop *loop);

/**
 * @brief Handle an event as if the window system had sent it: for replays,
 * and for tests.
 * @param loop Required non-null.
 * @param event The event; required. Its window may be NULL.
 */
TB_API void tb_game_loop_inject(tb_game_loop *loop, const tb_window_event *event);

#endif
