/* Keys: their names, the kinds of value they give, and the window's keys
 * and mouse buttons as keys. */
#include "input_internal.h"

#include "badger/assert.h"
#include "badger/str.h"

#include <string.h>

TB_STATIC_ASSERT(TB_INPUT_KEY_DELETE - TB_INPUT_KEY_A == TB_KEY_DELETE - TB_KEY_A,
                 "the keyboard's keys are in the order of tb_key");
TB_STATIC_ASSERT(TB_KEY_A == 1, "tb_key's first key follows TB_KEY_UNKNOWN");
TB_STATIC_ASSERT(TB_KEY_DELETE + 1 == TB_KEY_COUNT, "every tb_key has an input key");

static const char *const tb_input_key_names[TB_INPUT_KEY_COUNT] = {
    [TB_INPUT_KEY_NONE] = "",
    [TB_INPUT_KEY_A] = "a",
    [TB_INPUT_KEY_B] = "b",
    [TB_INPUT_KEY_C] = "c",
    [TB_INPUT_KEY_D] = "d",
    [TB_INPUT_KEY_E] = "e",
    [TB_INPUT_KEY_F] = "f",
    [TB_INPUT_KEY_G] = "g",
    [TB_INPUT_KEY_H] = "h",
    [TB_INPUT_KEY_I] = "i",
    [TB_INPUT_KEY_J] = "j",
    [TB_INPUT_KEY_K] = "k",
    [TB_INPUT_KEY_L] = "l",
    [TB_INPUT_KEY_M] = "m",
    [TB_INPUT_KEY_N] = "n",
    [TB_INPUT_KEY_O] = "o",
    [TB_INPUT_KEY_P] = "p",
    [TB_INPUT_KEY_Q] = "q",
    [TB_INPUT_KEY_R] = "r",
    [TB_INPUT_KEY_S] = "s",
    [TB_INPUT_KEY_T] = "t",
    [TB_INPUT_KEY_U] = "u",
    [TB_INPUT_KEY_V] = "v",
    [TB_INPUT_KEY_W] = "w",
    [TB_INPUT_KEY_X] = "x",
    [TB_INPUT_KEY_Y] = "y",
    [TB_INPUT_KEY_Z] = "z",
    [TB_INPUT_KEY_0] = "0",
    [TB_INPUT_KEY_1] = "1",
    [TB_INPUT_KEY_2] = "2",
    [TB_INPUT_KEY_3] = "3",
    [TB_INPUT_KEY_4] = "4",
    [TB_INPUT_KEY_5] = "5",
    [TB_INPUT_KEY_6] = "6",
    [TB_INPUT_KEY_7] = "7",
    [TB_INPUT_KEY_8] = "8",
    [TB_INPUT_KEY_9] = "9",
    [TB_INPUT_KEY_F1] = "f1",
    [TB_INPUT_KEY_F2] = "f2",
    [TB_INPUT_KEY_F3] = "f3",
    [TB_INPUT_KEY_F4] = "f4",
    [TB_INPUT_KEY_F5] = "f5",
    [TB_INPUT_KEY_F6] = "f6",
    [TB_INPUT_KEY_F7] = "f7",
    [TB_INPUT_KEY_F8] = "f8",
    [TB_INPUT_KEY_F9] = "f9",
    [TB_INPUT_KEY_F10] = "f10",
    [TB_INPUT_KEY_F11] = "f11",
    [TB_INPUT_KEY_F12] = "f12",
    [TB_INPUT_KEY_ESCAPE] = "escape",
    [TB_INPUT_KEY_ENTER] = "enter",
    [TB_INPUT_KEY_SPACE] = "space",
    [TB_INPUT_KEY_TAB] = "tab",
    [TB_INPUT_KEY_BACKSPACE] = "backspace",
    [TB_INPUT_KEY_GRAVE] = "grave",
    [TB_INPUT_KEY_LEFT] = "left",
    [TB_INPUT_KEY_RIGHT] = "right",
    [TB_INPUT_KEY_UP] = "up",
    [TB_INPUT_KEY_DOWN] = "down",
    [TB_INPUT_KEY_LEFT_SHIFT] = "left_shift",
    [TB_INPUT_KEY_RIGHT_SHIFT] = "right_shift",
    [TB_INPUT_KEY_LEFT_CONTROL] = "left_control",
    [TB_INPUT_KEY_RIGHT_CONTROL] = "right_control",
    [TB_INPUT_KEY_LEFT_ALT] = "left_alt",
    [TB_INPUT_KEY_RIGHT_ALT] = "right_alt",
    [TB_INPUT_KEY_HOME] = "home",
    [TB_INPUT_KEY_END] = "end",
    [TB_INPUT_KEY_PAGE_UP] = "page_up",
    [TB_INPUT_KEY_PAGE_DOWN] = "page_down",
    [TB_INPUT_KEY_INSERT] = "insert",
    [TB_INPUT_KEY_DELETE] = "delete",
    [TB_INPUT_KEY_MOUSE_LEFT] = "mouse_left",
    [TB_INPUT_KEY_MOUSE_RIGHT] = "mouse_right",
    [TB_INPUT_KEY_MOUSE_MIDDLE] = "mouse_middle",
    [TB_INPUT_KEY_MOUSE_THUMB_1] = "mouse_thumb_1",
    [TB_INPUT_KEY_MOUSE_THUMB_2] = "mouse_thumb_2",
    [TB_INPUT_KEY_MOUSE_WHEEL_UP] = "mouse_wheel_up",
    [TB_INPUT_KEY_MOUSE_WHEEL_DOWN] = "mouse_wheel_down",
    [TB_INPUT_KEY_MOUSE_WHEEL] = "mouse_wheel",
    [TB_INPUT_KEY_MOUSE_X] = "mouse_x",
    [TB_INPUT_KEY_MOUSE_Y] = "mouse_y",
    [TB_INPUT_KEY_MOUSE_XY] = "mouse_xy",
    [TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = "gamepad_face_south",
    [TB_INPUT_KEY_GAMEPAD_FACE_EAST] = "gamepad_face_east",
    [TB_INPUT_KEY_GAMEPAD_FACE_WEST] = "gamepad_face_west",
    [TB_INPUT_KEY_GAMEPAD_FACE_NORTH] = "gamepad_face_north",
    [TB_INPUT_KEY_GAMEPAD_LEFT_SHOULDER] = "gamepad_left_shoulder",
    [TB_INPUT_KEY_GAMEPAD_RIGHT_SHOULDER] = "gamepad_right_shoulder",
    [TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER] = "gamepad_left_trigger",
    [TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER] = "gamepad_right_trigger",
    [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X] = "gamepad_left_stick_x",
    [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y] = "gamepad_left_stick_y",
    [TB_INPUT_KEY_GAMEPAD_LEFT_STICK] = "gamepad_left_stick",
    [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_X] = "gamepad_right_stick_x",
    [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_Y] = "gamepad_right_stick_y",
    [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK] = "gamepad_right_stick",
    [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_CLICK] = "gamepad_left_stick_click",
    [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_CLICK] = "gamepad_right_stick_click",
    [TB_INPUT_KEY_GAMEPAD_DPAD_UP] = "gamepad_dpad_up",
    [TB_INPUT_KEY_GAMEPAD_DPAD_DOWN] = "gamepad_dpad_down",
    [TB_INPUT_KEY_GAMEPAD_DPAD_LEFT] = "gamepad_dpad_left",
    [TB_INPUT_KEY_GAMEPAD_DPAD_RIGHT] = "gamepad_dpad_right",
    [TB_INPUT_KEY_GAMEPAD_START] = "gamepad_start",
    [TB_INPUT_KEY_GAMEPAD_BACK] = "gamepad_back",
    [TB_INPUT_KEY_GAMEPAD_GUIDE] = "gamepad_guide",
    [TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_1] = "gamepad_left_paddle_1",
    [TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_2] = "gamepad_left_paddle_2",
    [TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_1] = "gamepad_right_paddle_1",
    [TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_2] = "gamepad_right_paddle_2",
    [TB_INPUT_KEY_TOUCH_1] = "touch_1",
    [TB_INPUT_KEY_TOUCH_2] = "touch_2",
    [TB_INPUT_KEY_TOUCH_3] = "touch_3",
    [TB_INPUT_KEY_TOUCH_4] = "touch_4",
    [TB_INPUT_KEY_TOUCH_5] = "touch_5",
    [TB_INPUT_KEY_TOUCH_6] = "touch_6",
    [TB_INPUT_KEY_TOUCH_7] = "touch_7",
    [TB_INPUT_KEY_TOUCH_8] = "touch_8",
    [TB_INPUT_KEY_TOUCH_9] = "touch_9",
    [TB_INPUT_KEY_TOUCH_10] = "touch_10",
    [TB_INPUT_KEY_TOUCH_1_POSITION] = "touch_1_position",
    [TB_INPUT_KEY_TOUCH_2_POSITION] = "touch_2_position",
    [TB_INPUT_KEY_TOUCH_3_POSITION] = "touch_3_position",
    [TB_INPUT_KEY_TOUCH_4_POSITION] = "touch_4_position",
    [TB_INPUT_KEY_TOUCH_5_POSITION] = "touch_5_position",
    [TB_INPUT_KEY_TOUCH_6_POSITION] = "touch_6_position",
    [TB_INPUT_KEY_TOUCH_7_POSITION] = "touch_7_position",
    [TB_INPUT_KEY_TOUCH_8_POSITION] = "touch_8_position",
    [TB_INPUT_KEY_TOUCH_9_POSITION] = "touch_9_position",
    [TB_INPUT_KEY_TOUCH_10_POSITION] = "touch_10_position",
};

TB_STATIC_ASSERT(TB_INPUT_KEY_TOUCH_10 - TB_INPUT_KEY_TOUCH_1 + 1 == TB_INPUT_TOUCH_COUNT,
                 "a key for every touch");
TB_STATIC_ASSERT(TB_INPUT_KEY_TOUCH_10_POSITION - TB_INPUT_KEY_TOUCH_1_POSITION + 1 ==
                     TB_INPUT_TOUCH_COUNT,
                 "a position for every touch");

const char *tb_input_key_name(tb_input_key key)
{
    if ((unsigned)key >= (unsigned)TB_INPUT_KEY_COUNT)
    {
        return "";
    }
    return tb_input_key_names[key];
}

tb_input_key tb_input_find_key(const char *name, size_t length)
{
    TB_ASSERT(name != NULL || length == 0);
    for (size_t key = 1; key < TB_INPUT_KEY_COUNT; key++)
    {
        const char *const candidate = tb_input_key_names[key];
        if (tb_str_length(candidate) == length && memcmp(candidate, name, length) == 0)
        {
            return (tb_input_key)key;
        }
    }
    return TB_INPUT_KEY_NONE;
}

/* The kind of each key that is not a button. */
static const unsigned char tb_input_key_kinds[TB_INPUT_KEY_COUNT] = {
    [TB_INPUT_KEY_MOUSE_WHEEL_UP] = TB_INPUT_KEY_KIND_RELATIVE,
    [TB_INPUT_KEY_MOUSE_WHEEL_DOWN] = TB_INPUT_KEY_KIND_RELATIVE,
    [TB_INPUT_KEY_MOUSE_WHEEL] = TB_INPUT_KEY_KIND_RELATIVE,
    [TB_INPUT_KEY_MOUSE_X] = TB_INPUT_KEY_KIND_RELATIVE,
    [TB_INPUT_KEY_MOUSE_Y] = TB_INPUT_KEY_KIND_RELATIVE,
    [TB_INPUT_KEY_MOUSE_XY] = TB_INPUT_KEY_KIND_COMPOSITE,
    [TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER] = TB_INPUT_KEY_KIND_AXIS,
    [TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER] = TB_INPUT_KEY_KIND_AXIS,
    [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X] = TB_INPUT_KEY_KIND_AXIS,
    [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y] = TB_INPUT_KEY_KIND_AXIS,
    [TB_INPUT_KEY_GAMEPAD_LEFT_STICK] = TB_INPUT_KEY_KIND_COMPOSITE,
    [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_X] = TB_INPUT_KEY_KIND_AXIS,
    [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_Y] = TB_INPUT_KEY_KIND_AXIS,
    [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK] = TB_INPUT_KEY_KIND_COMPOSITE,
    [TB_INPUT_KEY_TOUCH_1_POSITION] = TB_INPUT_KEY_KIND_POSITION,
    [TB_INPUT_KEY_TOUCH_2_POSITION] = TB_INPUT_KEY_KIND_POSITION,
    [TB_INPUT_KEY_TOUCH_3_POSITION] = TB_INPUT_KEY_KIND_POSITION,
    [TB_INPUT_KEY_TOUCH_4_POSITION] = TB_INPUT_KEY_KIND_POSITION,
    [TB_INPUT_KEY_TOUCH_5_POSITION] = TB_INPUT_KEY_KIND_POSITION,
    [TB_INPUT_KEY_TOUCH_6_POSITION] = TB_INPUT_KEY_KIND_POSITION,
    [TB_INPUT_KEY_TOUCH_7_POSITION] = TB_INPUT_KEY_KIND_POSITION,
    [TB_INPUT_KEY_TOUCH_8_POSITION] = TB_INPUT_KEY_KIND_POSITION,
    [TB_INPUT_KEY_TOUCH_9_POSITION] = TB_INPUT_KEY_KIND_POSITION,
    [TB_INPUT_KEY_TOUCH_10_POSITION] = TB_INPUT_KEY_KIND_POSITION,
};

TB_STATIC_ASSERT(TB_INPUT_KEY_KIND_BUTTON == 0, "a key the table leaves out is a button");

tb_input_key_kind tb_input_key_kind_of(tb_input_key key)
{
    TB_ASSERT(key > TB_INPUT_KEY_NONE && key < TB_INPUT_KEY_COUNT);
    return (tb_input_key_kind)tb_input_key_kinds[key];
}

void tb_input_key_components(tb_input_key key, tb_input_key *x, tb_input_key *y)
{
    TB_ASSERT(tb_input_key_kind_of(key) == TB_INPUT_KEY_KIND_COMPOSITE);
    if (key == TB_INPUT_KEY_MOUSE_XY)
    {
        *x = TB_INPUT_KEY_MOUSE_X;
        *y = TB_INPUT_KEY_MOUSE_Y;
        return;
    }
    if (key == TB_INPUT_KEY_GAMEPAD_LEFT_STICK)
    {
        *x = TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X;
        *y = TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y;
        return;
    }
    *x = TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_X;
    *y = TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_Y;
}

bool tb_input_key_is_window_button(tb_input_key key)
{
    return key >= TB_INPUT_KEY_A && key <= TB_INPUT_KEY_MOUSE_THUMB_2;
}

tb_input_value_type tb_input_key_type(tb_input_key key)
{
    switch (tb_input_key_kind_of(key))
    {
    case TB_INPUT_KEY_KIND_BUTTON:
        return TB_INPUT_VALUE_BOOL;
    case TB_INPUT_KEY_KIND_AXIS:
    case TB_INPUT_KEY_KIND_RELATIVE:
        return TB_INPUT_VALUE_AXIS1D;
    case TB_INPUT_KEY_KIND_COMPOSITE:
    case TB_INPUT_KEY_KIND_POSITION:
        return TB_INPUT_VALUE_AXIS2D;
    }
    /* coverage-off: every kind is handled above; this only quiets compilers */
    return TB_INPUT_VALUE_BOOL;
    /* coverage-on */
}

tb_input_key tb_input_key_from_window(tb_key key)
{
    if (key <= TB_KEY_UNKNOWN || key >= TB_KEY_COUNT)
    {
        return TB_INPUT_KEY_NONE;
    }
    return (tb_input_key)(TB_INPUT_KEY_A + (key - TB_KEY_A));
}

tb_input_key tb_input_key_from_mouse_button(tb_mouse_button button)
{
    switch (button)
    {
    case TB_MOUSE_BUTTON_LEFT:
        return TB_INPUT_KEY_MOUSE_LEFT;
    case TB_MOUSE_BUTTON_RIGHT:
        return TB_INPUT_KEY_MOUSE_RIGHT;
    case TB_MOUSE_BUTTON_MIDDLE:
        return TB_INPUT_KEY_MOUSE_MIDDLE;
    case TB_MOUSE_BUTTON_X1:
        return TB_INPUT_KEY_MOUSE_THUMB_1;
    case TB_MOUSE_BUTTON_X2:
        return TB_INPUT_KEY_MOUSE_THUMB_2;
    case TB_MOUSE_BUTTON_NONE:
    case TB_MOUSE_BUTTON_COUNT:
        break;
    }
    return TB_INPUT_KEY_NONE;
}
