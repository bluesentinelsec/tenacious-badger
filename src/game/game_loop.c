/* The game loop: events, settings, fixed ticks, a frame, and pacing, in
 * that order, once a frame.
 *
 * Simulated time owed to the simulation is the accumulator: each frame adds
 * the real time that passed, scaled and clamped, and each tick takes one
 * tick's length from it. What is left over is how far the frame stands
 * between two ticks. It counts nanoseconds times the tick rate, in which a
 * tick is exactly a billion: a tick of a sixtieth of a second is no whole
 * number of nanoseconds, and rounding it would gain or lose a tick now and
 * then. */
#include "badger/game_loop.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/platform.h"

#define TB_NANOSECONDS_PER_SECOND 1000000000.0

/* A tick, in the accumulator's units of nanoseconds times the tick rate. */
#define TB_GAME_LOOP_TICK UINT64_C(1000000000)

/* Where the system has no dialog, asking to quit a second time within this
 * long confirms it. */
#define TB_GAME_LOOP_CONFIRM_NANOSECONDS UINT64_C(3000000000)

#define TB_GAME_LOOP_QUIT_TITLE "Quit?"
#define TB_GAME_LOOP_QUIT_MESSAGE "Unsaved progress may be lost."

struct tb_game_loop
{
    tb_window_system *system;
    tb_window *window;
    const tb_engine_settings *settings;
    tb_game_loop_callbacks callbacks;
    tb_game_clock clock;
    const char *quit_title;
    const char *quit_message;
    tb_window_options window_options; /* As the settings last described the window. */
    tb_window_backend backend;        /* The window system's; automatic without one. */
    bool fullscreen_shortcut;         /* Alt+Enter switches to fullscreen and back. */
    /* The window's size when the shortcut last took it out of windowed
     * mode, which it returns to while the size settings stay at the
     * versions kept. */
    bool windowed_remembered;
    uint32_t windowed_width;
    uint32_t windowed_height;
    uint64_t width_version;
    uint64_t height_version;
    tb_game_time time;
    bool started;
    uint64_t last_frame;  /* When the last frame began. */
    uint64_t accumulator; /* Simulated time not yet ticked, in nanoseconds times the tick rate. */
    uint64_t deadline;    /* When the next frame may begin, when frames are limited. */
    uint64_t quit_asked;  /* When quitting was last asked without a dialog; 0 for never. */
    bool paused_by_game;
    bool quitting;
};

static uint64_t tb_game_platform_now(void *context)
{
    (void)context;
    return tb_platform_clock_nanoseconds();
}

static void tb_game_platform_wait_until(void *context, uint64_t nanoseconds)
{
    (void)context;
    tb_platform_wait_until(nanoseconds);
}

static uint64_t tb_game_now(const tb_game_loop *loop)
{
    return loop->clock.now(loop->clock.context);
}

tb_game_loop *tb_game_loop_create(const tb_game_loop_options *options)
{
    tb_game_loop *loop = NULL;

    TB_ASSERT(options != NULL);
    TB_ASSERT_MSG(options->settings != NULL, "a game loop has the engine's settings");
    TB_ASSERT_MSG((options->system == NULL) == (options->window == NULL),
                  "a game loop has both a window system and a window, or neither");
    loop = tb_calloc(1, sizeof(tb_game_loop));
    loop->system = options->system;
    loop->window = options->window;
    loop->settings = options->settings;
    loop->callbacks = options->callbacks;
    loop->quit_title = options->quit_title != NULL ? options->quit_title : TB_GAME_LOOP_QUIT_TITLE;
    loop->quit_message =
        options->quit_message != NULL ? options->quit_message : TB_GAME_LOOP_QUIT_MESSAGE;
    if (options->clock != NULL)
    {
        TB_ASSERT(options->clock->now != NULL && options->clock->wait_until != NULL);
        loop->clock = *options->clock;
    }
    else
    {
        loop->clock =
            (tb_game_clock){.now = tb_game_platform_now, .wait_until = tb_game_platform_wait_until};
    }
    tb_engine_settings_window_options(loop->settings, NULL, &loop->window_options);
    loop->backend =
        loop->system != NULL ? tb_window_system_backend(loop->system) : TB_WINDOW_BACKEND_AUTOMATIC;
    loop->fullscreen_shortcut = !options->disable_fullscreen_shortcut;
    loop->time.focused = true;
    loop->time.visible = true;
    if (loop->window != NULL)
    {
        tb_window_state state = {0};
        tb_window_state_of(loop->window, &state);
        loop->time.focused = state.focused;
        loop->time.visible = !state.minimized;
    }
    return loop;
}

void tb_game_loop_destroy(tb_game_loop *loop)
{
    tb_free(loop);
}

/* ---------------------------------------------------------------------------
 * Quitting.
 * ------------------------------------------------------------------------- */

void tb_game_loop_quit(tb_game_loop *loop)
{
    TB_ASSERT(loop != NULL);
    loop->quitting = true;
}

bool tb_game_loop_quitting(const tb_game_loop *loop)
{
    TB_ASSERT(loop != NULL);
    return loop->quitting;
}

/* Without a dialog, the player confirms by asking again soon. */
static bool tb_game_loop_asked_twice(tb_game_loop *loop)
{
    const uint64_t now = tb_game_now(loop);

    if (loop->quit_asked != 0 && now - loop->quit_asked <= TB_GAME_LOOP_CONFIRM_NANOSECONDS)
    {
        return true;
    }
    loop->quit_asked = now != 0 ? now : 1;
    (void)tb_fmt_print_error("%s %s Ask again within 3 seconds to quit.\n", loop->quit_title,
                             loop->quit_message);
    return false;
}

static void tb_game_loop_request_quit(tb_game_loop *loop, tb_game_quit_reason reason)
{
    bool confirmed = false;

    if (tb_setting_boolean(loop->settings->developer.quit_without_asking))
    {
        loop->quitting = true;
        return;
    }
    if (loop->callbacks.confirm_quit != NULL)
    {
        loop->quitting = loop->callbacks.confirm_quit(loop->callbacks.context, reason);
    }
    else if (loop->window != NULL &&
             tb_window_confirm(loop->window, loop->quit_title, loop->quit_message, &confirmed) ==
                 TB_WINDOW_OK)
    {
        loop->quitting = confirmed;
    }
    else
    {
        loop->quitting = tb_game_loop_asked_twice(loop);
    }
    /* The question took time the game should not count. */
    loop->last_frame = tb_game_now(loop);
}

/* ---------------------------------------------------------------------------
 * Fullscreen and back.
 * ------------------------------------------------------------------------- */

bool tb_game_loop_is_fullscreen_shortcut(tb_window_backend backend, const tb_window_event *event)
{
    uint32_t held = 0;

    TB_ASSERT(event != NULL);
    held = event->modifiers &
           (TB_MODIFIER_SHIFT | TB_MODIFIER_CONTROL | TB_MODIFIER_ALT | TB_MODIFIER_SUPER);
    if (event->type != TB_WINDOW_EVENT_KEY_DOWN || event->repeat)
    {
        return false;
    }
    if (backend == TB_WINDOW_BACKEND_COCOA)
    {
        return event->key == TB_KEY_F && held == (TB_MODIFIER_CONTROL | TB_MODIFIER_SUPER);
    }
    return event->key == TB_KEY_ENTER && held == TB_MODIFIER_ALT;
}

/* Switch `display.window_mode` between windowed and borderless, at the
 * runtime layer, which the settings apply before the next frame. Leaving a
 * window, remember its size, which the event's state has. */
static void tb_game_loop_toggle_fullscreen(tb_game_loop *loop, const tb_window_event *event)
{
    const tb_engine_settings *const settings = loop->settings;
    tb_setting *const mode = settings->display.window_mode;

    if (tb_setting_choice(mode) != (size_t)TB_WINDOW_MODE_WINDOWED)
    {
        (void)tb_setting_set_choice(mode, (size_t)TB_WINDOW_MODE_WINDOWED);
        return;
    }
    if (event->state.mode == TB_WINDOW_MODE_WINDOWED && event->state.width > 0 &&
        event->state.height > 0)
    {
        loop->windowed_remembered = true;
        loop->windowed_width = event->state.width;
        loop->windowed_height = event->state.height;
        loop->width_version = tb_setting_version(settings->display.window_width);
        loop->height_version = tb_setting_version(settings->display.window_height);
    }
    (void)tb_setting_set_choice(mode, (size_t)TB_WINDOW_MODE_BORDERLESS);
}

/* A windowed window takes the size it had when the shortcut took it out of
 * a window, unless the size settings changed since, which forgets it. */
static void tb_game_loop_recall_windowed_size(tb_game_loop *loop, tb_window_options *options)
{
    const tb_engine_settings *const settings = loop->settings;

    if (!loop->windowed_remembered)
    {
        return;
    }
    if (tb_setting_version(settings->display.window_width) != loop->width_version ||
        tb_setting_version(settings->display.window_height) != loop->height_version)
    {
        loop->windowed_remembered = false;
        return;
    }
    if (options->mode == TB_WINDOW_MODE_WINDOWED)
    {
        options->width = loop->windowed_width;
        options->height = loop->windowed_height;
    }
}

/* ---------------------------------------------------------------------------
 * Events.
 * ------------------------------------------------------------------------- */

void tb_game_loop_inject(tb_game_loop *loop, const tb_window_event *event)
{
    TB_ASSERT(loop != NULL);
    TB_ASSERT(event != NULL);
    switch (event->type)
    {
    case TB_WINDOW_EVENT_FOCUS_GAINED:
        loop->time.focused = true;
        break;
    case TB_WINDOW_EVENT_FOCUS_LOST:
        loop->time.focused = false;
        break;
    case TB_WINDOW_EVENT_MINIMIZED:
        loop->time.visible = false;
        break;
    case TB_WINDOW_EVENT_RESTORED:
        loop->time.visible = true;
        break;
    case TB_WINDOW_EVENT_CLOSE_REQUESTED:
    case TB_WINDOW_EVENT_RESIZED:
    case TB_WINDOW_EVENT_SCALE_CHANGED:
    case TB_WINDOW_EVENT_KEY_DOWN:
    case TB_WINDOW_EVENT_KEY_UP:
    case TB_WINDOW_EVENT_DISPLAYS_CHANGED:
    case TB_WINDOW_EVENT_WAKE:
    case TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN:
    case TB_WINDOW_EVENT_MOUSE_BUTTON_UP:
    case TB_WINDOW_EVENT_MOUSE_MOVED:
    case TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION:
    case TB_WINDOW_EVENT_MOUSE_WHEEL:
    case TB_WINDOW_EVENT_MOUSE_ENTERED:
    case TB_WINDOW_EVENT_MOUSE_LEFT:
    case TB_WINDOW_EVENT_TEXT_INPUT:
        break;
    }
    if (loop->callbacks.event != NULL && loop->callbacks.event(loop->callbacks.context, event))
    {
        return;
    }
    if (event->type == TB_WINDOW_EVENT_CLOSE_REQUESTED)
    {
        tb_game_loop_request_quit(loop, TB_GAME_QUIT_CLOSE_REQUESTED);
    }
    else if (loop->fullscreen_shortcut && tb_game_loop_is_fullscreen_shortcut(loop->backend, event))
    {
        tb_game_loop_toggle_fullscreen(loop, event);
    }
    else if (event->type == TB_WINDOW_EVENT_KEY_DOWN && event->key == TB_KEY_ESCAPE &&
             !event->repeat)
    {
        tb_game_loop_request_quit(loop, TB_GAME_QUIT_ESCAPE);
    }
}

static void tb_game_loop_read_events(tb_game_loop *loop)
{
    tb_window_event event = {0};

    if (loop->system == NULL)
    {
        return;
    }
    while (!loop->quitting && tb_window_system_poll(loop->system, &event))
    {
        tb_game_loop_inject(loop, &event);
    }
}

/* ---------------------------------------------------------------------------
 * Settings.
 * ------------------------------------------------------------------------- */

static bool tb_game_window_options_equal(const tb_window_options *left,
                                         const tb_window_options *right)
{
    return left->mode == right->mode && left->display == right->display &&
           left->width == right->width && left->height == right->height &&
           left->fullscreen_width == right->fullscreen_width &&
           left->fullscreen_height == right->fullscreen_height &&
           left->refresh_rate == right->refresh_rate && left->fixed_size == right->fixed_size &&
           left->min_width == right->min_width && left->min_height == right->min_height &&
           left->aspect_width == right->aspect_width && left->aspect_height == right->aspect_height;
}

/* Configure the window when the display settings changed since the last
 * frame. A configuration the system refuses leaves the window as it was. */
static void tb_game_loop_apply_settings(tb_game_loop *loop)
{
    tb_window_options options = {0};

    tb_engine_settings_window_options(loop->settings, NULL, &options);
    tb_game_loop_recall_windowed_size(loop, &options);
    if (tb_game_window_options_equal(&options, &loop->window_options))
    {
        return;
    }
    loop->window_options = options;
    if (loop->window != NULL)
    {
        (void)tb_window_configure(loop->window, &options, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Frames.
 * ------------------------------------------------------------------------- */

bool tb_game_loop_paused(const tb_game_loop *loop)
{
    TB_ASSERT(loop != NULL);
    return loop->paused_by_game ||
           (!loop->time.focused &&
            tb_setting_boolean(loop->settings->gameplay.pause_when_unfocused));
}

void tb_game_loop_set_paused(tb_game_loop *loop, bool paused)
{
    TB_ASSERT(loop != NULL);
    loop->paused_by_game = paused;
}

static uint64_t tb_game_loop_seconds(double seconds)
{
    return (uint64_t)(seconds * TB_NANOSECONDS_PER_SECOND + 0.5);
}

/* Run the ticks the time that passed is owed, and say how far the frame
 * stands toward the next. */
static void tb_game_loop_tick(tb_game_loop *loop, uint64_t elapsed)
{
    const tb_engine_settings *const settings = loop->settings;
    const uint64_t rate = (uint64_t)tb_setting_integer(settings->engine.tick_rate);

    loop->time.tick_seconds = 1.0 / (double)rate;
    loop->time.paused = tb_game_loop_paused(loop);
    if (!loop->time.paused)
    {
        const double scale = tb_setting_number(settings->developer.time_scale);
        loop->accumulator += (uint64_t)((double)elapsed * scale + 0.5) * rate;
    }
    while (loop->accumulator >= TB_GAME_LOOP_TICK && !loop->time.paused)
    {
        if (loop->callbacks.tick != NULL)
        {
            loop->callbacks.tick(loop->callbacks.context, &loop->time);
        }
        loop->time.tick++;
        loop->time.seconds += loop->time.tick_seconds;
        loop->accumulator -= TB_GAME_LOOP_TICK;
    }
    /* A paused game keeps what it owed, so a frame stands where it stood. */
    loop->time.alpha = (double)(loop->accumulator % TB_GAME_LOOP_TICK) / (double)TB_GAME_LOOP_TICK;
}

/* Wait until the next frame may begin, as the frame limit says: the
 * background limit while the window has no focus or is not seen. */
static void tb_game_loop_pace(tb_game_loop *loop)
{
    const tb_engine_settings *const settings = loop->settings;
    const bool foreground = loop->time.focused && loop->time.visible;
    const int64_t limit = tb_setting_integer(foreground ? settings->display.frame_limit
                                                        : settings->display.background_frame_limit);
    const uint64_t now = tb_game_now(loop);
    uint64_t period = 0;

    if (limit <= 0)
    {
        loop->deadline = now;
        return;
    }
    period = tb_game_loop_seconds(1.0 / (double)limit);
    loop->deadline += period;
    /* A frame that ran late by more than a period starts the count again,
     * rather than running frames back to back to catch up. */
    if (loop->deadline + period < now)
    {
        loop->deadline = now;
        return;
    }
    if (loop->deadline > now)
    {
        loop->clock.wait_until(loop->clock.context, loop->deadline);
    }
}

bool tb_game_loop_step(tb_game_loop *loop)
{
    uint64_t now = 0;
    uint64_t elapsed = 0;
    int64_t frame_count = 0;

    TB_ASSERT(loop != NULL);
    if (loop->quitting)
    {
        return false;
    }
    if (!loop->started)
    {
        loop->started = true;
        loop->last_frame = tb_game_now(loop);
        loop->deadline = loop->last_frame;
    }
    tb_game_loop_read_events(loop);
    if (loop->quitting)
    {
        return false;
    }
    tb_game_loop_apply_settings(loop);

    now = tb_game_now(loop);
    elapsed = now - loop->last_frame;
    {
        const uint64_t longest =
            tb_game_loop_seconds(tb_setting_number(loop->settings->engine.max_frame_time));
        elapsed = elapsed < longest ? elapsed : longest;
    }
    loop->last_frame = now;
    loop->time.frame_seconds = (double)elapsed / TB_NANOSECONDS_PER_SECOND;
    tb_game_loop_tick(loop, elapsed);
    if (loop->callbacks.frame != NULL)
    {
        loop->callbacks.frame(loop->callbacks.context, &loop->time);
    }
    loop->time.frame++;

    frame_count = tb_setting_integer(loop->settings->developer.frame_count);
    if (frame_count > 0 && loop->time.frame >= (uint64_t)frame_count)
    {
        loop->quitting = true;
    }
    if (loop->quitting)
    {
        return false;
    }
    tb_game_loop_pace(loop);
    return true;
}

void tb_game_loop_run(tb_game_loop *loop)
{
    TB_ASSERT(loop != NULL);
    while (tb_game_loop_step(loop))
    {
    }
}
