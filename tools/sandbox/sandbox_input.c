/* The input scene: actions and mapping contexts read from a file in the
 * sandbox's resources, fed the window's events, and every action's state,
 * value and events shown live as text over the shapes. M puts the menu
 * context on the stack above gameplay, or takes it off: the menu maps WASD
 * too, and consumes them, so movement stops while it is up. The scene runs
 * input once a frame with the frame's time, where a game runs it once a
 * tick. */
#include "sandbox.h"

#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/input.h"
#include "badger/memory.h"
#include "badger/vfs.h"

#include <stddef.h>

/* The definitions, in the sandbox's resources (SANDBOX_RESOURCES). */
#define INPUT_FILE_PATH "input/sandbox.json"
/* Units from the window's edges, and between lines. */
#define INPUT_MARGIN 24.0f
#define INPUT_LINE 22.0f
/* The menu's priority, above gameplay's 0. */
#define INPUT_MENU_PRIORITY 10
/* Bytes of a line of text. */
#define INPUT_LINE_CAPACITY 160
/* Frames a moment of an event stays lit, so that a single tick is seen. */
#define INPUT_EVENT_FRAMES 20u
/* The events, in the order they are shown, and the letters they are shown by. */
#define INPUT_EVENT_COUNT 5u

static const tb_input_event input_events[INPUT_EVENT_COUNT] = {
    TB_INPUT_EVENT_STARTED, TB_INPUT_EVENT_ONGOING, TB_INPUT_EVENT_TRIGGERED,
    TB_INPUT_EVENT_COMPLETED, TB_INPUT_EVENT_CANCELED};
static const char input_event_letters[INPUT_EVENT_COUNT] = {'S', 'O', 'T', 'C', 'X'};
static const char *const input_state_names[] = {"none", "ongoing", "triggered"};

/* The panel behind the text: its width, and the space around the text. */
#define INPUT_PANEL_WIDTH 560.0f
#define INPUT_PANEL_PADDING 10.0f

/* The most actions the scene shows. */
#define INPUT_MOST_ACTIONS 32u

typedef struct input_scene
{
    void *shapes;
    tb_input *input;
    tb_input_player *player;
    tb_input_context gameplay;
    tb_input_context menu;
    tb_input_action toggle_menu;
    /* Frames left to show each event of each action. */
    uint32_t lit[INPUT_MOST_ACTIONS][INPUT_EVENT_COUNT];
    char error[TB_INPUT_FILE_ERROR_CAPACITY];
} input_scene;

bool input_scene_create(sandbox *box, void **state)
{
    input_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_vfs *const resources = tb_vfs_create(NULL);
    tb_input_file_error error = {0};
    bool made = false;

    *state = scene;
    made = shapes_create(box, &scene->shapes);
    scene->input = tb_input_create(NULL);
    scene->player = tb_input_player_at(scene->input, 0);
    tb_input_follow_settings(scene->input, box->engine);
    if (tb_vfs_mount_directory(resources, "", 0, SANDBOX_RESOURCES, 0) != TB_VFS_OK)
    {
        (void)tb_fmt_buffer(scene->error, sizeof(scene->error), "no resources at %s",
                            SANDBOX_RESOURCES);
    }
    else if (tb_input_load_file(scene->input, resources, INPUT_FILE_PATH, &error) !=
             TB_INPUT_FILE_OK)
    {
        (void)tb_fmt_buffer(scene->error, sizeof(scene->error), "%s", error.message);
    }
    tb_vfs_destroy(resources);
    if (scene->error[0] != '\0')
    {
        (void)tb_fmt_print("badger-sandbox: input: %s\n", scene->error);
        return made;
    }
    scene->gameplay = tb_input_find_context(scene->input, "gameplay");
    scene->menu = tb_input_find_context(scene->input, "menu");
    scene->toggle_menu = tb_input_find_action(scene->input, "toggle_menu");
    if (scene->gameplay.id != 0)
    {
        tb_input_add_context(scene->player, scene->gameplay, 0);
    }
    return made;
}

bool input_scene_event(sandbox *box, void *state, const tb_window_event *event)
{
    input_scene *const scene = state;

    (void)box;
    (void)tb_input_feed_window_event(scene->input, event);
    /* The sandbox's own keys and its camera keep working. */
    return false;
}

static void input_toggle_menu(input_scene *scene)
{
    if (scene->toggle_menu.id == 0 || scene->menu.id == 0 ||
        !tb_input_action_triggered(scene->player, scene->toggle_menu))
    {
        return;
    }
    if (!tb_input_remove_context(scene->player, scene->menu))
    {
        tb_input_add_context(scene->player, scene->menu, INPUT_MENU_PRIORITY);
    }
}

static float input_line(sandbox *box, const char *text, float y, tb_vec4 color)
{
    tb_canvas_draw_text(box->renderer, box->fonts[SANDBOX_FONT_MONOSPACE], tb_text_from_cstr(text),
                        box->specs[SANDBOX_FONT_MONOSPACE].size, (tb_vec2){INPUT_MARGIN, y}, color,
                        NULL);
    return y + INPUT_LINE;
}

static tb_vec4 input_state_color(tb_input_trigger_state state)
{
    if (state == TB_INPUT_STATE_TRIGGERED)
    {
        return (tb_vec4){1.0f, 0.85f, 0.4f, 1};
    }
    if (state == TB_INPUT_STATE_ONGOING)
    {
        return (tb_vec4){0.6f, 0.8f, 1.0f, 1};
    }
    return (tb_vec4){0.8f, 0.8f, 0.8f, 1};
}

/* An action's line: its name, state, value, time under way, and the
 * letters of its events lit in the last frames. */
static float input_action_line(sandbox *box, input_scene *scene, size_t index, float y)
{
    const tb_input_action action = tb_input_action_at(scene->input, index);
    tb_input_action_status status = {0};
    char events[INPUT_EVENT_COUNT + 1] = {0};
    char line[INPUT_LINE_CAPACITY] = {0};

    tb_input_get_action_status(scene->player, action, &status);
    for (size_t event = 0; event < INPUT_EVENT_COUNT; event++)
    {
        if ((status.events & (uint32_t)input_events[event]) != 0)
        {
            scene->lit[index][event] = INPUT_EVENT_FRAMES;
        }
        events[event] = scene->lit[index][event] > 0 ? input_event_letters[event] : '.';
        if (scene->lit[index][event] > 0)
        {
            scene->lit[index][event]--;
        }
    }
    (void)tb_fmt_buffer(line, sizeof(line), "%-12s %-9s %s  %7.2f %7.2f  %5.2fs",
                        tb_input_action_name(scene->input, action), input_state_names[status.state],
                        events, (double)status.value.x, (double)status.value.y,
                        status.elapsed_seconds);
    return input_line(box, line, y, input_state_color(status.state));
}

void input_scene_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    input_scene *const scene = state;
    const tb_canvas_view view = tb_canvas_view_logical(box->renderer);
    const tb_vec4 plain = {0.9f, 0.9f, 0.9f, 1};
    char line[INPUT_LINE_CAPACITY] = {0};
    float y = INPUT_MARGIN;

    tb_input_tick(scene->input, frame->delta_seconds);
    input_toggle_menu(scene);
    shapes_draw(box, scene->shapes, frame);
    if (box->fonts[SANDBOX_FONT_MONOSPACE] == NULL)
    {
        return;
    }
    tb_canvas_set_view(box->renderer, &view);
    const size_t lines = 2 + tb_input_action_count(scene->input);
    tb_canvas_fill_rect(box->renderer,
                        (tb_canvas_rect){INPUT_MARGIN - INPUT_PANEL_PADDING,
                                         INPUT_MARGIN - INPUT_PANEL_PADDING, INPUT_PANEL_WIDTH,
                                         (float)lines * INPUT_LINE + 2.0f * INPUT_PANEL_PADDING},
                        (tb_vec4){0.05f, 0.05f, 0.07f, 0.8f});
    if (scene->error[0] != '\0')
    {
        (void)input_line(box, scene->error, y, (tb_vec4){1.0f, 0.5f, 0.4f, 1});
        return;
    }
    (void)tb_fmt_buffer(line, sizeof(line), "contexts: gameplay%s    M: menu    events: S O T C X",
                        tb_input_has_context(scene->player, scene->menu) ? ", menu (above)" : "");
    y = input_line(box, line, y, plain);
    y = input_line(box, "action       state     events  value x value y  elapsed", y, plain);
    for (size_t index = 0;
         index < tb_input_action_count(scene->input) && index < INPUT_MOST_ACTIONS; index++)
    {
        y = input_action_line(box, scene, index, y);
    }
}

void input_scene_destroy(sandbox *box, void *state)
{
    input_scene *const scene = state;

    if (scene == NULL)
    {
        return;
    }
    shapes_destroy(box, scene->shapes);
    tb_input_destroy(scene->input);
    tb_free(scene);
}
