/* While the system holds the thread in a loop of its own, as Windows does
 * while the player drags a window, the window system calls its refresh, and
 * the game loop runs its frames from there: the events queued, the ticks
 * due, and a frame at the window's new size, paced as any. Calls never nest:
 * inside a refresh, polling and waiting read no messages, and a refresh that
 * comes inside one, or inside a frame, runs nothing.
 *
 * The window system here is the shared front with a backend of the test's
 * own, whose pump holds the thread for a number of refreshes, each after a
 * new size, as a drag does. It runs the same on every platform, without a
 * display. */
#include "platform/shared/window.h"

#include "badger/game_loop.h"
#include "badger/memory.h"
#include "badger/settings.h"
#include "badger/str.h"

#include "tb_test.h"

#define MILLISECOND UINT64_C(1000000)

/* The test's window system: a backend that drags its window. */
typedef struct drag
{
    tb_window_system *system;
    tb_window *window;
    size_t pumps;           /* The times the system's messages were read. */
    size_t held_refreshes;  /* The next pump holds the thread for this many refreshes. */
    uint32_t dragged_width; /* The first refresh's width; each next is a unit wider. */
    bool press_escape;      /* The drag's first message is Escape. */
} drag;

static void drag_close(tb_window_system *system)
{
    (void)system;
}

/* Hold the thread for the refreshes asked for, the window a unit wider
 * before each, as a drag of its border does. */
static void drag_pump(tb_window_system *system, uint64_t nanoseconds)
{
    drag *const state = system->native;
    const size_t held = state->held_refreshes;

    (void)nanoseconds;
    state->pumps++;
    state->held_refreshes = 0;
    if (state->press_escape)
    {
        tb_window_key(state->window, TB_KEY_ESCAPE, 1, true, false, 0);
    }
    for (size_t index = 0; index < held; index++)
    {
        tb_window_resized(state->window, state->dragged_width + (uint32_t)index, 180, 1.0);
        tb_window_refresh(system);
    }
}

static void drag_wake(tb_window_system *system)
{
    (void)system;
}

static const tb_window_ops drag_ops = {.close = drag_close, .pump = drag_pump, .wake = drag_wake};

static drag *open_drag(void)
{
    drag *const state = tb_calloc(1, sizeof(drag));
    tb_window_system *const system = tb_calloc(1, sizeof(tb_window_system));
    tb_window *const window = tb_calloc(1, sizeof(tb_window));

    system->backend = TB_WINDOW_BACKEND_AUTOMATIC;
    system->ops = &drag_ops;
    system->native = state;
    system->displays = TB_ARRAY_CREATE(NULL, tb_display);
    system->events = TB_ARRAY_CREATE(NULL, tb_window_event);
    system->windows = TB_ARRAY_CREATE(NULL, tb_window *);
    tb_window_set_displays(system, NULL, 0);
    window->system = system;
    window->state = (tb_window_state){.width = 320,
                                      .height = 180,
                                      .pixel_width = 320,
                                      .pixel_height = 180,
                                      .content_scale = 1.0,
                                      .focused = true};
    state->system = system;
    state->window = window;
    state->dragged_width = 400;
    return state;
}

static void close_drag(drag *state)
{
    tb_free(state->window);
    tb_window_system_close(state->system);
    tb_free(state);
}

/* ---------------------------------------------------------------------------
 * The window system's refresh.
 * ------------------------------------------------------------------------- */

/* What a refresh of the test's own saw. */
typedef struct seen
{
    drag *state;
    size_t refreshes;
    size_t events;       /* Events it polled. */
    uint32_t last_width; /* The width of the last resize it polled. */
    bool nest;           /* It polls, waits, and refreshes again, inside itself. */
} seen;

static void on_refresh(void *context)
{
    seen *const record = context;
    tb_window_event event = {0};

    record->refreshes++;
    if (!record->nest)
    {
        return;
    }
    while (tb_window_system_poll(record->state->system, &event))
    {
        record->events++;
        if (event.type == TB_WINDOW_EVENT_RESIZED)
        {
            record->last_width = event.state.pixel_width;
        }
    }
    tb_window_system_wait(record->state->system, 1000 * MILLISECOND);
    tb_window_refresh(record->state->system);
}

/* A window system calls the refresh set, from inside the loop the system
 * holds its thread in, and nothing once it is unset or with none. */
TB_TEST(window_refresh, is_called_from_inside_the_systems_loop)
{
    drag *const state = open_drag();
    seen record = {.state = state};
    tb_window_event event = {0};

    /* None set: a drag calls nothing. */
    state->held_refreshes = 2;
    while (tb_window_system_poll(state->system, &event))
    {
    }
    tb_window_system_set_refresh(state->system, on_refresh, &record);
    state->held_refreshes = 3;
    (void)tb_window_system_poll(state->system, &event);
    TB_TEST_EXPECT_UINT(record.refreshes, ==, 3);
    tb_window_system_set_refresh(state->system, NULL, NULL);
    state->held_refreshes = 3;
    (void)tb_window_system_poll(state->system, &event);
    TB_TEST_EXPECT_UINT(record.refreshes, ==, 3);
    close_drag(state);
}

/* Inside a refresh, polling returns what was queued without reading the
 * system's messages, waiting returns at once, and a refresh inside it is
 * dropped: the pump is never entered twice. */
TB_TEST(window_refresh, inside_a_refresh_no_messages_are_read_and_calls_do_not_nest)
{
    drag *const state = open_drag();
    seen record = {.state = state, .nest = true};
    tb_window_event event = {0};

    tb_window_system_set_refresh(state->system, on_refresh, &record);
    state->held_refreshes = 3;
    (void)tb_window_system_poll(state->system, &event);
    TB_TEST_EXPECT_UINT(state->pumps, ==, 1);
    TB_TEST_EXPECT_UINT(record.refreshes, ==, 3);
    /* Each refresh polled its resize. */
    TB_TEST_EXPECT_UINT(record.events, ==, 3);
    TB_TEST_EXPECT_UINT(record.last_width, ==, 402);
    TB_TEST_EXPECT(!state->system->refreshing);
    close_drag(state);
}

/* ---------------------------------------------------------------------------
 * The game loop's frames during a drag.
 * ------------------------------------------------------------------------- */

/* A clock that moves only when told: by a frame, or by a wait. */
typedef struct game
{
    drag *state;
    uint64_t now;
    uint64_t waits;
    uint64_t frame_cost;
    uint64_t ticks;
    uint64_t frames;
    uint32_t resized_width;     /* The width of the last resize the game was given. */
    uint32_t frame_widths[8];   /* The window's width at each frame. */
    uint32_t resized_widths[8]; /* The last resize given before each frame. */
    bool refresh_in_frame;      /* A frame refreshes, as a window it configures may. */
} game;

static uint64_t game_now(void *context)
{
    const game *const played = context;
    return played->now;
}

static void game_wait_until(void *context, uint64_t nanoseconds)
{
    game *const played = context;
    played->waits++;
    if (nanoseconds > played->now)
    {
        played->now = nanoseconds;
    }
}

static void on_tick(void *context, const tb_game_time *time)
{
    game *const played = context;
    (void)time;
    played->ticks++;
}

static void on_frame(void *context, const tb_game_time *time)
{
    game *const played = context;
    tb_window_state window_state = {0};

    (void)time;
    tb_window_state_of(played->state->window, &window_state);
    if (played->frames < 8)
    {
        played->frame_widths[played->frames] = window_state.pixel_width;
        played->resized_widths[played->frames] = played->resized_width;
    }
    played->frames++;
    played->now += played->frame_cost;
    if (played->refresh_in_frame)
    {
        tb_window_refresh(played->state->system);
    }
}

static bool on_event(void *context, const tb_window_event *event)
{
    game *const played = context;
    if (event->type == TB_WINDOW_EVENT_RESIZED)
    {
        played->resized_width = event->state.pixel_width;
    }
    return false;
}

typedef struct fixture
{
    tb_settings *settings;
    tb_engine_settings *engine;
    tb_game_clock clock;
    tb_game_loop *loop;
} fixture;

/* A developer build's loop on the drag's window, with the values that
 * `values` sets. */
static fixture make_loop(game *played, const char *values)
{
    fixture made = {0};

    made.settings = tb_settings_create();
    made.engine = tb_calloc(1, sizeof(tb_engine_settings));
    tb_engine_settings_add(made.settings, &(tb_engine_settings_options){.developer = true},
                           made.engine);
    TB_TEST_REQUIRE_UINT(tb_settings_load_text(made.settings, TB_SETTING_LAYER_COMMAND_LINE, values,
                                               tb_str_length(values), NULL, NULL),
                         ==, 0);
    made.clock = (tb_game_clock){.now = game_now, .wait_until = game_wait_until, .context = played};
    made.loop = tb_game_loop_create(&(tb_game_loop_options){
        .system = played->state->system,
        .window = played->state->window,
        .settings = made.engine,
        .clock = &made.clock,
        .callbacks = {.tick = on_tick, .frame = on_frame, .event = on_event, .context = played}});
    return made;
}

static void release_loop(fixture *made)
{
    tb_game_loop_destroy(made->loop);
    tb_settings_destroy(made->settings);
    tb_free(made->engine);
}

/* A step whose events take a drag of three sizes runs a frame at each, from
 * inside the drag, with its resize given first; ticks go on as time passes;
 * each frame is paced; and the step's own frame follows once the drag ends. */
TB_TEST(game_loop_refresh, frames_go_on_while_the_system_holds_the_thread)
{
    game played = {.state = open_drag(), .frame_cost = 20 * MILLISECOND};
    fixture made = make_loop(&played, "display.frame_limit = 10\nengine.tick_rate = 100\n");

    TB_TEST_REQUIRE(played.state->system->refresh != NULL);
    played.state->held_refreshes = 3;
    TB_TEST_EXPECT(tb_game_loop_step(made.loop));
    TB_TEST_EXPECT_UINT(played.frames, ==, 4);
    for (size_t index = 0; index < 3; index++)
    {
        TB_TEST_CONTEXT("frame %zu", index);
        TB_TEST_EXPECT_UINT(played.frame_widths[index], ==, 400 + index);
        TB_TEST_EXPECT_UINT(played.resized_widths[index], ==, 400 + index);
    }
    TB_TEST_EXPECT_UINT(played.frame_widths[3], ==, 402);
    TB_TEST_EXPECT_UINT(played.waits, ==, 4);
    /* A tenth of a second a frame, after the first, at 100 ticks a second. */
    TB_TEST_EXPECT_UINT(played.ticks, ==, 30);
    TB_TEST_EXPECT_UINT(played.state->pumps, ==, 1);
    release_loop(&made);
    TB_TEST_EXPECT(played.state->system->refresh == NULL);
    close_drag(played.state);
}

/* A refresh that comes while a frame runs, as one may when the settings
 * configure the window, runs no frame inside the frame. */
TB_TEST(game_loop_refresh, a_refresh_inside_a_frame_runs_no_frame)
{
    game played = {.state = open_drag(), .refresh_in_frame = true};
    fixture made = make_loop(&played, "display.frame_limit = 0\n");

    TB_TEST_EXPECT(tb_game_loop_step(made.loop));
    TB_TEST_EXPECT_UINT(played.frames, ==, 1);
    played.state->held_refreshes = 2;
    TB_TEST_EXPECT(tb_game_loop_step(made.loop));
    TB_TEST_EXPECT_UINT(played.frames, ==, 4);
    release_loop(&made);
    close_drag(played.state);
}

/* A game that quits during a drag runs no more frames in it: on Escape
 * before the drag's first frame, or at its frame count. */
TB_TEST(game_loop_refresh, quitting_during_a_drag_stops_its_frames)
{
    game played = {.state = open_drag()};
    fixture made = make_loop(&played, "display.frame_limit = 0\ndeveloper.frame_count = 2\n");

    played.state->held_refreshes = 5;
    TB_TEST_EXPECT(!tb_game_loop_step(made.loop));
    TB_TEST_EXPECT_UINT(played.frames, ==, 2);
    release_loop(&made);
    close_drag(played.state);

    played = (game){.state = open_drag()};
    made = make_loop(&played, "display.frame_limit = 0\n");
    played.state->press_escape = true;
    played.state->held_refreshes = 3;
    TB_TEST_EXPECT(!tb_game_loop_step(made.loop));
    TB_TEST_EXPECT_UINT(played.frames, ==, 0);
    release_loop(&made);
    close_drag(played.state);
}
