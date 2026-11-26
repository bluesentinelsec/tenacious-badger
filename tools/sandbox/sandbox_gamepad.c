/* The gamepad scene: every connected pad, as the gamepad system reads it,
 * shown live as text over the shapes: its name and what it can do, the
 * buttons down, the sticks and triggers, and the buttons the sticks and
 * triggers make, both as tb_gamepad_virtual_buttons() makes them and as the
 * input module's keys. R rumbles every pad for a quarter of a second, as
 * strongly as `controls.gamepad.vibration` allows. */
#include "sandbox.h"

#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/gamepad.h"
#include "badger/input.h"
#include "badger/memory.h"
#include "badger/settings.h"
#include "badger/str.h"

#include <stddef.h>

/* Units from the window's edges, and between lines. */
#define GAMEPAD_MARGIN 24.0f
#define GAMEPAD_LINE 22.0f
/* The panel behind the text: its width, and the space around the text. */
#define GAMEPAD_PANEL_WIDTH 880.0f
#define GAMEPAD_PANEL_PADDING 10.0f
/* Bytes of a line of text. */
#define GAMEPAD_LINE_CAPACITY 256
/* Lines a pad takes. */
#define GAMEPAD_PAD_LINES 4u
/* The test rumble: both motors at half strength for a quarter of a second. */
#define GAMEPAD_TEST_STRENGTH 0.5f
#define GAMEPAD_TEST_SECONDS 0.25

static const char *const gamepad_virtual_names[TB_GAMEPAD_VIRTUAL_COUNT] = {
    "LS-up",   "LS-down", "LS-left",  "LS-right", "RS-up",
    "RS-down", "RS-left", "RS-right", "LT",       "RT"};

/* The input module's keys of the virtual buttons, in the same order. */
static const tb_input_key gamepad_virtual_keys[TB_GAMEPAD_VIRTUAL_COUNT] = {
    TB_INPUT_KEY_GAMEPAD_LEFT_STICK_UP,       TB_INPUT_KEY_GAMEPAD_LEFT_STICK_DOWN,
    TB_INPUT_KEY_GAMEPAD_LEFT_STICK_LEFT,     TB_INPUT_KEY_GAMEPAD_LEFT_STICK_RIGHT,
    TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_UP,      TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_DOWN,
    TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_LEFT,    TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_RIGHT,
    TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER_BUTTON, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER_BUTTON};

typedef struct gamepad_scene
{
    void *shapes;
    tb_gamepad_system *gamepads; /* NULL when the platform has none. */
    tb_input *input;             /* A player for each slot, fed its pad. */
    uint32_t virtual_buttons[TB_GAMEPAD_SLOT_COUNT];
    bool rumble_asked;
    char status[TB_GAMEPAD_MESSAGE_CAPACITY + 32];
} gamepad_scene;

bool gamepad_scene_create(sandbox *box, void **state)
{
    gamepad_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_gamepad_error error = TB_GAMEPAD_ERROR_INIT;
    const bool made = shapes_create(box, &scene->shapes);

    *state = scene;
    scene->input = tb_input_create(&(tb_input_options){.player_count = TB_GAMEPAD_SLOT_COUNT});
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        tb_input_assign_device(scene->input, TB_INPUT_DEVICE_GAMEPAD(slot), slot);
    }
    if (tb_gamepad_system_open(&scene->gamepads, &error) != TB_GAMEPAD_OK)
    {
        (void)tb_fmt_buffer(scene->status, sizeof(scene->status), "no gamepads: %s", error.message);
        (void)tb_fmt_print("badger-sandbox: gamepad: %s\n", scene->status);
    }
    return made;
}

bool gamepad_scene_event(sandbox *box, void *state, const tb_window_event *event)
{
    gamepad_scene *const scene = state;

    (void)box;
    if (scene->gamepads != NULL && event->type == TB_WINDOW_EVENT_FOCUS_LOST)
    {
        tb_gamepad_system_set_focus(scene->gamepads, false);
    }
    if (scene->gamepads != NULL && event->type == TB_WINDOW_EVENT_FOCUS_GAINED)
    {
        tb_gamepad_system_set_focus(scene->gamepads, true);
    }
    if (event->type == TB_WINDOW_EVENT_KEY_DOWN && event->key == TB_KEY_R && !event->repeat)
    {
        scene->rumble_asked = true;
        return true;
    }
    return false;
}

static float gamepad_line(sandbox *box, const char *text, float y, tb_vec4 color)
{
    tb_canvas_draw_text(box->renderer, box->fonts[SANDBOX_FONT_MONOSPACE], tb_text_from_cstr(text),
                        box->specs[SANDBOX_FONT_MONOSPACE].size, (tb_vec2){GAMEPAD_MARGIN, y},
                        color, NULL);
    return y + GAMEPAD_LINE;
}

/* Append to a line, as far as it has room. */
static void gamepad_append(char *line, size_t capacity, const char *text)
{
    const size_t length = tb_str_length(line);

    if (length < capacity)
    {
        (void)tb_fmt_buffer(line + length, capacity - length, "%s", text);
    }
}

/* Read every pad, give each to its player, and rumble when R was pressed. */
static void gamepad_update(sandbox *box, gamepad_scene *scene, double seconds)
{
    tb_gamepad_event event = {0};
    size_t rumbled = 0;

    tb_gamepad_system_set_vibration(
        scene->gamepads, (float)tb_setting_number(box->engine->controls.gamepad_vibration));
    tb_gamepad_system_poll(scene->gamepads);
    while (tb_gamepad_system_next_event(scene->gamepads, &event))
    {
        (void)tb_fmt_print("badger-sandbox: gamepad: slot %zu %s\n", event.slot,
                           event.type == TB_GAMEPAD_EVENT_CONNECTED ? "connected" : "disconnected");
    }
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        tb_gamepad_state state = {0};
        tb_gamepad_get_state(scene->gamepads, slot, &state);
        tb_input_feed_gamepad(scene->input, TB_INPUT_DEVICE_GAMEPAD(slot), &state);
        scene->virtual_buttons[slot] =
            tb_gamepad_virtual_buttons(&state, scene->virtual_buttons[slot], NULL);
        if (scene->rumble_asked &&
            tb_gamepad_rumble(scene->gamepads, slot,
                              &(tb_gamepad_rumble_desc){GAMEPAD_TEST_STRENGTH,
                                                        GAMEPAD_TEST_STRENGTH,
                                                        GAMEPAD_TEST_SECONDS}))
        {
            rumbled++;
        }
    }
    if (scene->rumble_asked)
    {
        (void)tb_fmt_buffer(scene->status, sizeof(scene->status), "R: %zu pad%s rumbled", rumbled,
                            rumbled == 1 ? "" : "s");
    }
    scene->rumble_asked = false;
    tb_input_tick(scene->input, seconds);
}

/* A pad's lines: what it is, its buttons, its axes, and its virtual
 * buttons as the helper and as the input module's keys see them. */
static float gamepad_pad_lines(sandbox *box, const gamepad_scene *scene, size_t slot, float y)
{
    const tb_vec4 plain = {0.9f, 0.9f, 0.9f, 1};
    const tb_vec4 lit = {1.0f, 0.85f, 0.4f, 1};
    tb_input_player *const player = tb_input_player_at(scene->input, slot);
    tb_gamepad_state state = {0};
    tb_gamepad_info info = {0};
    char line[GAMEPAD_LINE_CAPACITY] = {0};

    tb_gamepad_get_state(scene->gamepads, slot, &state);
    tb_gamepad_get_info(scene->gamepads, slot, &info);
    (void)tb_fmt_buffer(line, sizeof(line), "slot %zu: %s  [%04x:%04x]  rumble: %s", slot,
                        info.name, (unsigned)info.vendor, (unsigned)info.product,
                        (info.capabilities & TB_GAMEPAD_CAPABILITY_RUMBLE) != 0 ? "yes" : "no");
    y = gamepad_line(box, line, y, plain);
    (void)tb_fmt_buffer(line, sizeof(line), "  buttons:");
    for (size_t button = 0; button < TB_GAMEPAD_BUTTON_COUNT; button++)
    {
        if (tb_gamepad_button_down(&state, (tb_gamepad_button)button))
        {
            gamepad_append(line, sizeof(line), " ");
            gamepad_append(line, sizeof(line), tb_gamepad_button_name((tb_gamepad_button)button));
        }
    }
    y = gamepad_line(box, line, y, state.buttons != 0 ? lit : plain);
    (void)tb_fmt_buffer(
        line, sizeof(line), "  left %+.3f %+.3f   right %+.3f %+.3f   triggers %.3f %.3f",
        (double)state.axes[TB_GAMEPAD_AXIS_LEFT_X], (double)state.axes[TB_GAMEPAD_AXIS_LEFT_Y],
        (double)state.axes[TB_GAMEPAD_AXIS_RIGHT_X], (double)state.axes[TB_GAMEPAD_AXIS_RIGHT_Y],
        (double)state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER],
        (double)state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER]);
    y = gamepad_line(box, line, y, plain);
    (void)tb_fmt_buffer(line, sizeof(line), "  virtual (helper / input keys):");
    for (size_t index = 0; index < TB_GAMEPAD_VIRTUAL_COUNT; index++)
    {
        const bool helper = (scene->virtual_buttons[slot] & (1u << index)) != 0;
        const bool key = tb_input_key_value(player, gamepad_virtual_keys[index]).x != 0.0f;
        if (helper || key)
        {
            gamepad_append(line, sizeof(line), " ");
            gamepad_append(line, sizeof(line), gamepad_virtual_names[index]);
            /* Where the two disagree, which one has it. */
            if (helper && !key)
            {
                gamepad_append(line, sizeof(line), "(h)");
            }
            if (key && !helper)
            {
                gamepad_append(line, sizeof(line), "(k)");
            }
        }
    }
    return gamepad_line(box, line, y, scene->virtual_buttons[slot] != 0 ? lit : plain);
}

void gamepad_scene_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    gamepad_scene *const scene = state;
    const tb_canvas_view view = tb_canvas_view_logical(box->renderer);
    const tb_vec4 plain = {0.9f, 0.9f, 0.9f, 1};
    size_t connected = 0;
    float y = GAMEPAD_MARGIN;

    if (scene->gamepads != NULL)
    {
        gamepad_update(box, scene, frame->delta_seconds);
        for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
        {
            tb_gamepad_state pad = {0};
            tb_gamepad_get_state(scene->gamepads, slot, &pad);
            connected += pad.connected ? 1u : 0u;
        }
    }
    shapes_draw(box, scene->shapes, frame);
    if (box->fonts[SANDBOX_FONT_MONOSPACE] == NULL)
    {
        return;
    }
    tb_canvas_set_view(box->renderer, &view);
    tb_canvas_fill_rect(
        box->renderer,
        (tb_canvas_rect){GAMEPAD_MARGIN - GAMEPAD_PANEL_PADDING,
                         GAMEPAD_MARGIN - GAMEPAD_PANEL_PADDING, GAMEPAD_PANEL_WIDTH,
                         (float)(2u + connected * GAMEPAD_PAD_LINES) * GAMEPAD_LINE +
                             2.0f * GAMEPAD_PANEL_PADDING},
        (tb_vec4){0.05f, 0.05f, 0.07f, 0.8f});
    y = gamepad_line(box,
                     connected == 0 ? "no gamepad connected: plug one in"
                                    : "R: rumble every pad (controls.gamepad.vibration)",
                     y, plain);
    y = gamepad_line(box, scene->status, y, plain);
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT && scene->gamepads != NULL; slot++)
    {
        tb_gamepad_state pad = {0};
        tb_gamepad_get_state(scene->gamepads, slot, &pad);
        if (pad.connected)
        {
            y = gamepad_pad_lines(box, scene, slot, y);
        }
    }
}

void gamepad_scene_destroy(sandbox *box, void *state)
{
    gamepad_scene *const scene = state;

    if (scene == NULL)
    {
        return;
    }
    shapes_destroy(box, scene->shapes);
    tb_gamepad_system_close(scene->gamepads);
    tb_input_destroy(scene->input);
    tb_free(scene);
}
