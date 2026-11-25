/* Message dialogs, after Tk's `messagebox`: a title, a message, and a row
 * of buttons, made of frames, labels, and buttons in grids, shown modally
 * over the root and destroyed when a button is chosen. The dialog is a
 * widget of a kind of its own, which holds the callback and its argument. */
#include "gui_internal.h"

#include "badger/assert.h"

#include <stddef.h>

/* Units a dialog's message wraps at, and its title's size in ems. */
#define TB_GUI_MESSAGE_WRAP 360.0f
#define TB_GUI_MESSAGE_TITLE_EMS 1.25f
/* The most buttons a dialog has. */
#define TB_GUI_MESSAGE_MOST_BUTTONS 3

typedef struct tb_gui_message
{
    tb_message_callback *callback;
    void *user;
    tb_widget buttons[TB_GUI_MESSAGE_MOST_BUTTONS];
    tb_message_result results[TB_GUI_MESSAGE_MOST_BUTTONS];
    size_t button_count;
    tb_message_result escape; /* what Escape chooses */
} tb_gui_message;

/* The dialog's buttons, their answers, and what Escape answers. */
typedef struct tb_gui_message_set
{
    const char *labels[TB_GUI_MESSAGE_MOST_BUTTONS];
    tb_message_result results[TB_GUI_MESSAGE_MOST_BUTTONS];
    size_t count;
    tb_message_result escape;
} tb_gui_message_set;

static tb_gui_message_set tb_gui_message_set_of(tb_message_buttons buttons)
{
    switch (buttons)
    {
    case TB_MESSAGE_OK_CANCEL:
        return (tb_gui_message_set){{"OK", "Cancel"},
                                    {TB_MESSAGE_RESULT_OK, TB_MESSAGE_RESULT_CANCEL},
                                    2,
                                    TB_MESSAGE_RESULT_CANCEL};
    case TB_MESSAGE_YES_NO:
        return (tb_gui_message_set){
            {"Yes", "No"}, {TB_MESSAGE_RESULT_YES, TB_MESSAGE_RESULT_NO}, 2, TB_MESSAGE_RESULT_NO};
    case TB_MESSAGE_YES_NO_CANCEL:
        return (tb_gui_message_set){
            {"Yes", "No", "Cancel"},
            {TB_MESSAGE_RESULT_YES, TB_MESSAGE_RESULT_NO, TB_MESSAGE_RESULT_CANCEL},
            3,
            TB_MESSAGE_RESULT_CANCEL};
    case TB_MESSAGE_RETRY_CANCEL:
        return (tb_gui_message_set){{"Retry", "Cancel"},
                                    {TB_MESSAGE_RESULT_RETRY, TB_MESSAGE_RESULT_CANCEL},
                                    2,
                                    TB_MESSAGE_RESULT_CANCEL};
    case TB_MESSAGE_OK:
        break;
    }
    return (tb_gui_message_set){{"OK"}, {TB_MESSAGE_RESULT_OK}, 1, TB_MESSAGE_RESULT_OK};
}

static const tb_widget_class tb_gui_message_kind = {
    .name = "messagebox", .data_size = sizeof(tb_gui_message), .mouse_filter = TB_GUI_MOUSE_PASS};

/* Answer a dialog: it ends, is destroyed, and its callback is called. */
static void tb_gui_message_answer(tb_widget dialog, tb_message_result result)
{
    const tb_gui_message *const message = tb_widget_data(dialog);
    tb_message_callback *const callback = message->callback;
    void *const user = message->user;

    tb_widget_end_modal(dialog);
    tb_widget_destroy(dialog);
    if (callback != NULL)
    {
        callback(result, user);
    }
}

/* The dialog a widget in it belongs to. */
static tb_widget tb_gui_message_of(tb_widget widget)
{
    tb_widget at = widget;

    while (tb_widget_alive(at) && tb_widget_class_of(at) != &tb_gui_message_kind)
    {
        at = tb_widget_parent(at);
    }
    return at;
}

static void tb_gui_message_on_button(const tb_gui_event *event, void *user)
{
    const tb_widget dialog = tb_gui_message_of(event->widget);
    const tb_gui_message *const message = tb_widget_data(dialog);

    (void)user;
    for (size_t index = 0; message != NULL && index < message->button_count; index++)
    {
        if (tb_widget_equal(message->buttons[index], event->widget))
        {
            tb_gui_message_answer(dialog, message->results[index]);
            return;
        }
    }
}

static void tb_gui_message_on_key(const tb_gui_event *event, void *user)
{
    const tb_gui_message *const message = tb_widget_data(event->widget);

    (void)user;
    if (event->key == TB_KEY_ESCAPE && message != NULL)
    {
        tb_gui_message_answer(event->widget, message->escape);
    }
}

static void tb_gui_message_body(tb_widget box, const char *title, const char *text)
{
    const tb_ui_theme *const theme = tb_gui_theme(box.gui);
    /* The title in the heading role's family, at its regular weight. */
    const tb_font_spec heading = {.role = TB_FONT_ROLE_HEADING,
                                  .weight = TB_FONT_WEIGHT_REGULAR,
                                  .size = theme->font_size * TB_GUI_MESSAGE_TITLE_EMS};
    const float gap = theme->item_spacing.y * 2.0f;

    if (title != NULL && title[0] != '\0')
    {
        const tb_widget label = tb_label(box, &(tb_label_desc){.text = title, .font = heading});
        (void)tb_grid(label, &(tb_grid_options){.sticky = TB_STICKY_W, .pady = {0.0f, gap}});
    }
    const tb_widget message =
        tb_label(box, &(tb_label_desc){.text = text, .wrap_length = TB_GUI_MESSAGE_WRAP});
    (void)tb_grid(message,
                  &(tb_grid_options){.row = 1, .sticky = TB_STICKY_W, .pady = {0.0f, gap}});
}

tb_widget tb_gui_message_box(tb_gui *gui, const char *title, const char *text,
                             tb_message_buttons buttons, tb_message_callback *callback, void *user)
{
    const tb_widget root = tb_gui_root(gui);
    const tb_gui_message_set set = tb_gui_message_set_of(buttons);
    const tb_ui_theme *const theme = tb_gui_theme(gui);
    size_t columns = 0;
    size_t rows = 0;

    TB_ASSERT(text != NULL);
    /* Over every cell of the root's grid, in their middle. */
    tb_gui_layout(gui);
    tb_grid_size(root, &columns, &rows);
    const tb_widget dialog = tb_widget_create(root, &tb_gui_message_kind);
    tb_gui_message *const message = tb_widget_data(dialog);
    *message = (tb_gui_message){
        .callback = callback, .user = user, .button_count = set.count, .escape = set.escape};
    (void)tb_grid(dialog, &(tb_grid_options){.row_span = rows > 0 ? (int32_t)rows : 1,
                                             .column_span = columns > 0 ? (int32_t)columns : 1,
                                             .sticky = TB_STICKY_NSEW});
    tb_grid_anchor(dialog, TB_GUI_ANCHOR_CENTER);
    const tb_widget box = tb_frame(
        dialog, &(tb_frame_desc){
                    .padding = theme->window_padding.x * 2.0f, .background = true, .border = true});
    (void)tb_grid(box, NULL);
    tb_gui_message_body(box, title, text);

    const tb_widget row = tb_frame(box, NULL);
    (void)tb_grid(row, &(tb_grid_options){.row = 2, .sticky = TB_STICKY_E});
    for (size_t index = 0; index < set.count; index++)
    {
        const tb_widget button =
            tb_button(row, &(tb_button_desc){.text = set.labels[index],
                                             .command = tb_gui_message_on_button,
                                             .is_default = index == 0});
        const float gap = index > 0 ? theme->item_spacing.x : 0.0f;
        (void)tb_grid(button, &(tb_grid_options){.column = (int32_t)index,
                                                 .sticky = TB_STICKY_EW,
                                                 .padx = {gap, 0.0f}});
        (void)tb_grid_column_configure(row, index, &(tb_grid_track){.uniform = "answers"});
        message->buttons[index] = button;
        message->results[index] = set.results[index];
    }
    (void)tb_bind(dialog, TB_GUI_EVENT_KEY, tb_gui_message_on_key, NULL);
    (void)tb_widget_show_modal(dialog);
    /* The first button has the focus: Enter or Space chooses it. */
    tb_gui_layout(gui);
    (void)tb_widget_focus(message->buttons[0]);
    return dialog;
}

const tb_widget_class *tb_message_box_class(void)
{
    return &tb_gui_message_kind;
}

void tb_gui_register_message_box(tb_gui *gui)
{
    (void)tb_gui_register_class(gui, &tb_gui_message_kind);
}
