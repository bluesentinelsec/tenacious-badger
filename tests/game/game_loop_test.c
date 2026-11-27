/* The game loop must tick at the tick rate whatever the frame rate, put each
 * frame between two ticks, count a long frame as only so long, stop ticking
 * while paused, pace frames by the frame limits, stop after a frame count,
 * apply changed display settings to its window, switch it to fullscreen and
 * back on the platform's shortcut, and quit as players expect: at once in a
 * developer build, after asking in a release build.
 *
 * Most cases run without a window on a clock of their own, which a frame
 * advances by as long as the frame takes, so that every count is exact. */
#include "badger/assert.h"
#include "badger/game_loop.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/settings.h"
#include "badger/str.h"

#include "tb_test.h"

#define MILLISECOND UINT64_C(1000000)
#define SECOND (UINT64_C(1000) * MILLISECOND)

/* How long a case with a window waits for the system to answer. */
#define SETTLE_NANOSECONDS (UINT64_C(500) * MILLISECOND)

/* A clock that only moves when told: by a frame, or by a wait. */
typedef struct fake_clock
{
    uint64_t now;
    uint64_t waits;       /* How many times the loop waited. */
    uint64_t last_target; /* The last time it waited for. */
} fake_clock;

static uint64_t fake_now(void *context)
{
    const fake_clock *const clock = context;
    return clock->now;
}

static void fake_wait_until(void *context, uint64_t nanoseconds)
{
    fake_clock *const clock = context;
    clock->waits++;
    clock->last_target = nanoseconds;
    if (nanoseconds > clock->now)
    {
        clock->now = nanoseconds;
    }
}

/* What the loop told the game, and how long its frames take. */
typedef struct game
{
    fake_clock clock;
    uint64_t frame_cost; /* Each frame advances the clock by this much. */
    uint64_t ticks;
    uint64_t frames;
    double alphas[16];
    double last_seconds;
    bool last_paused;
    bool take_escape;   /* The event callback takes Escape. */
    int confirm_answer; /* -1 for no confirm callback; else what it answers. */
    uint64_t confirms;
} game;

static void on_tick(void *context, const tb_game_time *time)
{
    game *const state = context;
    TB_TEST_EXPECT_UINT(time->tick, ==, state->ticks);
    state->ticks++;
}

static void on_frame(void *context, const tb_game_time *time)
{
    game *const state = context;
    if (state->frames < 16)
    {
        state->alphas[state->frames] = time->alpha;
    }
    state->frames++;
    state->last_seconds = time->seconds;
    state->last_paused = time->paused;
    state->clock.now += state->frame_cost;
}

static bool on_event(void *context, const tb_window_event *event)
{
    const game *const state = context;
    return state->take_escape && event->type == TB_WINDOW_EVENT_KEY_DOWN &&
           event->key == TB_KEY_ESCAPE;
}

static bool on_confirm(void *context, tb_game_quit_reason reason)
{
    game *const state = context;
    (void)reason;
    state->confirms++;
    return state->confirm_answer > 0;
}

typedef struct fixture
{
    tb_settings *settings;
    tb_engine_settings *engine; /* Of the heap: the loop keeps a pointer to it. */
    tb_game_clock clock;
    tb_game_loop *loop;
} fixture;

/* A headless loop for a game, with the settings as a developer build or a
 * release build has them, and the values that `values` sets. */
static fixture make_loop(game *state, bool developer, const char *values)
{
    fixture made = {0};
    tb_game_loop_options options = {0};

    made.settings = tb_settings_create();
    made.engine = tb_calloc(1, sizeof(tb_engine_settings));
    tb_engine_settings_add(made.settings, &(tb_engine_settings_options){.developer = developer},
                           made.engine);
    if (values != NULL)
    {
        TB_TEST_REQUIRE_UINT(tb_settings_load_text(made.settings, TB_SETTING_LAYER_COMMAND_LINE,
                                                   values, tb_str_length(values), NULL, NULL),
                             ==, 0);
    }
    made.clock =
        (tb_game_clock){.now = fake_now, .wait_until = fake_wait_until, .context = &state->clock};
    options = (tb_game_loop_options){
        .settings = made.engine,
        .clock = &made.clock,
        .callbacks = {.tick = on_tick,
                      .frame = on_frame,
                      .event = on_event,
                      .confirm_quit = state->confirm_answer >= 0 ? on_confirm : NULL,
                      .context = state}};
    made.loop = tb_game_loop_create(&options);
    return made;
}

static void release_loop(fixture *made)
{
    tb_game_loop_destroy(made->loop);
    tb_settings_destroy(made->settings);
    tb_free(made->engine);
}

static void step(fixture *made, size_t frames)
{
    for (size_t index = 0; index < frames; index++)
    {
        (void)tb_game_loop_step(made->loop);
    }
}

static void press(fixture *made, tb_key key, bool repeat)
{
    const tb_window_event event = {.type = TB_WINDOW_EVENT_KEY_DOWN, .key = key, .repeat = repeat};
    tb_game_loop_inject(made->loop, &event);
}

static void send(fixture *made, tb_window_event_type type)
{
    const tb_window_event event = {.type = type};
    tb_game_loop_inject(made->loop, &event);
}

/* ---------------------------------------------------------------------------
 * Ticks and frames.
 * ------------------------------------------------------------------------- */

/* A second of frames runs a second of ticks, at 30, 144, or 1000 frames a
 * second. */
TB_TEST(game_loop, ticks_follow_the_tick_rate_whatever_the_frame_rate)
{
    static const uint64_t frame_rates[] = {30, 144, 1000};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(frame_rates); index++)
    {
        game state = {.confirm_answer = -1};
        fixture made = make_loop(&state, true, "engine.tick_rate = 60\n");
        TB_TEST_CONTEXT("%llu frames a second", (unsigned long long)frame_rates[index]);
        state.frame_cost = SECOND / frame_rates[index];
        /* The first frame begins the clock; the others are a second. */
        step(&made, (size_t)frame_rates[index] + 1);
        TB_TEST_EXPECT_UINT(state.ticks, >=, 59);
        TB_TEST_EXPECT_UINT(state.ticks, <=, 60);
        TB_TEST_EXPECT_FLOAT_NEAR(state.last_seconds, (double)state.ticks / 60.0, 1e-9);
        release_loop(&made);
    }
}

/* Ticks of a tenth of a second and frames of a fortieth: each frame stands a
 * quarter further between two ticks. */
TB_TEST(game_loop, a_frame_stands_between_two_ticks)
{
    game state = {.confirm_answer = -1, .frame_cost = 25 * MILLISECOND};
    fixture made = make_loop(&state, true, "engine.tick_rate = 10\n");

    step(&made, 6);
    TB_TEST_EXPECT_FLOAT_NEAR(state.alphas[0], 0.0, 1e-9);
    TB_TEST_EXPECT_FLOAT_NEAR(state.alphas[1], 0.25, 1e-9);
    TB_TEST_EXPECT_FLOAT_NEAR(state.alphas[2], 0.5, 1e-9);
    TB_TEST_EXPECT_FLOAT_NEAR(state.alphas[3], 0.75, 1e-9);
    TB_TEST_EXPECT_FLOAT_NEAR(state.alphas[4], 0.0, 1e-9);
    TB_TEST_EXPECT_UINT(state.ticks, ==, 1);
    release_loop(&made);
}

/* A frame of five seconds, a breakpoint or a stall, counts for the longest
 * a frame may: a quarter of a second, fifteen ticks at 60. */
TB_TEST(game_loop, a_long_frame_counts_for_the_longest_a_frame_may)
{
    game state = {.confirm_answer = -1};
    fixture made = make_loop(&state, true, "engine.tick_rate = 60\nengine.max_frame_time = 0.25\n");

    step(&made, 1);
    state.clock.now += 5 * SECOND;
    step(&made, 1);
    TB_TEST_EXPECT_UINT(state.ticks, ==, 15);
    release_loop(&made);
}

TB_TEST(game_loop, time_scale_slows_or_speeds_simulated_time)
{
    game state = {.confirm_answer = -1, .frame_cost = 10 * MILLISECOND};
    fixture made = make_loop(&state, true, "engine.tick_rate = 100\ndeveloper.time_scale = 2\n");

    step(&made, 101);
    TB_TEST_EXPECT_UINT(state.ticks, ==, 200);
    release_loop(&made);
}

/* ---------------------------------------------------------------------------
 * Pausing.
 * ------------------------------------------------------------------------- */

TB_TEST(game_loop, a_paused_game_ticks_no_more_and_its_frames_go_on)
{
    game state = {.confirm_answer = -1, .frame_cost = 25 * MILLISECOND};
    fixture made = make_loop(&state, true, "engine.tick_rate = 10\n");
    uint64_t ticks = 0;
    uint64_t frames = 0;
    double alpha = 0.0;

    step(&made, 3);
    ticks = state.ticks;
    frames = state.frames;
    alpha = state.alphas[2];
    tb_game_loop_set_paused(made.loop, true);
    TB_TEST_EXPECT(tb_game_loop_paused(made.loop));
    step(&made, 40);
    TB_TEST_EXPECT_UINT(state.ticks, ==, ticks);
    TB_TEST_EXPECT_UINT(state.frames, ==, frames + 40);
    TB_TEST_EXPECT(state.last_paused);
    tb_game_loop_set_paused(made.loop, false);
    step(&made, 1);
    TB_TEST_EXPECT(!state.last_paused);
    /* Resuming goes on from where the frame stood. */
    TB_TEST_EXPECT_FLOAT_NEAR(alpha, 0.5, 1e-9);
    release_loop(&made);
}

TB_TEST(game_loop, losing_the_focus_pauses_when_the_settings_say_so)
{
    game state = {.confirm_answer = -1, .frame_cost = 10 * MILLISECOND};
    fixture made = make_loop(&state, true, NULL);

    send(&made, TB_WINDOW_EVENT_FOCUS_LOST);
    TB_TEST_EXPECT(tb_game_loop_paused(made.loop));
    send(&made, TB_WINDOW_EVENT_FOCUS_GAINED);
    TB_TEST_EXPECT(!tb_game_loop_paused(made.loop));
    (void)tb_setting_set_boolean(made.engine->gameplay.pause_when_unfocused, false);
    send(&made, TB_WINDOW_EVENT_FOCUS_LOST);
    TB_TEST_EXPECT(!tb_game_loop_paused(made.loop));
    release_loop(&made);
}

/* ---------------------------------------------------------------------------
 * Pacing.
 * ------------------------------------------------------------------------- */

/* At a limit of 100, frames that take a millisecond begin ten apart; without
 * the focus, at the background limit of 25, forty apart. */
TB_TEST(game_loop, frames_are_paced_by_the_frame_limits)
{
    game state = {.confirm_answer = -1, .frame_cost = 1 * MILLISECOND};
    fixture made = make_loop(&state, true,
                             "display.frame_limit = 100\ndisplay.background_frame_limit = 25\n"
                             "gameplay.pause_when_unfocused = false\n");
    uint64_t start = 0;

    step(&made, 1);
    start = state.clock.now;
    step(&made, 10);
    TB_TEST_EXPECT_UINT(state.clock.now - start, ==, 100 * MILLISECOND);
    send(&made, TB_WINDOW_EVENT_FOCUS_LOST);
    start = state.clock.now;
    step(&made, 10);
    TB_TEST_EXPECT_UINT(state.clock.now - start, ==, 400 * MILLISECOND);
    /* A minimized window is paced as one without the focus. */
    send(&made, TB_WINDOW_EVENT_FOCUS_GAINED);
    send(&made, TB_WINDOW_EVENT_MINIMIZED);
    start = state.clock.now;
    step(&made, 10);
    TB_TEST_EXPECT_UINT(state.clock.now - start, ==, 400 * MILLISECOND);
    send(&made, TB_WINDOW_EVENT_RESTORED);
    release_loop(&made);
}

TB_TEST(game_loop, without_a_limit_frames_never_wait)
{
    game state = {.confirm_answer = -1, .frame_cost = 1 * MILLISECOND};
    fixture made = make_loop(&state, true, "display.frame_limit = 0\n");

    step(&made, 50);
    TB_TEST_EXPECT_UINT(state.clock.waits, ==, 0);
    release_loop(&made);
}

/* A frame far later than its limit starts the count again, rather than
 * running the frames it missed back to back. */
TB_TEST(game_loop, a_late_frame_does_not_make_later_frames_rush)
{
    game state = {.confirm_answer = -1, .frame_cost = 1 * MILLISECOND};
    fixture made = make_loop(&state, true, "display.frame_limit = 100\n");
    uint64_t start = 0;

    step(&made, 2);
    state.clock.now += 500 * MILLISECOND;
    step(&made, 1);
    start = state.clock.now;
    step(&made, 5);
    TB_TEST_EXPECT_UINT(state.clock.now - start, >=, 40 * MILLISECOND);
    release_loop(&made);
}

TB_TEST(game_loop, a_frame_count_stops_the_loop)
{
    game state = {.confirm_answer = -1};
    fixture made = make_loop(&state, true, "developer.frame_count = 7\n");

    tb_game_loop_run(made.loop);
    TB_TEST_EXPECT_UINT(state.frames, ==, 7);
    TB_TEST_EXPECT(tb_game_loop_quitting(made.loop));
    TB_TEST_EXPECT(!tb_game_loop_step(made.loop));
    TB_TEST_EXPECT_UINT(state.frames, ==, 7);
    release_loop(&made);
}

/* ---------------------------------------------------------------------------
 * Quitting.
 * ------------------------------------------------------------------------- */

TB_TEST(game_loop, a_developer_build_quits_on_escape_at_once)
{
    game state = {.confirm_answer = -1};
    fixture made = make_loop(&state, true, NULL);

    step(&made, 1);
    press(&made, TB_KEY_ESCAPE, false);
    TB_TEST_EXPECT(tb_game_loop_quitting(made.loop));
    TB_TEST_EXPECT(!tb_game_loop_step(made.loop));
    release_loop(&made);
}

TB_TEST(game_loop, a_release_build_asks_before_quitting)
{
    game state = {.confirm_answer = 0};
    fixture made = make_loop(&state, false, NULL);

    /* No: the game goes on. */
    press(&made, TB_KEY_ESCAPE, false);
    TB_TEST_EXPECT_UINT(state.confirms, ==, 1);
    TB_TEST_EXPECT(!tb_game_loop_quitting(made.loop));
    /* A key held down asks once. */
    press(&made, TB_KEY_ESCAPE, true);
    TB_TEST_EXPECT_UINT(state.confirms, ==, 1);
    /* Other keys do not ask. */
    press(&made, TB_KEY_W, false);
    TB_TEST_EXPECT_UINT(state.confirms, ==, 1);
    /* Yes: the game quits, from the close button too. */
    state.confirm_answer = 1;
    send(&made, TB_WINDOW_EVENT_CLOSE_REQUESTED);
    TB_TEST_EXPECT_UINT(state.confirms, ==, 2);
    TB_TEST_EXPECT(tb_game_loop_quitting(made.loop));
    release_loop(&made);
}

/* Without a dialog, the player confirms by asking again within three
 * seconds. */
TB_TEST(game_loop, without_a_dialog_asking_twice_quits)
{
    game state = {.confirm_answer = -1};
    fixture made = make_loop(&state, false, NULL);

    press(&made, TB_KEY_ESCAPE, false);
    TB_TEST_EXPECT(!tb_game_loop_quitting(made.loop));
    state.clock.now += 4 * SECOND;
    press(&made, TB_KEY_ESCAPE, false);
    TB_TEST_EXPECT(!tb_game_loop_quitting(made.loop));
    state.clock.now += 2 * SECOND;
    send(&made, TB_WINDOW_EVENT_CLOSE_REQUESTED);
    TB_TEST_EXPECT(tb_game_loop_quitting(made.loop));
    release_loop(&made);
}

/* A game that opens its pause menu on Escape takes the key, and the loop
 * leaves it alone. */
TB_TEST(game_loop, a_game_may_take_escape_for_itself)
{
    game state = {.confirm_answer = -1, .take_escape = true};
    fixture made = make_loop(&state, true, NULL);

    press(&made, TB_KEY_ESCAPE, false);
    TB_TEST_EXPECT(!tb_game_loop_quitting(made.loop));
    tb_game_loop_quit(made.loop);
    TB_TEST_EXPECT(tb_game_loop_quitting(made.loop));
    release_loop(&made);
}

/* ---------------------------------------------------------------------------
 * Fullscreen and back.
 * ------------------------------------------------------------------------- */

static tb_window_event key_down(tb_key key, uint32_t modifiers)
{
    return (tb_window_event){.type = TB_WINDOW_EVENT_KEY_DOWN, .key = key, .modifiers = modifiers};
}

/* Each platform's own shortcut, pressed, not repeated, with exactly its
 * modifiers: Control+Command+F on a Mac, Alt+Enter elsewhere. */
TB_TEST(game_loop, the_fullscreen_shortcut_is_the_platforms_own)
{
    static const tb_window_backend alt_enter[] = {TB_WINDOW_BACKEND_AUTOMATIC,
                                                  TB_WINDOW_BACKEND_WIN32,
                                                  TB_WINDOW_BACKEND_WAYLAND, TB_WINDOW_BACKEND_X11};
    const tb_window_event control_command_f =
        key_down(TB_KEY_F, TB_MODIFIER_CONTROL | TB_MODIFIER_SUPER);
    tb_window_event repeated = key_down(TB_KEY_ENTER, TB_MODIFIER_ALT);
    tb_window_event released = key_down(TB_KEY_ENTER, TB_MODIFIER_ALT);

    repeated.repeat = true;
    released.type = TB_WINDOW_EVENT_KEY_UP;
    for (size_t index = 0; index < TB_TEST_COUNT_OF(alt_enter); index++)
    {
        const tb_window_backend backend = alt_enter[index];
        const tb_window_event alt_enter_event = key_down(TB_KEY_ENTER, TB_MODIFIER_ALT);
        const tb_window_event shift_alt_enter =
            key_down(TB_KEY_ENTER, TB_MODIFIER_ALT | TB_MODIFIER_SHIFT);
        const tb_window_event enter = key_down(TB_KEY_ENTER, 0);
        TB_TEST_CONTEXT("%s", tb_window_backend_name(backend));
        TB_TEST_EXPECT(tb_game_loop_is_fullscreen_shortcut(backend, &alt_enter_event));
        TB_TEST_EXPECT(!tb_game_loop_is_fullscreen_shortcut(backend, &shift_alt_enter));
        TB_TEST_EXPECT(!tb_game_loop_is_fullscreen_shortcut(backend, &enter));
        TB_TEST_EXPECT(!tb_game_loop_is_fullscreen_shortcut(backend, &repeated));
        TB_TEST_EXPECT(!tb_game_loop_is_fullscreen_shortcut(backend, &released));
        TB_TEST_EXPECT(!tb_game_loop_is_fullscreen_shortcut(backend, &control_command_f));
    }
    {
        const tb_window_event alt_enter_event = key_down(TB_KEY_ENTER, TB_MODIFIER_ALT);
        const tb_window_event command_f = key_down(TB_KEY_F, TB_MODIFIER_SUPER);
        TB_TEST_EXPECT(
            tb_game_loop_is_fullscreen_shortcut(TB_WINDOW_BACKEND_COCOA, &control_command_f));
        TB_TEST_EXPECT(!tb_game_loop_is_fullscreen_shortcut(TB_WINDOW_BACKEND_COCOA, &command_f));
        TB_TEST_EXPECT(
            !tb_game_loop_is_fullscreen_shortcut(TB_WINDOW_BACKEND_COCOA, &alt_enter_event));
    }
}

/* Alt+Enter switches the window mode setting between windowed and
 * borderless, in the runtime layer that the options menu shows, and never
 * in the player's file. Exclusive fullscreen goes back to a window too. */
TB_TEST(game_loop, alt_enter_switches_the_window_mode_setting)
{
    game state = {.confirm_answer = -1};
    fixture made = make_loop(&state, true, NULL);
    const tb_window_event alt_enter = key_down(TB_KEY_ENTER, TB_MODIFIER_ALT);
    tb_setting *const mode = made.engine->display.window_mode;

    TB_TEST_REQUIRE_UINT(tb_setting_choice(mode), ==, TB_WINDOW_MODE_WINDOWED);
    tb_game_loop_inject(made.loop, &alt_enter);
    TB_TEST_EXPECT_UINT(tb_setting_choice(mode), ==, TB_WINDOW_MODE_BORDERLESS);
    TB_TEST_EXPECT_UINT(tb_setting_source(mode), ==, TB_SETTING_LAYER_RUNTIME);
    TB_TEST_EXPECT(!tb_setting_has_layer(mode, TB_SETTING_LAYER_PLAYER_FILE));
    step(&made, 1);
    tb_game_loop_inject(made.loop, &alt_enter);
    TB_TEST_EXPECT_UINT(tb_setting_choice(mode), ==, TB_WINDOW_MODE_WINDOWED);

    TB_TEST_REQUIRE_UINT(tb_setting_set_choice(mode, TB_WINDOW_MODE_FULLSCREEN), ==, TB_SETTING_OK);
    tb_game_loop_inject(made.loop, &alt_enter);
    TB_TEST_EXPECT_UINT(tb_setting_choice(mode), ==, TB_WINDOW_MODE_WINDOWED);
    TB_TEST_EXPECT(!tb_game_loop_quitting(made.loop));
    release_loop(&made);
}

/* A game turns the shortcut off, for a binding of its own. */
TB_TEST(game_loop, a_game_may_turn_the_fullscreen_shortcut_off)
{
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};
    tb_game_loop *loop = NULL;
    const tb_window_event alt_enter = key_down(TB_KEY_ENTER, TB_MODIFIER_ALT);

    tb_engine_settings_add(settings, &(tb_engine_settings_options){.developer = true}, &engine);
    loop = tb_game_loop_create(
        &(tb_game_loop_options){.settings = &engine, .disable_fullscreen_shortcut = true});
    tb_game_loop_inject(loop, &alt_enter);
    TB_TEST_EXPECT_UINT(tb_setting_choice(engine.display.window_mode), ==, TB_WINDOW_MODE_WINDOWED);
    tb_game_loop_destroy(loop);
    tb_settings_destroy(settings);
}

/* ---------------------------------------------------------------------------
 * A window, where there is one.
 * ------------------------------------------------------------------------- */

/* Wait for the native resize as well as the mode. A windowed size is in
 * logical units, and a borderless surface covers the display in pixels. */
static void step_until(tb_game_loop *loop, tb_window_system *system, tb_window *window,
                       tb_window_mode mode, uint32_t width, uint32_t height)
{
    const uint64_t deadline = tb_platform_clock_nanoseconds() + SETTLE_NANOSECONDS;
    tb_window_state state = {0};

    do
    {
        TB_TEST_EXPECT(tb_game_loop_step(loop));
        tb_window_state_of(window, &state);
        const bool fits = mode == TB_WINDOW_MODE_WINDOWED
                              ? (state.width == width && state.height == height)
                              : (state.pixel_width == width && state.pixel_height == height);
        if (state.mode == mode && fits)
        {
            return;
        }
        tb_window_system_wait(system, UINT64_C(10) * MILLISECOND);
    } while (tb_platform_clock_nanoseconds() < deadline);
}

/* The platform's fullscreen shortcut, from a window in the state it is in. */
static tb_window_event fullscreen_shortcut(tb_window_system *system, tb_window *window)
{
    tb_window_event event = key_down(TB_KEY_ENTER, TB_MODIFIER_ALT);

    if (tb_window_system_backend(system) == TB_WINDOW_BACKEND_COCOA)
    {
        event = key_down(TB_KEY_F, TB_MODIFIER_CONTROL | TB_MODIFIER_SUPER);
    }
    event.window = window;
    tb_window_state_of(window, &event.state);
    return event;
}

/* Back from fullscreen, a window has the size the player gave it before,
 * not the size the settings give, until those settings change. */
TB_TEST(game_loop, back_from_fullscreen_a_window_has_its_size_again)
{
    tb_window_system *system = NULL;
    tb_window *window = NULL;
    tb_window_options options = {0};
    tb_window_state state = {0};
    tb_window_event shortcut = {0};
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};
    tb_game_loop *loop = NULL;
    tb_display display = {0};
    static const uint32_t sizes[][2] = {{800, 450}, {768, 432}};

    if (tb_window_system_open(NULL, &system, NULL) != TB_WINDOW_OK)
    {
        tb_settings_destroy(settings);
        TB_TEST_SKIP("no windowing here");
    }
    tb_engine_settings_add(settings, &(tb_engine_settings_options){.developer = true}, &engine);
    (void)tb_settings_set(settings, TB_SETTING_LAYER_RUNTIME, "display.window_width", "640");
    (void)tb_settings_set(settings, TB_SETTING_LAYER_RUNTIME, "display.window_height", "360");
    tb_engine_settings_window_options(&engine, "loop", &options);
    TB_TEST_REQUIRE_UINT(tb_window_create(system, &options, &window, NULL), ==, TB_WINDOW_OK);
    loop = tb_game_loop_create(
        &(tb_game_loop_options){.system = system, .window = window, .settings = &engine});
    TB_TEST_EXPECT(tb_game_loop_step(loop));
    tb_window_state_of(window, &state);
    tb_window_system_display(system, state.display, &display);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(sizes); index++)
    {
        /* A new size each time, as if the player dragged the window's border. */
        options.width = sizes[index][0];
        options.height = sizes[index][1];
        TB_TEST_CONTEXT("resize %u x %u on %s", options.width, options.height,
                        tb_window_backend_name(tb_window_system_backend(system)));
        TB_TEST_REQUIRE_UINT(tb_window_configure(window, &options, NULL), ==, TB_WINDOW_OK);
        step_until(loop, system, window, TB_WINDOW_MODE_WINDOWED, options.width, options.height);
        shortcut = fullscreen_shortcut(system, window);
        TB_TEST_EXPECT_UINT(shortcut.state.width, ==, options.width);
        TB_TEST_EXPECT_UINT(shortcut.state.height, ==, options.height);
        tb_game_loop_inject(loop, &shortcut);
        step_until(loop, system, window, TB_WINDOW_MODE_BORDERLESS, display.pixel_width,
                   display.pixel_height);
        shortcut = fullscreen_shortcut(system, window);
        TB_TEST_EXPECT_UINT(shortcut.state.mode, ==, TB_WINDOW_MODE_BORDERLESS);
        TB_TEST_EXPECT_UINT(shortcut.state.pixel_width, ==, display.pixel_width);
        TB_TEST_EXPECT_UINT(shortcut.state.pixel_height, ==, display.pixel_height);

        tb_game_loop_inject(loop, &shortcut);
        step_until(loop, system, window, TB_WINDOW_MODE_WINDOWED, options.width, options.height);
        tb_window_state_of(window, &state);
        TB_TEST_EXPECT_UINT(state.mode, ==, TB_WINDOW_MODE_WINDOWED);
        TB_TEST_EXPECT_UINT(state.width, ==, options.width);
        TB_TEST_EXPECT_UINT(state.height, ==, options.height);
    }

    /* A new windowed size requested while fullscreen replaces the one
     * remembered by both the shortcut and the compositor. */
    shortcut = fullscreen_shortcut(system, window);
    tb_game_loop_inject(loop, &shortcut);
    step_until(loop, system, window, TB_WINDOW_MODE_BORDERLESS, display.pixel_width,
               display.pixel_height);
    (void)tb_settings_set(settings, TB_SETTING_LAYER_RUNTIME, "display.window_width", "720");
    (void)tb_settings_set(settings, TB_SETTING_LAYER_RUNTIME, "display.window_height", "405");
    TB_TEST_EXPECT(tb_game_loop_step(loop));
    shortcut = fullscreen_shortcut(system, window);
    tb_game_loop_inject(loop, &shortcut);
    step_until(loop, system, window, TB_WINDOW_MODE_WINDOWED, 720, 405);
    tb_window_state_of(window, &state);
    TB_TEST_EXPECT_UINT(state.width, ==, 720);
    TB_TEST_EXPECT_UINT(state.height, ==, 405);
    tb_game_loop_destroy(loop);
    tb_window_destroy(window);
    tb_window_system_close(system);
    tb_settings_destroy(settings);
}

/* Changing the window mode setting while the game runs changes the window
 * before the next frame, and the loop reads the window's events. */
TB_TEST(game_loop, changed_display_settings_reconfigure_the_window)
{
    tb_window_system *system = NULL;
    tb_window *window = NULL;
    tb_window_options options = {0};
    tb_window_state state = {0};
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};
    tb_game_loop *loop = NULL;

    if (tb_window_system_open(NULL, &system, NULL) != TB_WINDOW_OK)
    {
        tb_settings_destroy(settings);
        TB_TEST_SKIP("no windowing here");
    }
    tb_engine_settings_add(settings, &(tb_engine_settings_options){.developer = true}, &engine);
    (void)tb_settings_set(settings, TB_SETTING_LAYER_RUNTIME, "display.window_width", "640");
    (void)tb_settings_set(settings, TB_SETTING_LAYER_RUNTIME, "display.window_height", "360");
    tb_engine_settings_window_options(&engine, "loop", &options);
    TB_TEST_REQUIRE_UINT(tb_window_create(system, &options, &window, NULL), ==, TB_WINDOW_OK);
    loop = tb_game_loop_create(
        &(tb_game_loop_options){.system = system, .window = window, .settings = &engine});
    TB_TEST_EXPECT(tb_game_loop_step(loop));
    (void)tb_settings_set(settings, TB_SETTING_LAYER_RUNTIME, "display.window_mode", "borderless");
    TB_TEST_EXPECT(tb_game_loop_step(loop));
    tb_window_state_of(window, &state);
    TB_TEST_EXPECT_UINT(state.mode, ==, TB_WINDOW_MODE_BORDERLESS);
    tb_game_loop_destroy(loop);
    tb_window_destroy(window);
    tb_window_system_close(system);
    tb_settings_destroy(settings);
}

/* ---------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void create_without_settings(void *context)
{
    (void)context;
    (void)tb_game_loop_create(&(tb_game_loop_options){0});
}

TB_TEST(game_loop, a_loop_without_settings_violates_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(create_without_settings, NULL, "a game loop has the engine's settings");
}
