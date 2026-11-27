/* Prompts: how each key is shown to a player, as a label printed on it and
 * a glyph's identifier, for each family of gamepads (docs/input.md). The
 * labels are English; the glyphs are `set/element`, stable names for an
 * icon atlas and keys for a game's own strings. */
#include "input_internal.h"

#include "badger/assert.h"

/* A key as a prompt shows it. */
typedef struct tb_input_shown
{
    const char *label;
    const char *glyph;
} tb_input_shown;

/* The keyboard's, the mouse's and touches: the same for every family. */
static const tb_input_shown tb_input_shown_keys[TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {
    [TB_INPUT_KEY_NONE] = {"", ""},
    [TB_INPUT_KEY_A] = {"A", "keyboard/a"},
    [TB_INPUT_KEY_B] = {"B", "keyboard/b"},
    [TB_INPUT_KEY_C] = {"C", "keyboard/c"},
    [TB_INPUT_KEY_D] = {"D", "keyboard/d"},
    [TB_INPUT_KEY_E] = {"E", "keyboard/e"},
    [TB_INPUT_KEY_F] = {"F", "keyboard/f"},
    [TB_INPUT_KEY_G] = {"G", "keyboard/g"},
    [TB_INPUT_KEY_H] = {"H", "keyboard/h"},
    [TB_INPUT_KEY_I] = {"I", "keyboard/i"},
    [TB_INPUT_KEY_J] = {"J", "keyboard/j"},
    [TB_INPUT_KEY_K] = {"K", "keyboard/k"},
    [TB_INPUT_KEY_L] = {"L", "keyboard/l"},
    [TB_INPUT_KEY_M] = {"M", "keyboard/m"},
    [TB_INPUT_KEY_N] = {"N", "keyboard/n"},
    [TB_INPUT_KEY_O] = {"O", "keyboard/o"},
    [TB_INPUT_KEY_P] = {"P", "keyboard/p"},
    [TB_INPUT_KEY_Q] = {"Q", "keyboard/q"},
    [TB_INPUT_KEY_R] = {"R", "keyboard/r"},
    [TB_INPUT_KEY_S] = {"S", "keyboard/s"},
    [TB_INPUT_KEY_T] = {"T", "keyboard/t"},
    [TB_INPUT_KEY_U] = {"U", "keyboard/u"},
    [TB_INPUT_KEY_V] = {"V", "keyboard/v"},
    [TB_INPUT_KEY_W] = {"W", "keyboard/w"},
    [TB_INPUT_KEY_X] = {"X", "keyboard/x"},
    [TB_INPUT_KEY_Y] = {"Y", "keyboard/y"},
    [TB_INPUT_KEY_Z] = {"Z", "keyboard/z"},
    [TB_INPUT_KEY_0] = {"0", "keyboard/0"},
    [TB_INPUT_KEY_1] = {"1", "keyboard/1"},
    [TB_INPUT_KEY_2] = {"2", "keyboard/2"},
    [TB_INPUT_KEY_3] = {"3", "keyboard/3"},
    [TB_INPUT_KEY_4] = {"4", "keyboard/4"},
    [TB_INPUT_KEY_5] = {"5", "keyboard/5"},
    [TB_INPUT_KEY_6] = {"6", "keyboard/6"},
    [TB_INPUT_KEY_7] = {"7", "keyboard/7"},
    [TB_INPUT_KEY_8] = {"8", "keyboard/8"},
    [TB_INPUT_KEY_9] = {"9", "keyboard/9"},
    [TB_INPUT_KEY_F1] = {"F1", "keyboard/f1"},
    [TB_INPUT_KEY_F2] = {"F2", "keyboard/f2"},
    [TB_INPUT_KEY_F3] = {"F3", "keyboard/f3"},
    [TB_INPUT_KEY_F4] = {"F4", "keyboard/f4"},
    [TB_INPUT_KEY_F5] = {"F5", "keyboard/f5"},
    [TB_INPUT_KEY_F6] = {"F6", "keyboard/f6"},
    [TB_INPUT_KEY_F7] = {"F7", "keyboard/f7"},
    [TB_INPUT_KEY_F8] = {"F8", "keyboard/f8"},
    [TB_INPUT_KEY_F9] = {"F9", "keyboard/f9"},
    [TB_INPUT_KEY_F10] = {"F10", "keyboard/f10"},
    [TB_INPUT_KEY_F11] = {"F11", "keyboard/f11"},
    [TB_INPUT_KEY_F12] = {"F12", "keyboard/f12"},
    [TB_INPUT_KEY_ESCAPE] = {"Escape", "keyboard/escape"},
    [TB_INPUT_KEY_ENTER] = {"Enter", "keyboard/enter"},
    [TB_INPUT_KEY_SPACE] = {"Space", "keyboard/space"},
    [TB_INPUT_KEY_TAB] = {"Tab", "keyboard/tab"},
    [TB_INPUT_KEY_BACKSPACE] = {"Backspace", "keyboard/backspace"},
    [TB_INPUT_KEY_GRAVE] = {"`", "keyboard/grave"},
    [TB_INPUT_KEY_LEFT] = {"Left Arrow", "keyboard/left"},
    [TB_INPUT_KEY_RIGHT] = {"Right Arrow", "keyboard/right"},
    [TB_INPUT_KEY_UP] = {"Up Arrow", "keyboard/up"},
    [TB_INPUT_KEY_DOWN] = {"Down Arrow", "keyboard/down"},
    [TB_INPUT_KEY_LEFT_SHIFT] = {"Left Shift", "keyboard/left_shift"},
    [TB_INPUT_KEY_RIGHT_SHIFT] = {"Right Shift", "keyboard/right_shift"},
    [TB_INPUT_KEY_LEFT_CONTROL] = {"Left Ctrl", "keyboard/left_control"},
    [TB_INPUT_KEY_RIGHT_CONTROL] = {"Right Ctrl", "keyboard/right_control"},
    [TB_INPUT_KEY_LEFT_ALT] = {"Left Alt", "keyboard/left_alt"},
    [TB_INPUT_KEY_RIGHT_ALT] = {"Right Alt", "keyboard/right_alt"},
    [TB_INPUT_KEY_HOME] = {"Home", "keyboard/home"},
    [TB_INPUT_KEY_END] = {"End", "keyboard/end"},
    [TB_INPUT_KEY_PAGE_UP] = {"Page Up", "keyboard/page_up"},
    [TB_INPUT_KEY_PAGE_DOWN] = {"Page Down", "keyboard/page_down"},
    [TB_INPUT_KEY_INSERT] = {"Insert", "keyboard/insert"},
    [TB_INPUT_KEY_DELETE] = {"Delete", "keyboard/delete"},
    [TB_INPUT_KEY_MOUSE_LEFT] = {"Left Mouse Button", "mouse/left"},
    [TB_INPUT_KEY_MOUSE_RIGHT] = {"Right Mouse Button", "mouse/right"},
    [TB_INPUT_KEY_MOUSE_MIDDLE] = {"Middle Mouse Button", "mouse/middle"},
    [TB_INPUT_KEY_MOUSE_THUMB_1] = {"Mouse Button 4", "mouse/thumb_1"},
    [TB_INPUT_KEY_MOUSE_THUMB_2] = {"Mouse Button 5", "mouse/thumb_2"},
    [TB_INPUT_KEY_MOUSE_WHEEL_UP] = {"Mouse Wheel Up", "mouse/wheel_up"},
    [TB_INPUT_KEY_MOUSE_WHEEL_DOWN] = {"Mouse Wheel Down", "mouse/wheel_down"},
    [TB_INPUT_KEY_MOUSE_WHEEL] = {"Mouse Wheel", "mouse/wheel"},
    [TB_INPUT_KEY_MOUSE_X] = {"Mouse X", "mouse/x"},
    [TB_INPUT_KEY_MOUSE_Y] = {"Mouse Y", "mouse/y"},
    [TB_INPUT_KEY_MOUSE_XY] = {"Mouse", "mouse/xy"},
};

/* Touches, by their number from 1. */
static const tb_input_shown tb_input_shown_touches[TB_INPUT_TOUCH_COUNT][2] = {
    {{"Touch 1", "touch/1"}, {"Touch 1 Position", "touch/1_position"}},
    {{"Touch 2", "touch/2"}, {"Touch 2 Position", "touch/2_position"}},
    {{"Touch 3", "touch/3"}, {"Touch 3 Position", "touch/3_position"}},
    {{"Touch 4", "touch/4"}, {"Touch 4 Position", "touch/4_position"}},
    {{"Touch 5", "touch/5"}, {"Touch 5 Position", "touch/5_position"}},
    {{"Touch 6", "touch/6"}, {"Touch 6 Position", "touch/6_position"}},
    {{"Touch 7", "touch/7"}, {"Touch 7 Position", "touch/7_position"}},
    {{"Touch 8", "touch/8"}, {"Touch 8 Position", "touch/8_position"}},
    {{"Touch 9", "touch/9"}, {"Touch 9 Position", "touch/9_position"}},
    {{"Touch 10", "touch/10"}, {"Touch 10 Position", "touch/10_position"}},
};

#define TB_INPUT_PAD_KEY_COUNT (TB_INPUT_KEY_TOUCH_1 - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH)

/* A gamepad's keys, by family, from #TB_INPUT_KEY_GAMEPAD_FACE_SOUTH. */
static const tb_input_shown tb_input_shown_pads[TB_GAMEPAD_FAMILY_COUNT][TB_INPUT_PAD_KEY_COUNT] = {
    [TB_GAMEPAD_FAMILY_GENERIC] =
        {
            [TB_INPUT_KEY_GAMEPAD_FACE_SOUTH - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"A",
                                                                                   "generic/south"},
            [TB_INPUT_KEY_GAMEPAD_FACE_EAST - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"B",
                                                                                  "generic/east"},
            [TB_INPUT_KEY_GAMEPAD_FACE_WEST - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"X",
                                                                                  "generic/west"},
            [TB_INPUT_KEY_GAMEPAD_FACE_NORTH - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Y",
                                                                                   "generic/north"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_SHOULDER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"LB", "generic/left_shoulder"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_SHOULDER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"RB", "generic/right_shoulder"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"LT", "generic/left_trigger"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"RT", "generic/right_trigger"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick X", "generic/left_stick_x"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Y", "generic/left_stick_y"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick", "generic/left_stick"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_X -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick X", "generic/right_stick_x"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_Y -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Y", "generic/right_stick_y"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick", "generic/right_stick"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_CLICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"LS", "generic/left_stick_click"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_CLICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"RS", "generic/right_stick_click"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_UP - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Up",
                                                                                "generic/dpad_up"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Down", "generic/dpad_down"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Left", "generic/dpad_left"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Right", "generic/dpad_right"},
            [TB_INPUT_KEY_GAMEPAD_START - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Start",
                                                                              "generic/start"},
            [TB_INPUT_KEY_GAMEPAD_BACK - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Back",
                                                                             "generic/back"},
            [TB_INPUT_KEY_GAMEPAD_GUIDE - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Guide",
                                                                              "generic/guide"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_1 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Paddle 1", "generic/left_paddle_1"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_2 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Paddle 2", "generic/left_paddle_2"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_1 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Paddle 1", "generic/right_paddle_1"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_2 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Paddle 2", "generic/right_paddle_2"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_UP -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Up", "generic/left_stick_up"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Down", "generic/left_stick_down"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Left", "generic/left_stick_left"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Right", "generic/left_stick_right"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_UP -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Up", "generic/right_stick_up"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Down", "generic/right_stick_down"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Left", "generic/right_stick_left"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Right", "generic/right_stick_right"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER_BUTTON -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"LT", "generic/left_trigger"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER_BUTTON -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"RT", "generic/right_trigger"},
        },
    [TB_GAMEPAD_FAMILY_XBOX] =
        {
            [TB_INPUT_KEY_GAMEPAD_FACE_SOUTH - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"A", "xbox/a"},
            [TB_INPUT_KEY_GAMEPAD_FACE_EAST - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"B", "xbox/b"},
            [TB_INPUT_KEY_GAMEPAD_FACE_WEST - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"X", "xbox/x"},
            [TB_INPUT_KEY_GAMEPAD_FACE_NORTH - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Y", "xbox/y"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_SHOULDER - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"LB",
                                                                                      "xbox/lb"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_SHOULDER - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"RB",
                                                                                       "xbox/rb"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"LT",
                                                                                     "xbox/lt"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"RT",
                                                                                      "xbox/rt"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick X", "xbox/left_stick_x"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Y", "xbox/left_stick_y"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick", "xbox/left_stick"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_X -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick X", "xbox/right_stick_x"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_Y -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Y", "xbox/right_stick_y"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick", "xbox/right_stick"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_CLICK - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"LS",
                                                                                         "xbox/ls"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_CLICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"RS", "xbox/rs"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_UP - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Up",
                                                                                "xbox/dpad_up"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_DOWN - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Down",
                                                                                  "xbox/dpad_down"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_LEFT - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Left",
                                                                                  "xbox/dpad_left"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Right", "xbox/dpad_right"},
            [TB_INPUT_KEY_GAMEPAD_START - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Menu", "xbox/menu"},
            [TB_INPUT_KEY_GAMEPAD_BACK - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"View", "xbox/view"},
            [TB_INPUT_KEY_GAMEPAD_GUIDE - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Xbox", "xbox/guide"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_1 - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"P3",
                                                                                      "xbox/p3"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_2 - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"P4",
                                                                                      "xbox/p4"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_1 - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"P1",
                                                                                       "xbox/p1"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_2 - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"P2",
                                                                                       "xbox/p2"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_UP -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Up", "xbox/left_stick_up"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Down", "xbox/left_stick_down"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Left", "xbox/left_stick_left"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Right", "xbox/left_stick_right"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_UP -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Up", "xbox/right_stick_up"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Down", "xbox/right_stick_down"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Left", "xbox/right_stick_left"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Right", "xbox/right_stick_right"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER_BUTTON -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"LT", "xbox/lt"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER_BUTTON -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"RT", "xbox/rt"},
        },
    [TB_GAMEPAD_FAMILY_PLAYSTATION] =
        {
            [TB_INPUT_KEY_GAMEPAD_FACE_SOUTH -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Cross", "playstation/cross"},
            [TB_INPUT_KEY_GAMEPAD_FACE_EAST -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Circle", "playstation/circle"},
            [TB_INPUT_KEY_GAMEPAD_FACE_WEST -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Square", "playstation/square"},
            [TB_INPUT_KEY_GAMEPAD_FACE_NORTH -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Triangle", "playstation/triangle"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_SHOULDER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"L1", "playstation/l1"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_SHOULDER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"R1", "playstation/r1"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"L2", "playstation/l2"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"R2", "playstation/r2"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick X", "playstation/left_stick_x"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Y", "playstation/left_stick_y"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick", "playstation/left_stick"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_X -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick X", "playstation/right_stick_x"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_Y -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Y", "playstation/right_stick_y"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick", "playstation/right_stick"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_CLICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"L3", "playstation/l3"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_CLICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"R3", "playstation/r3"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_UP -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Up", "playstation/dpad_up"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Down", "playstation/dpad_down"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Left", "playstation/dpad_left"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Right", "playstation/dpad_right"},
            [TB_INPUT_KEY_GAMEPAD_START -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Options", "playstation/options"},
            [TB_INPUT_KEY_GAMEPAD_BACK - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Create",
                                                                             "playstation/create"},
            [TB_INPUT_KEY_GAMEPAD_GUIDE - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"PS",
                                                                              "playstation/ps"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_1 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Back", "playstation/left_back"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_2 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Paddle 2", "playstation/left_paddle_2"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_1 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Back", "playstation/right_back"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_2 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Paddle 2", "playstation/right_paddle_2"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_UP -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Up", "playstation/left_stick_up"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Down", "playstation/left_stick_down"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Left", "playstation/left_stick_left"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Right",
                                                 "playstation/left_stick_right"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_UP -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Up", "playstation/right_stick_up"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Down",
                                                 "playstation/right_stick_down"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Left",
                                                 "playstation/right_stick_left"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Right",
                                                 "playstation/right_stick_right"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER_BUTTON -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"L2", "playstation/l2"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER_BUTTON -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"R2", "playstation/r2"},
        },
    [TB_GAMEPAD_FAMILY_NINTENDO] =
        {
            [TB_INPUT_KEY_GAMEPAD_FACE_SOUTH - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"B",
                                                                                   "nintendo/b"},
            [TB_INPUT_KEY_GAMEPAD_FACE_EAST - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"A",
                                                                                  "nintendo/a"},
            [TB_INPUT_KEY_GAMEPAD_FACE_WEST - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Y",
                                                                                  "nintendo/y"},
            [TB_INPUT_KEY_GAMEPAD_FACE_NORTH - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"X",
                                                                                   "nintendo/x"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_SHOULDER - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"L",
                                                                                      "nintendo/l"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_SHOULDER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"R", "nintendo/r"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"ZL",
                                                                                     "nintendo/zl"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"ZR", "nintendo/zr"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick X", "nintendo/left_stick_x"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Y", "nintendo/left_stick_y"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick", "nintendo/left_stick"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_X -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick X", "nintendo/right_stick_x"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_Y -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Y", "nintendo/right_stick_y"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick", "nintendo/right_stick"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_CLICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"LS", "nintendo/ls"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_CLICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"RS", "nintendo/rs"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_UP - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Up",
                                                                                "nintendo/dpad_up"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Down", "nintendo/dpad_down"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Left", "nintendo/dpad_left"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Right", "nintendo/dpad_right"},
            [TB_INPUT_KEY_GAMEPAD_START - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"+", "nintendo/plus"},
            [TB_INPUT_KEY_GAMEPAD_BACK - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"-", "nintendo/minus"},
            [TB_INPUT_KEY_GAMEPAD_GUIDE - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Home",
                                                                              "nintendo/home"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_1 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"GL", "nintendo/gl"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_2 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Paddle 2", "nintendo/left_paddle_2"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_1 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"GR", "nintendo/gr"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_2 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Paddle 2", "nintendo/right_paddle_2"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_UP -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Up", "nintendo/left_stick_up"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Down", "nintendo/left_stick_down"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Left", "nintendo/left_stick_left"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Right", "nintendo/left_stick_right"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_UP -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Up", "nintendo/right_stick_up"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Down", "nintendo/right_stick_down"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Left", "nintendo/right_stick_left"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Right", "nintendo/right_stick_right"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER_BUTTON -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"ZL", "nintendo/zl"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER_BUTTON -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"ZR", "nintendo/zr"},
        },
    [TB_GAMEPAD_FAMILY_STEAM_DECK] =
        {
            [TB_INPUT_KEY_GAMEPAD_FACE_SOUTH - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"A",
                                                                                   "steam_deck/a"},
            [TB_INPUT_KEY_GAMEPAD_FACE_EAST - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"B",
                                                                                  "steam_deck/b"},
            [TB_INPUT_KEY_GAMEPAD_FACE_WEST - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"X",
                                                                                  "steam_deck/x"},
            [TB_INPUT_KEY_GAMEPAD_FACE_NORTH - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Y",
                                                                                   "steam_deck/y"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_SHOULDER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"L1", "steam_deck/l1"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_SHOULDER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"R1", "steam_deck/r1"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"L2", "steam_deck/l2"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"R2", "steam_deck/r2"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick X", "steam_deck/left_stick_x"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Y", "steam_deck/left_stick_y"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick", "steam_deck/left_stick"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_X -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick X", "steam_deck/right_stick_x"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_Y -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Y", "steam_deck/right_stick_y"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick", "steam_deck/right_stick"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_CLICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"L3", "steam_deck/l3"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_CLICK -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"R3", "steam_deck/r3"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_UP -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Up", "steam_deck/dpad_up"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Down", "steam_deck/dpad_down"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Left", "steam_deck/dpad_left"},
            [TB_INPUT_KEY_GAMEPAD_DPAD_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"D-pad Right", "steam_deck/dpad_right"},
            [TB_INPUT_KEY_GAMEPAD_START - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Menu",
                                                                              "steam_deck/menu"},
            [TB_INPUT_KEY_GAMEPAD_BACK - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"View",
                                                                             "steam_deck/view"},
            [TB_INPUT_KEY_GAMEPAD_GUIDE - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Steam",
                                                                              "steam_deck/steam"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_1 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"L4", "steam_deck/l4"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_2 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"L5", "steam_deck/l5"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_1 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"R4", "steam_deck/r4"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_2 -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"R5", "steam_deck/r5"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_UP -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Up", "steam_deck/left_stick_up"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Down", "steam_deck/left_stick_down"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Left", "steam_deck/left_stick_left"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_STICK_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Left Stick Right", "steam_deck/left_stick_right"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_UP -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Up", "steam_deck/right_stick_up"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_DOWN -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Down", "steam_deck/right_stick_down"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_LEFT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Left", "steam_deck/right_stick_left"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_RIGHT -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"Right Stick Right",
                                                 "steam_deck/right_stick_right"},
            [TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER_BUTTON -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"L2", "steam_deck/l2"},
            [TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER_BUTTON -
             TB_INPUT_KEY_GAMEPAD_FACE_SOUTH] = {"R2", "steam_deck/r2"},
        },
};

TB_STATIC_ASSERT(TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER_BUTTON + 1 == TB_INPUT_KEY_TOUCH_1,
                 "the gamepad's keys come before the touches");
TB_STATIC_ASSERT(TB_INPUT_KEY_TOUCH_10_POSITION + 1 == TB_INPUT_KEY_COUNT,
                 "the touches' keys come last");

void tb_input_key_prompt(tb_input_key key, tb_gamepad_family family, tb_input_prompt *prompt)
{
    const tb_gamepad_family shown =
        (unsigned)family < (unsigned)TB_GAMEPAD_FAMILY_COUNT ? family : TB_GAMEPAD_FAMILY_GENERIC;
    tb_input_shown found = {"", ""};

    TB_ASSERT(prompt != NULL);
    if ((unsigned)key >= (unsigned)TB_INPUT_KEY_COUNT)
    {
        found = (tb_input_shown){"", ""};
    }
    else if (key < TB_INPUT_KEY_GAMEPAD_FACE_SOUTH)
    {
        found = tb_input_shown_keys[key];
    }
    else if (key < TB_INPUT_KEY_TOUCH_1)
    {
        found = tb_input_shown_pads[shown][key - TB_INPUT_KEY_GAMEPAD_FACE_SOUTH];
    }
    else if (key < TB_INPUT_KEY_TOUCH_1_POSITION)
    {
        found = tb_input_shown_touches[key - TB_INPUT_KEY_TOUCH_1][0];
    }
    else
    {
        found = tb_input_shown_touches[key - TB_INPUT_KEY_TOUCH_1_POSITION][1];
    }
    *prompt = (tb_input_prompt){.label = found.label, .glyph = found.glyph};
}
