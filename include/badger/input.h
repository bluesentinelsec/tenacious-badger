/** @file input.h
 * @brief Input: actions that a game reads, and the keys, buttons and axes
 * that drive them, after Unreal's Enhanced Input.
 *
 * A game names what a player does, not what the player presses: "jump",
 * "move", "look", "select". Those are **actions**, each with a value of a
 * type: a bool, or an axis of one, two or three dimensions. **Mapping
 * contexts** ("gameplay", "menu", "vehicle") map **keys** (keyboard keys,
 * mouse buttons and motion, gamepad buttons and sticks, touches) to actions.
 * A player's input has a stack of contexts, each with a priority: a higher
 * one sees a key first, and may consume it so that lower ones do not.
 *
 * @code
 * tb_input *input = tb_input_create(NULL);
 * const tb_input_action jump = tb_input_create_action(input, &(tb_input_action_desc){
 *     .name = "jump", .type = TB_INPUT_VALUE_BOOL});
 * const tb_input_context gameplay = tb_input_create_context(input, "gameplay");
 * const tb_input_trigger pressed = {.type = TB_INPUT_TRIGGER_PRESSED};
 * tb_input_context_map(input, gameplay, &(tb_input_mapping_desc){
 *     .action = jump, .key = TB_INPUT_KEY_SPACE, .triggers = &pressed, .trigger_count = 1});
 * tb_input_player *const player = tb_input_player_at(input, 0);
 * tb_input_add_context(player, gameplay, 0);
 *
 * // As the window's events arrive:      tb_input_feed_window_event(input, &event);
 * // Once a tick, before the simulation: tb_input_tick(input, time->tick_seconds);
 * if (tb_input_action_triggered(player, jump)) { ... }
 * @endcode
 *
 * **Modifiers** change a key's value on its way to an action: a dead zone,
 * negation, a swizzle that turns W into the Y of a 2D vector, a scale, a
 * response curve, smoothing, and the player's own settings: mouse and
 * gamepad sensitivity, invert Y, and the sticks' dead zones.
 *
 * **Triggers** decide when an action fires: while down, when pressed, when
 * released, after a hold, on a tap, in pulses, or only with another action
 * (a chord). An action moves between three states, none, ongoing and
 * triggered, and each move is an **event**: started, ongoing, triggered,
 * completed or canceled. A game polls an action's value, state and events,
 * or binds callbacks to its events.
 *
 * **Ticks.** Input is gathered as it arrives and evaluated once a tick, with
 * the tick's length, so that the same input gives the same actions at any
 * frame rate. A key pressed and released between two ticks is seen down for
 * one tick and up the next, so a tap is never lost and "triggered" fires
 * once. What a tick gathered can be recorded and replayed, to give the same
 * actions again.
 *
 * **Definitions.** Actions and contexts are made in code, or loaded from a
 * JSON file (`"format": "badger-input"`) that tb_input_save() writes back.
 *
 * See `docs/input.md`.
 *
 * @par Threading
 * Stage-owned: an input, its players and its recordings are used on one
 * thread, the one that ticks it, usually the game loop's. Callbacks are
 * called on that thread from inside tb_input_tick(). The functions on keys
 * alone (tb_input_key_name(), tb_input_find_key(), tb_input_key_type(),
 * tb_input_key_from_window(), tb_input_key_from_mouse_button()) are
 * thread-safe.
 */
#ifndef TB_INPUT_H
#define TB_INPUT_H

#include "badger/api.h"
#include "badger/window.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_engine_settings;
struct tb_stream;
struct tb_vfs;

/** @brief An input: actions, contexts, players, and the input gathered for
 * the next tick. Created by tb_input_create(); owned by the caller;
 * destroyed by tb_input_destroy(). */
typedef struct tb_input tb_input;

/** @brief One player's input: the contexts added to it, the state of its
 * keys, and its actions' values. Owned by its input; see
 * tb_input_player_at(). */
typedef struct tb_input_player tb_input_player;

/** @brief The players an input can have. */
#define TB_INPUT_MAX_PLAYERS 8u

/** @brief No player: a device assigned to none is ignored. */
#define TB_INPUT_NO_PLAYER ((size_t)-1)

/** @brief A value: a key's, or an action's. A bool is 1 or 0 in `x`; an axis
 * of fewer than three dimensions leaves the others 0. */
typedef struct tb_input_value
{
    float x; /**< The first axis; a bool's value. */
    float y; /**< The second axis. */
    float z; /**< The third axis. */
} tb_input_value;

/** @brief What kind of value an action has, or a key gives. */
typedef enum tb_input_value_type
{
    TB_INPUT_VALUE_BOOL,      /**< On or off: a button. */
    TB_INPUT_VALUE_AXIS1D,    /**< A number: a trigger, the wheel, one axis of a stick. */
    TB_INPUT_VALUE_AXIS2D,    /**< Two: a stick, the mouse's motion, WASD. */
    TB_INPUT_VALUE_AXIS3D,    /**< Three: movement with up and down. */
    TB_INPUT_VALUE_TYPE_COUNT /**< The number of types; not a type. */
} tb_input_value_type;

/** @brief The number of touches the keys name. */
#define TB_INPUT_TOUCH_COUNT 10u

/**
 * @brief A key: anything a player presses, moves or touches, by its place
 * and not by its label, as Unreal's `EKeys`.
 *
 * Buttons are 1 while down and 0 while up. The mouse's motion is how far it
 * moved in a tick, and its X and Y are an axis2d together. A stick's X and Y
 * are from -1 to 1, up and right positive, and its 2D key is both. A
 * gamepad's triggers are from 0 to 1. A touch's position is in the window's
 * logical units.
 */
typedef enum tb_input_key
{
    TB_INPUT_KEY_NONE, /**< No key. */
    /* The keyboard, in the order of #tb_key. */
    TB_INPUT_KEY_A,             /**< A. */
    TB_INPUT_KEY_B,             /**< B. */
    TB_INPUT_KEY_C,             /**< C. */
    TB_INPUT_KEY_D,             /**< D. */
    TB_INPUT_KEY_E,             /**< E. */
    TB_INPUT_KEY_F,             /**< F. */
    TB_INPUT_KEY_G,             /**< G. */
    TB_INPUT_KEY_H,             /**< H. */
    TB_INPUT_KEY_I,             /**< I. */
    TB_INPUT_KEY_J,             /**< J. */
    TB_INPUT_KEY_K,             /**< K. */
    TB_INPUT_KEY_L,             /**< L. */
    TB_INPUT_KEY_M,             /**< M. */
    TB_INPUT_KEY_N,             /**< N. */
    TB_INPUT_KEY_O,             /**< O. */
    TB_INPUT_KEY_P,             /**< P. */
    TB_INPUT_KEY_Q,             /**< Q. */
    TB_INPUT_KEY_R,             /**< R. */
    TB_INPUT_KEY_S,             /**< S. */
    TB_INPUT_KEY_T,             /**< T. */
    TB_INPUT_KEY_U,             /**< U. */
    TB_INPUT_KEY_V,             /**< V. */
    TB_INPUT_KEY_W,             /**< W. */
    TB_INPUT_KEY_X,             /**< X. */
    TB_INPUT_KEY_Y,             /**< Y. */
    TB_INPUT_KEY_Z,             /**< Z. */
    TB_INPUT_KEY_0,             /**< 0, above the letters. */
    TB_INPUT_KEY_1,             /**< 1. */
    TB_INPUT_KEY_2,             /**< 2. */
    TB_INPUT_KEY_3,             /**< 3. */
    TB_INPUT_KEY_4,             /**< 4. */
    TB_INPUT_KEY_5,             /**< 5. */
    TB_INPUT_KEY_6,             /**< 6. */
    TB_INPUT_KEY_7,             /**< 7. */
    TB_INPUT_KEY_8,             /**< 8. */
    TB_INPUT_KEY_9,             /**< 9. */
    TB_INPUT_KEY_F1,            /**< F1. */
    TB_INPUT_KEY_F2,            /**< F2. */
    TB_INPUT_KEY_F3,            /**< F3. */
    TB_INPUT_KEY_F4,            /**< F4. */
    TB_INPUT_KEY_F5,            /**< F5. */
    TB_INPUT_KEY_F6,            /**< F6. */
    TB_INPUT_KEY_F7,            /**< F7. */
    TB_INPUT_KEY_F8,            /**< F8. */
    TB_INPUT_KEY_F9,            /**< F9. */
    TB_INPUT_KEY_F10,           /**< F10. */
    TB_INPUT_KEY_F11,           /**< F11. */
    TB_INPUT_KEY_F12,           /**< F12. */
    TB_INPUT_KEY_ESCAPE,        /**< Escape. */
    TB_INPUT_KEY_ENTER,         /**< Enter. */
    TB_INPUT_KEY_SPACE,         /**< The space bar. */
    TB_INPUT_KEY_TAB,           /**< Tab. */
    TB_INPUT_KEY_BACKSPACE,     /**< Backspace. */
    TB_INPUT_KEY_GRAVE,         /**< The key left of 1: ` on an American keyboard. */
    TB_INPUT_KEY_LEFT,          /**< The left arrow. */
    TB_INPUT_KEY_RIGHT,         /**< The right arrow. */
    TB_INPUT_KEY_UP,            /**< The up arrow. */
    TB_INPUT_KEY_DOWN,          /**< The down arrow. */
    TB_INPUT_KEY_LEFT_SHIFT,    /**< The left Shift. */
    TB_INPUT_KEY_RIGHT_SHIFT,   /**< The right Shift. */
    TB_INPUT_KEY_LEFT_CONTROL,  /**< The left Control. */
    TB_INPUT_KEY_RIGHT_CONTROL, /**< The right Control. */
    TB_INPUT_KEY_LEFT_ALT,      /**< The left Alt; Option on a Mac. */
    TB_INPUT_KEY_RIGHT_ALT,     /**< The right Alt. */
    TB_INPUT_KEY_HOME,          /**< Home. */
    TB_INPUT_KEY_END,           /**< End. */
    TB_INPUT_KEY_PAGE_UP,       /**< Page Up. */
    TB_INPUT_KEY_PAGE_DOWN,     /**< Page Down. */
    TB_INPUT_KEY_INSERT,        /**< Insert. */
    TB_INPUT_KEY_DELETE,        /**< Delete, forward. */
    /* The mouse. */
    TB_INPUT_KEY_MOUSE_LEFT,    /**< The primary button. */
    TB_INPUT_KEY_MOUSE_RIGHT,   /**< The secondary button. */
    TB_INPUT_KEY_MOUSE_MIDDLE,  /**< The wheel, pressed. */
    TB_INPUT_KEY_MOUSE_THUMB_1, /**< The first side button, usually "back". */
    TB_INPUT_KEY_MOUSE_THUMB_2, /**< The second side button, usually "forward". */
    /** The wheel turned away from the player, as a button: its notches in a
     * tick, 0 in a tick it did not turn. */
    TB_INPUT_KEY_MOUSE_WHEEL_UP,
    TB_INPUT_KEY_MOUSE_WHEEL_DOWN, /**< The wheel turned toward the player, as a button. */
    /** The wheel, as an axis: its notches in a tick, away from the player
     * positive. */
    TB_INPUT_KEY_MOUSE_WHEEL,
    /** The mouse's motion to the right in a tick: in relative mouse mode its
     * own motion, else the cursor's, in logical units. */
    TB_INPUT_KEY_MOUSE_X,
    /** The mouse's motion away from the player in a tick, which moves the
     * cursor up: positive is up, as Unreal's. */
    TB_INPUT_KEY_MOUSE_Y,
    TB_INPUT_KEY_MOUSE_XY, /**< #TB_INPUT_KEY_MOUSE_X and #TB_INPUT_KEY_MOUSE_Y, as an axis2d. */
    /* Gamepads, in one standard layout. */
    TB_INPUT_KEY_GAMEPAD_FACE_SOUTH,        /**< The bottom face button: A, Cross. */
    TB_INPUT_KEY_GAMEPAD_FACE_EAST,         /**< The right face button: B, Circle. */
    TB_INPUT_KEY_GAMEPAD_FACE_WEST,         /**< The left face button: X, Square. */
    TB_INPUT_KEY_GAMEPAD_FACE_NORTH,        /**< The top face button: Y, Triangle. */
    TB_INPUT_KEY_GAMEPAD_LEFT_SHOULDER,     /**< The left bumper. */
    TB_INPUT_KEY_GAMEPAD_RIGHT_SHOULDER,    /**< The right bumper. */
    TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER,      /**< The left trigger, from 0 to 1. */
    TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER,     /**< The right trigger, from 0 to 1. */
    TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X,      /**< The left stick, right positive. */
    TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y,      /**< The left stick, up positive. */
    TB_INPUT_KEY_GAMEPAD_LEFT_STICK,        /**< The left stick, as an axis2d. */
    TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_X,     /**< The right stick, right positive. */
    TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_Y,     /**< The right stick, up positive. */
    TB_INPUT_KEY_GAMEPAD_RIGHT_STICK,       /**< The right stick, as an axis2d. */
    TB_INPUT_KEY_GAMEPAD_LEFT_STICK_CLICK,  /**< The left stick, pressed. */
    TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_CLICK, /**< The right stick, pressed. */
    TB_INPUT_KEY_GAMEPAD_DPAD_UP,           /**< The d-pad's up. */
    TB_INPUT_KEY_GAMEPAD_DPAD_DOWN,         /**< The d-pad's down. */
    TB_INPUT_KEY_GAMEPAD_DPAD_LEFT,         /**< The d-pad's left. */
    TB_INPUT_KEY_GAMEPAD_DPAD_RIGHT,        /**< The d-pad's right. */
    TB_INPUT_KEY_GAMEPAD_START,             /**< Start, Menu, Options. */
    TB_INPUT_KEY_GAMEPAD_BACK,              /**< Back, View, Share, Select. */
    TB_INPUT_KEY_GAMEPAD_GUIDE,             /**< The guide button: Xbox, PS, Steam. */
    TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_1,     /**< The upper left back paddle: the Steam Deck's L4. */
    TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_2,     /**< The lower left back paddle: L5. */
    TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_1,    /**< The upper right back paddle: R4. */
    TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_2,    /**< The lower right back paddle: R5. */
    /* Touches, as buttons while a finger is down. */
    TB_INPUT_KEY_TOUCH_1,  /**< The first finger down. */
    TB_INPUT_KEY_TOUCH_2,  /**< The second. */
    TB_INPUT_KEY_TOUCH_3,  /**< The third. */
    TB_INPUT_KEY_TOUCH_4,  /**< The fourth. */
    TB_INPUT_KEY_TOUCH_5,  /**< The fifth. */
    TB_INPUT_KEY_TOUCH_6,  /**< The sixth. */
    TB_INPUT_KEY_TOUCH_7,  /**< The seventh. */
    TB_INPUT_KEY_TOUCH_8,  /**< The eighth. */
    TB_INPUT_KEY_TOUCH_9,  /**< The ninth. */
    TB_INPUT_KEY_TOUCH_10, /**< The tenth. */
    /* Where each touch is, as an axis2d. */
    TB_INPUT_KEY_TOUCH_1_POSITION,  /**< Where the first finger is. */
    TB_INPUT_KEY_TOUCH_2_POSITION,  /**< The second. */
    TB_INPUT_KEY_TOUCH_3_POSITION,  /**< The third. */
    TB_INPUT_KEY_TOUCH_4_POSITION,  /**< The fourth. */
    TB_INPUT_KEY_TOUCH_5_POSITION,  /**< The fifth. */
    TB_INPUT_KEY_TOUCH_6_POSITION,  /**< The sixth. */
    TB_INPUT_KEY_TOUCH_7_POSITION,  /**< The seventh. */
    TB_INPUT_KEY_TOUCH_8_POSITION,  /**< The eighth. */
    TB_INPUT_KEY_TOUCH_9_POSITION,  /**< The ninth. */
    TB_INPUT_KEY_TOUCH_10_POSITION, /**< The tenth. */
    TB_INPUT_KEY_COUNT              /**< The number of keys; not a key. */
} tb_input_key;

/** @brief A device a key comes from. The keyboard and the mouse are named
 * here; gamepads and touch screens get numbers of their own from
 * #TB_INPUT_DEVICE_FIRST_OTHER. */
typedef uint32_t tb_input_device;

/** @brief No device. */
#define TB_INPUT_DEVICE_NONE ((tb_input_device)0)
/** @brief The keyboard, which tb_input_feed_window_event() feeds. */
#define TB_INPUT_DEVICE_KEYBOARD ((tb_input_device)1)
/** @brief The mouse, which tb_input_feed_window_event() feeds. */
#define TB_INPUT_DEVICE_MOUSE ((tb_input_device)2)
/** @brief The first number of other devices. */
#define TB_INPUT_DEVICE_FIRST_OTHER ((tb_input_device)16)

/** @brief An action: a handle that its input gave. Zero is none. Actions
 * last as long as their input. */
typedef struct tb_input_action
{
    uint32_t id; /**< Its number in its input, from 1; 0 for none. */
} tb_input_action;

/** @brief No action. */
#define TB_INPUT_ACTION_NONE ((tb_input_action){0u})

/** @brief A mapping context: a handle that its input gave. Zero is none.
 * Contexts last as long as their input. */
typedef struct tb_input_context
{
    uint32_t id; /**< Its number in its input, from 1; 0 for none. */
} tb_input_context;

/** @brief No context. */
#define TB_INPUT_CONTEXT_NONE ((tb_input_context){0u})

/* ---------------------------------------------------------------------------
 * Modifiers.
 * ------------------------------------------------------------------------- */

/** @brief What a modifier does. */
typedef enum tb_input_modifier_type
{
    /** Values near zero are zero, and the rest are scaled from `lower` to
     * `upper` onto 0 to 1. */
    TB_INPUT_MODIFIER_DEAD_ZONE,
    TB_INPUT_MODIFIER_NEGATE,         /**< The axes of `axes` change sign. */
    TB_INPUT_MODIFIER_SWIZZLE,        /**< The axes change places, in `order`. */
    TB_INPUT_MODIFIER_SCALAR,         /**< Each axis is multiplied by `scale`'s. */
    TB_INPUT_MODIFIER_RESPONSE_CURVE, /**< Each axis is raised to `exponent`'s, keeping its sign. */
    TB_INPUT_MODIFIER_SMOOTH,         /**< The mean of the last `samples` ticks' values. */
    /** The player's settings, of `source`: sensitivity, invert Y, smoothing
     * and dead zones, as tb_input_set_settings() gives them. */
    TB_INPUT_MODIFIER_SETTINGS,
    TB_INPUT_MODIFIER_TYPE_COUNT /**< The number of types; not a type. */
} tb_input_modifier_type;

/** @brief How a dead zone measures a value. */
typedef enum tb_input_dead_zone
{
    /** By its length, keeping its direction: a stick's dead zone. */
    TB_INPUT_DEAD_ZONE_RADIAL,
    TB_INPUT_DEAD_ZONE_AXIAL, /**< Each axis alone. */
    /** By its length, as radial, but a value outside is kept as it is
     * rather than scaled. */
    TB_INPUT_DEAD_ZONE_UNSCALED_RADIAL,
    TB_INPUT_DEAD_ZONE_COUNT /**< The number of kinds; not a kind. */
} tb_input_dead_zone;

/** @brief The axes of a value, as bits. */
typedef enum tb_input_axis
{
    TB_INPUT_AXIS_X = 1u << 0, /**< X. */
    TB_INPUT_AXIS_Y = 1u << 1, /**< Y. */
    TB_INPUT_AXIS_Z = 1u << 2  /**< Z. */
} tb_input_axis;

/** @brief Where a swizzle puts the axes: YXZ makes the value (y, x, z), so
 * that a key's X lands in Y. */
typedef enum tb_input_swizzle
{
    TB_INPUT_SWIZZLE_YXZ,  /**< X and Y change places: W into the Y of a 2D vector. */
    TB_INPUT_SWIZZLE_ZYX,  /**< X and Z change places. */
    TB_INPUT_SWIZZLE_XZY,  /**< Y and Z change places. */
    TB_INPUT_SWIZZLE_YZX,  /**< (y, z, x). */
    TB_INPUT_SWIZZLE_ZXY,  /**< (z, x, y). */
    TB_INPUT_SWIZZLE_COUNT /**< The number of orders; not an order. */
} tb_input_swizzle;

/** @brief Which of the player's settings a settings modifier applies. */
typedef enum tb_input_settings_source
{
    /** Mouse look: `mouse_sensitivity` on X and Y, Y negated when
     * `mouse_invert_y`, and the mean of two ticks when `mouse_smoothing`. */
    TB_INPUT_SETTINGS_MOUSE,
    /** Gamepad look: `gamepad_horizontal_sensitivity` on X,
     * `gamepad_vertical_sensitivity` on Y, Y negated when `gamepad_invert_y`. */
    TB_INPUT_SETTINGS_GAMEPAD,
    TB_INPUT_SETTINGS_TOUCH, /**< `touch_sensitivity` on X and Y. */
    /** A radial dead zone of `gamepad_left_dead_zone`, for the left stick. */
    TB_INPUT_SETTINGS_LEFT_STICK,
    /** A radial dead zone of `gamepad_right_dead_zone`, for the right stick. */
    TB_INPUT_SETTINGS_RIGHT_STICK,
    TB_INPUT_SETTINGS_SOURCE_COUNT /**< The number of sources; not a source. */
} tb_input_settings_source;

/** @brief The most ticks a smooth modifier takes the mean of. */
#define TB_INPUT_MOST_SMOOTH_SAMPLES 8u

/**
 * @brief A modifier: one step of the way from a key's value to an action's.
 * Zero-initialize it, then set `type` and the fields of that type; the
 * others are ignored.
 */
typedef struct tb_input_modifier
{
    tb_input_modifier_type type; /**< What it does. */
    /** Dead zone: how it measures. */
    tb_input_dead_zone dead_zone;
    /** Dead zone: below this, a value is zero; 0 for 0.2, Unreal's. From 0
     * to below `upper`. */
    float lower;
    /** Dead zone: at and above this, a value is 1; 0 for 1. At most 1. */
    float upper;
    /** Negate: the axes that change sign, of #tb_input_axis; 0 for all three. */
    uint32_t axes;
    tb_input_swizzle order; /**< Swizzle: where the axes go. */
    tb_input_value scale;   /**< Scalar: what each axis is multiplied by. */
    /** Response curve: the power each axis is raised to; 0 in an axis is 1,
     * a straight line. Not negative. */
    tb_input_value exponent;
    /** Smooth: the ticks whose mean it is, from 1 to
     * #TB_INPUT_MOST_SMOOTH_SAMPLES; 0 for 4. */
    uint32_t samples;
    tb_input_settings_source source; /**< Settings: which. */
} tb_input_modifier;

/* ---------------------------------------------------------------------------
 * Triggers.
 * ------------------------------------------------------------------------- */

/**
 * @brief What a trigger waits for.
 *
 * Most are **explicit**: an action with any fires when one of them fires.
 * Chorded is **implicit**: an action fires only when every implicit
 * trigger does. A chord blocker is a **blocker**: while it fires, the action
 * does not. A value is actuated when its length is at least the trigger's
 * `threshold`.
 */
typedef enum tb_input_trigger_type
{
    TB_INPUT_TRIGGER_DOWN,     /**< Triggered every tick the value is actuated. */
    TB_INPUT_TRIGGER_PRESSED,  /**< Triggered on the tick it becomes actuated, once. */
    TB_INPUT_TRIGGER_RELEASED, /**< Ongoing while actuated; triggered on the tick it is not. */
    /** Ongoing while actuated; triggered once it has been for `seconds`, every
     * tick after, or once with `one_shot`. */
    TB_INPUT_TRIGGER_HOLD,
    /** Ongoing while actuated; triggered when released after `seconds` or
     * more. */
    TB_INPUT_TRIGGER_HOLD_AND_RELEASE,
    /** Ongoing while actuated; triggered when released within `seconds`, and
     * no longer ongoing once held longer. */
    TB_INPUT_TRIGGER_TAP,
    /** Ongoing while actuated; triggered every `seconds` of it, at the start
     * too with `pulse_on_start`, at most `limit` times when that is not 0. */
    TB_INPUT_TRIGGER_PULSE,
    /** Implicit: triggered while the `chord` action is triggered. */
    TB_INPUT_TRIGGER_CHORDED,
    /** Blocker: blocks the action while the `chord` action is triggered. */
    TB_INPUT_TRIGGER_CHORD_BLOCKER,
    TB_INPUT_TRIGGER_TYPE_COUNT /**< The number of types; not a type. */
} tb_input_trigger_type;

/** @brief The actuation threshold of a trigger that gives none, Unreal's. */
#define TB_INPUT_DEFAULT_THRESHOLD 0.5f

/**
 * @brief A trigger. Zero-initialize it, then set `type` and the fields of
 * that type; the others are ignored.
 */
typedef struct tb_input_trigger
{
    tb_input_trigger_type type; /**< What it waits for. */
    /** How long a value must be to be actuated; 0 for
     * #TB_INPUT_DEFAULT_THRESHOLD. */
    float threshold;
    /** Hold and hold-and-release: how long, 0 for 1 second. Tap: the
     * longest a tap is, 0 for 0.2. Pulse: the time between pulses, 0 for 1.
     * Not negative. */
    double seconds;
    bool one_shot;       /**< Hold: trigger once per hold, not every tick after. */
    bool pulse_on_start; /**< Pulse: trigger on the first tick too. */
    uint32_t limit;      /**< Pulse: the most pulses per hold; 0 for no limit. */
    /** Chorded and chord blocker: the other action; required for them. */
    tb_input_action chord;
} tb_input_trigger;

/* ---------------------------------------------------------------------------
 * The input.
 * ------------------------------------------------------------------------- */

/** @brief How an input is made. Zero-initialize it for the defaults. */
typedef struct tb_input_options
{
    /** Players, from 1 to #TB_INPUT_MAX_PLAYERS; 0 for 1. */
    size_t player_count;
} tb_input_options;

/**
 * @brief The player's settings that settings modifiers apply. An input
 * starts with #TB_INPUT_SETTINGS_DEFAULT, the engine's defaults.
 */
typedef struct tb_input_settings
{
    float mouse_sensitivity;              /**< `controls.mouse.sensitivity`. */
    bool mouse_invert_y;                  /**< `controls.mouse.invert_y`. */
    bool mouse_smoothing;                 /**< `controls.mouse.smoothing`. */
    float gamepad_horizontal_sensitivity; /**< `controls.gamepad.horizontal_sensitivity`. */
    float gamepad_vertical_sensitivity;   /**< `controls.gamepad.vertical_sensitivity`. */
    bool gamepad_invert_y;                /**< `controls.gamepad.invert_y`. */
    float gamepad_left_dead_zone;         /**< `controls.gamepad.left_dead_zone`. */
    float gamepad_right_dead_zone;        /**< `controls.gamepad.right_dead_zone`. */
    float touch_sensitivity;              /**< `controls.touch.sensitivity`. */
} tb_input_settings;

/** @brief The engine's default settings. */
#define TB_INPUT_SETTINGS_DEFAULT {1.0f, false, false, 1.0f, 1.0f, false, 0.1f, 0.1f, 1.0f}

/**
 * @brief Make an input, with its players and no actions or contexts. The
 * keyboard and the mouse are assigned to player 0.
 * @param options How; NULL for the defaults.
 * @return The input; never NULL. Destroy it with tb_input_destroy().
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_input *tb_input_create(const tb_input_options *options);

/** @brief Destroy an input, its players, actions, contexts and bindings.
 * A recording it was making is kept, as far as it got.
 * @param input The input; NULL is ignored. */
TB_API void tb_input_destroy(tb_input *input);

/** @brief An input's players. @param input Required. @return From 1. */
TB_API size_t tb_input_player_count(const tb_input *input);

/**
 * @brief One of an input's players.
 * @param input Required.
 * @param index Below tb_input_player_count(); anything else is a contract
 * violation.
 * @return The player, owned by the input, for as long as the input lasts.
 */
TB_API tb_input_player *tb_input_player_at(tb_input *input, size_t index);

/** @brief A player's index in its input. @param player Required. */
TB_API size_t tb_input_player_index(const tb_input_player *player);

/**
 * @brief Give a device's keys to a player, from the next key it feeds. A
 * device belongs to one player at a time.
 * @param input Required.
 * @param device The device; not #TB_INPUT_DEVICE_NONE.
 * @param player Below tb_input_player_count(), or #TB_INPUT_NO_PLAYER for
 * none, so that its keys are ignored.
 */
TB_API void tb_input_assign_device(tb_input *input, tb_input_device device, size_t player);

/** @brief The player a device belongs to. @param input Required.
 * @param device The device. @return Its player's index, or
 * #TB_INPUT_NO_PLAYER. */
TB_API size_t tb_input_device_player(const tb_input *input, tb_input_device device);

/**
 * @brief Set the player's settings that settings modifiers apply, from the
 * next tick, and stop following the engine's settings.
 * @param input Required. @param settings Required; copied.
 */
TB_API void tb_input_set_settings(tb_input *input, const tb_input_settings *settings);

/**
 * @brief Read the player's settings from the engine's `controls.*`
 * settings at the start of every tick, so that a change in an options menu
 * applies at the next tick.
 * @param input Required.
 * @param engine The engine's settings, borrowed until the input is
 * destroyed or this is called again; NULL to stop, keeping the values last
 * read.
 */
TB_API void tb_input_follow_settings(tb_input *input, const struct tb_engine_settings *engine);

/** @brief The settings the last tick used, or that the next will start
 * from. @param input Required. @param[out] settings Required. */
TB_API void tb_input_get_settings(const tb_input *input, tb_input_settings *settings);

/* ---------------------------------------------------------------------------
 * Actions and contexts.
 * ------------------------------------------------------------------------- */

/** @brief How an action combines the values of several keys at once. */
typedef enum tb_input_accumulation
{
    /** In each axis, the value farthest from zero, as Unreal's default: W and
     * the up arrow together are 1, not 2. */
    TB_INPUT_ACCUMULATE_HIGHEST,
    /** The sum: W and S together cancel. */
    TB_INPUT_ACCUMULATE_CUMULATIVE,
    TB_INPUT_ACCUMULATION_COUNT /**< The number of ways; not a way. */
} tb_input_accumulation;

/** @brief An action. Zero-initialize it, then set its name and type. */
typedef struct tb_input_action_desc
{
    /** Its name, unique in its input: lowercase letters, digits and `_`,
     * as `"move"`; required; copied. */
    const char *name;
    tb_input_value_type type;           /**< Its value's type. */
    tb_input_accumulation accumulation; /**< How its keys' values combine. */
    /** Modifiers applied to the combined value, after each mapping's; copied. */
    const tb_input_modifier *modifiers;
    size_t modifier_count; /**< Of `modifiers`. */
    /** Triggers of the action itself, on the combined value, after each
     * mapping's: the action fires when both do; copied. */
    const tb_input_trigger *triggers;
    size_t trigger_count; /**< Of `triggers`. */
} tb_input_action_desc;

/**
 * @brief Make an action.
 * @param input Required.
 * @param desc Required, with a valid name that no action of the input has,
 * and valid modifiers and triggers: a broken one is a contract violation,
 * which tb_input_check_action() reports first.
 * @return The action, for as long as the input lasts.
 */
TB_API tb_input_action tb_input_create_action(tb_input *input, const tb_input_action_desc *desc);

/**
 * @brief Why an action could not be made, or NULL when it can.
 * @param input Required. @param desc Required.
 * @return A short phrase, static: "a name an action already has".
 */
TB_API const char *tb_input_check_action(const tb_input *input, const tb_input_action_desc *desc);

/** @brief An action by name. @param input Required. @param name Required.
 * @return The action, or #TB_INPUT_ACTION_NONE. */
TB_API tb_input_action tb_input_find_action(const tb_input *input, const char *name);

/** @brief An input's actions. @param input Required. */
TB_API size_t tb_input_action_count(const tb_input *input);

/** @brief An action by its place, in the order made. @param input Required.
 * @param index Below tb_input_action_count(). */
TB_API tb_input_action tb_input_action_at(const tb_input *input, size_t index);

/** @brief An action's name. @param input Required. @param action One of
 * the input's. @return Its name, for as long as the input lasts. */
TB_API const char *tb_input_action_name(const tb_input *input, tb_input_action action);

/** @brief An action's type. @param input Required. @param action One of the input's. */
TB_API tb_input_value_type tb_input_action_type(const tb_input *input, tb_input_action action);

/**
 * @brief Make a mapping context, with no mappings.
 * @param input Required.
 * @param name Unique in the input, of lowercase letters, digits and `_`;
 * required; copied. A broken or taken name is a contract violation.
 * @return The context, for as long as the input lasts.
 */
TB_API tb_input_context tb_input_create_context(tb_input *input, const char *name);

/** @brief A context by name. @param input Required. @param name Required.
 * @return The context, or #TB_INPUT_CONTEXT_NONE. */
TB_API tb_input_context tb_input_find_context(const tb_input *input, const char *name);

/** @brief An input's contexts. @param input Required. */
TB_API size_t tb_input_context_count(const tb_input *input);

/** @brief A context by its place, in the order made. @param input Required.
 * @param index Below tb_input_context_count(). */
TB_API tb_input_context tb_input_context_at(const tb_input *input, size_t index);

/** @brief A context's name. @param input Required. @param context One of
 * the input's. @return Its name, for as long as the input lasts. */
TB_API const char *tb_input_context_name(const tb_input *input, tb_input_context context);

/** @brief A mapping: a key, the action it drives, and how. Zero-initialize
 * it, then set the action and the key. */
typedef struct tb_input_mapping_desc
{
    tb_input_action action; /**< The action; required. */
    tb_input_key key;       /**< The key; required. */
    /** Modifiers applied to the key's value, in order; copied. */
    const tb_input_modifier *modifiers;
    size_t modifier_count;            /**< Of `modifiers`. */
    const tb_input_trigger *triggers; /**< Triggers of the mapping; copied. */
    size_t trigger_count;             /**< Of `triggers`. */
    /** Let contexts of lower priority see the key too. By default a mapping
     * consumes its key, as Unreal's actions do. */
    bool pass_through;
} tb_input_mapping_desc;

/**
 * @brief Add a mapping to a context. A context changed while players have
 * it starts over for them: its mappings' triggers and modifiers forget what
 * they held.
 * @param input Required. @param context One of the input's.
 * @param desc Required and valid: a broken one is a contract violation,
 * which tb_input_check_mapping() reports first.
 */
TB_API void tb_input_context_map(tb_input *input, tb_input_context context,
                                 const tb_input_mapping_desc *desc);

/** @brief Why a mapping could not be added, or NULL when it can.
 * @param input Required. @param desc Required. @return A short phrase, static. */
TB_API const char *tb_input_check_mapping(const tb_input *input, const tb_input_mapping_desc *desc);

/** @brief A context's mappings. @param input Required. @param context One of the input's. */
TB_API size_t tb_input_context_mapping_count(const tb_input *input, tb_input_context context);

/** @brief Modifiers and triggers a context adds to every one of its
 * mappings of an action, after the mapping's own. */
typedef struct tb_input_action_rules_desc
{
    tb_input_action action;             /**< The action; required. */
    const tb_input_modifier *modifiers; /**< Copied. */
    size_t modifier_count;              /**< Of `modifiers`. */
    const tb_input_trigger *triggers;   /**< Copied. */
    size_t trigger_count;               /**< Of `triggers`. */
} tb_input_action_rules_desc;

/**
 * @brief Set the modifiers and triggers a context adds to its mappings of
 * an action, in place of any it had: a menu context that makes "select" a
 * tap, while gameplay makes it a hold.
 * @param input Required. @param context One of the input's.
 * @param desc Required and valid: a broken one is a contract violation,
 * which tb_input_check_action_rules() reports first.
 */
TB_API void tb_input_context_set_action_rules(tb_input *input, tb_input_context context,
                                              const tb_input_action_rules_desc *desc);

/** @brief Why action rules could not be set, or NULL when they can.
 * @param input Required. @param desc Required. @return A short phrase, static. */
TB_API const char *tb_input_check_action_rules(const tb_input *input,
                                               const tb_input_action_rules_desc *desc);

/* ---------------------------------------------------------------------------
 * Players and their contexts.
 * ------------------------------------------------------------------------- */

/**
 * @brief Add a context to a player's stack, or change its priority when the
 * player has it. Higher priorities see keys first; of equal priorities, the
 * context added last does. Keys held down when it is added are ignored by
 * its mappings until they are released, so that the press that opened a
 * menu does not act in it.
 * @param player Required. @param context One of the player's input's.
 * @param priority Its priority; any number.
 */
TB_API void tb_input_add_context(tb_input_player *player, tb_input_context context,
                                 int32_t priority);

/**
 * @brief Take a context from a player's stack. Its actions that were
 * ongoing or triggered are canceled or completed at the next tick.
 * @param player Required. @param context Any.
 * @return Whether the player had it.
 */
TB_API bool tb_input_remove_context(tb_input_player *player, tb_input_context context);

/** @brief Take every context from a player's stack. @param player Required. */
TB_API void tb_input_clear_contexts(tb_input_player *player);

/** @brief Whether a player has a context. @param player Required. @param context Any. */
TB_API bool tb_input_has_context(const tb_input_player *player, tb_input_context context);

/* ---------------------------------------------------------------------------
 * Gathering and ticks.
 * ------------------------------------------------------------------------- */

/**
 * @brief Gather a window's event: keys from the keyboard, and buttons,
 * motion and the wheel from the mouse, for the players the keyboard and
 * the mouse are assigned to. A key's repeats are ignored, and so are text,
 * which stays with the window. When the window loses the focus, every key
 * and button held is released.
 * @param input Required. @param event Required.
 * @return Whether the event gave input.
 */
TB_API bool tb_input_feed_window_event(tb_input *input, const tb_window_event *event);

/**
 * @brief Gather a key's value from a device, for the player the device is
 * assigned to: a gamepad's and a touch screen's way in.
 *
 * A button is down while its value's `x` is not 0. The mouse's motion and
 * the wheel add up until the tick. Any other axis has the value fed last.
 * A 2D key sets its X and Y keys too, and they set it.
 * @param input Required. @param device The device. @param key Any key but
 * #TB_INPUT_KEY_NONE. @param value Finite.
 */
TB_API void tb_input_feed_key(tb_input *input, tb_input_device device, tb_input_key key,
                              tb_input_value value);

/**
 * @brief Evaluate every player's actions with the input gathered since the
 * last tick, call the callbacks bound to their events, and start gathering
 * for the next.
 * @param input Required. @param seconds The tick's length; finite and not
 * negative.
 */
TB_API void tb_input_tick(tb_input *input, double seconds);

/** @brief The ticks an input has evaluated. @param input Required. */
TB_API uint64_t tb_input_tick_count(const tb_input *input);

/** @brief The sum of the lengths of an input's ticks, in seconds. @param input Required. */
TB_API double tb_input_seconds(const tb_input *input);

/**
 * @brief A key's value as the last tick saw it, for a player.
 * @param player Required. @param key Any key but #TB_INPUT_KEY_NONE.
 */
TB_API tb_input_value tb_input_key_value(const tb_input_player *player, tb_input_key key);

/* ---------------------------------------------------------------------------
 * Actions' states and events.
 * ------------------------------------------------------------------------- */

/** @brief Where an action stands, as its triggers decided at the last tick. */
typedef enum tb_input_trigger_state
{
    TB_INPUT_STATE_NONE,     /**< Not under way. */
    TB_INPUT_STATE_ONGOING,  /**< Under way, not yet fired: a hold not yet long enough. */
    TB_INPUT_STATE_TRIGGERED /**< Fired. */
} tb_input_trigger_state;

/** @brief What happened to an action in a tick, as bits: the move from its
 * last state to its new one. */
typedef enum tb_input_event
{
    TB_INPUT_EVENT_STARTED = 1u << 0,   /**< From none to ongoing or triggered. */
    TB_INPUT_EVENT_ONGOING = 1u << 1,   /**< Ongoing, after ongoing or triggered. */
    TB_INPUT_EVENT_TRIGGERED = 1u << 2, /**< Triggered, from any state. */
    TB_INPUT_EVENT_COMPLETED = 1u << 3, /**< From triggered to none. */
    TB_INPUT_EVENT_CANCELED = 1u << 4   /**< From ongoing to none, never triggered. */
} tb_input_event;

/** @brief Every event's bit. */
#define TB_INPUT_EVENT_ALL 0x1fu

/** @brief An action as the last tick left it, for one player. */
typedef struct tb_input_action_status
{
    /** Its value: zero in state none. A bool's is 1 or 0. */
    tb_input_value value;
    tb_input_trigger_state state; /**< Its state. */
    uint32_t events;              /**< The #tb_input_event bits of the last tick. */
    /** How long it has been ongoing or triggered, without a tick of none, in
     * seconds of ticks. */
    double elapsed_seconds;
    /** How long it has been triggered without a break. */
    double triggered_seconds;
    /** When it last triggered, in tb_input_seconds() at the end of that
     * tick; negative when it never has. */
    double last_triggered_at;
} tb_input_action_status;

/** @brief An action as the last tick left it, for a player.
 * @param player Required. @param action One of the player's input's.
 * @param[out] status Required. */
TB_API void tb_input_get_action_status(const tb_input_player *player, tb_input_action action,
                                       tb_input_action_status *status);

/** @brief An action's value. @param player Required.
 * @param action One of the player's input's. */
TB_API tb_input_value tb_input_action_value(const tb_input_player *player, tb_input_action action);

/** @brief Whether an action's value is not zero: a bool action's value.
 * @param player Required. @param action One of the player's input's. */
TB_API bool tb_input_action_active(const tb_input_player *player, tb_input_action action);

/** @brief Whether an action triggered in the last tick.
 * @param player Required. @param action One of the player's input's. */
TB_API bool tb_input_action_triggered(const tb_input_player *player, tb_input_action action);

/** @brief An action's #tb_input_event bits of the last tick.
 * @param player Required. @param action One of the player's input's. */
TB_API uint32_t tb_input_action_events(const tb_input_player *player, tb_input_action action);

/** @brief An action's state. @param player Required.
 * @param action One of the player's input's. */
TB_API tb_input_trigger_state tb_input_action_state(const tb_input_player *player,
                                                    tb_input_action action);

/** @brief An event of an action, for a callback. */
typedef struct tb_input_action_event
{
    tb_input_player *player; /**< The player. */
    tb_input_action action;  /**< The action. */
    tb_input_event event;    /**< One event. */
    /** The action as the tick left it. */
    tb_input_action_status status;
} tb_input_action_event;

/** @brief A callback of an action's events. @param user As bound.
 * @param event What happened; valid for the call. */
typedef void tb_input_callback(void *user, const tb_input_action_event *event);

/** @brief A binding of a callback: a handle. Zero is none. */
typedef struct tb_input_binding
{
    uint32_t id; /**< Its number in its player, from 1; 0 for none. */
} tb_input_binding;

/**
 * @brief Call a function on an action's events, from inside tb_input_tick(),
 * after every action of the player has been evaluated. Bindings are called
 * in the order bound, once for each of their events in a tick, in the
 * order started, ongoing, triggered, completed, canceled. A callback may
 * bind and unbind, and add and remove contexts; a binding it makes is
 * called from the next tick.
 * @param player Required. @param action One of the player's input's.
 * @param events The #tb_input_event bits to call it on; not 0.
 * @param callback Required. @param user Given to it.
 * @return The binding, for tb_input_unbind().
 */
TB_API tb_input_binding tb_input_bind(tb_input_player *player, tb_input_action action,
                                      uint32_t events, tb_input_callback *callback, void *user);

/** @brief Stop calling a binding; from inside a callback, at once.
 * @param player Required. @param binding One of the player's, or none;
 * one unbound already is ignored. */
TB_API void tb_input_unbind(tb_input_player *player, tb_input_binding binding);

/* ---------------------------------------------------------------------------
 * Recording and replaying.
 * ------------------------------------------------------------------------- */

/** @brief Recorded ticks: what each gathered, its length, and the settings
 * it used. Created by tb_input_recording_create(); owned by the caller;
 * destroyed by tb_input_recording_destroy(). */
typedef struct tb_input_recording tb_input_recording;

/** @brief Make an empty recording. @return Never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process. */
TB_API tb_input_recording *tb_input_recording_create(void);

/** @brief Destroy a recording. @param recording NULL is ignored. It must not
 * be one an input is recording into. */
TB_API void tb_input_recording_destroy(tb_input_recording *recording);

/** @brief The ticks a recording holds. @param recording Required. */
TB_API size_t tb_input_recording_tick_count(const tb_input_recording *recording);

/**
 * @brief Record every tick from now on at the end of a recording, or stop.
 * A tick records the keys gathered for it, by player, after devices were
 * assigned, its length, and the settings it used.
 * @param input Required.
 * @param recording Borrowed until recording stops: the input is destroyed,
 * or this is called again; NULL to stop.
 */
TB_API void tb_input_record(tb_input *input, tb_input_recording *recording);

/**
 * @brief Replay one recorded tick: gather what it gathered, with its
 * settings, and tick for its length. An input with the same actions and
 * contexts, in the state the recorded one was in when the tick began, gives
 * the same actions, events and callbacks. Input gathered since the last
 * tick is evaluated with it.
 * @param input Required; not recording.
 * @param recording Required.
 * @param tick Below tb_input_recording_tick_count().
 */
TB_API void tb_input_replay_tick(tb_input *input, const tb_input_recording *recording, size_t tick);

/* ---------------------------------------------------------------------------
 * Keys.
 * ------------------------------------------------------------------------- */

/** @brief A key's name, as files write it: `"w"`, `"left_shift"`,
 * `"mouse_x"`, `"gamepad_face_south"`. @param key Any. @return Static; ""
 * for none and for a value that is not a key. */
TB_API const char *tb_input_key_name(tb_input_key key);

/** @brief A key by its name. @param name The name; NULL only at length 0.
 * @param length Bytes of it. @return The key, or #TB_INPUT_KEY_NONE. */
TB_API tb_input_key tb_input_find_key(const char *name, size_t length);

/** @brief The type of a key's value: bool for buttons, axis1d for one
 * axis, axis2d for a stick, the mouse's motion and a touch's position.
 * @param key Any key but #TB_INPUT_KEY_NONE. */
TB_API tb_input_value_type tb_input_key_type(tb_input_key key);

/** @brief The key of a window's key. @param key Any.
 * @return The key, or #TB_INPUT_KEY_NONE for #TB_KEY_UNKNOWN. */
TB_API tb_input_key tb_input_key_from_window(tb_key key);

/** @brief The key of a mouse button. @param button Any.
 * @return The key, or #TB_INPUT_KEY_NONE for no button. */
TB_API tb_input_key tb_input_key_from_mouse_button(tb_mouse_button button);

/* ---------------------------------------------------------------------------
 * Files.
 * ------------------------------------------------------------------------- */

/** @brief What loading or saving a file came to. */
typedef enum tb_input_file_result
{
    TB_INPUT_FILE_OK,           /**< Done. */
    TB_INPUT_FILE_NOT_FOUND,    /**< The VFS has no such file. */
    TB_INPUT_FILE_INPUT_FAILED, /**< The VFS could not read it. */
    TB_INPUT_FILE_MALFORMED,    /**< The text is not JSON. */
    /** The text is JSON but not definitions the input can take: another
     * format, a newer version, a key the format does not have, a name taken
     * or unknown, a value of the wrong type or out of range. */
    TB_INPUT_FILE_INVALID,
    TB_INPUT_FILE_OUTPUT_FAILED /**< The stream refused what was written. */
} tb_input_file_result;

/** @brief Bytes of the path, and of the message, of #tb_input_file_error. */
#define TB_INPUT_FILE_ERROR_CAPACITY 256

/** @brief Why a file was refused, and where. */
typedef struct tb_input_file_error
{
    tb_input_file_result result; /**< As the function returned. */
    /** The JSON path of the value at fault, as
     * `$.contexts[0].mappings[2].key`; "" when the fault is not in a value.
     * Cut short when it is too long. */
    char path[TB_INPUT_FILE_ERROR_CAPACITY];
    size_t line;        /**< In the text, from 1; 0 when the fault is not in the text. */
    size_t column;      /**< Byte in the line, from 1; 0 as `line`. */
    const char *reason; /**< A short phrase: "an unknown key"; static; "" for none. */
    /** All of it, for a log: "input.json:12:16: $.contexts[0].mappings[2].key:
     * an unknown key \"spcae\"". */
    char message[TB_INPUT_FILE_ERROR_CAPACITY];
} tb_input_file_error;

/** @brief The format an input file names: `"format": "badger-input"`. */
#define TB_INPUT_FILE_FORMAT "badger-input"

/** @brief The version of the format this engine writes, and the newest it reads. */
#define TB_INPUT_FILE_VERSION 1

/**
 * @brief Make the actions and contexts of a file's text (see
 * `docs/input.md`), all or nothing. A mapping may name an action the input
 * had before; an action or context whose name the input has is refused.
 * @param input Required.
 * @param text Strict JSON; NULL only at length 0.
 * @param length Bytes of text.
 * @param name The file's name, for messages; NULL for none.
 * @param[out] error Why it failed; NULL when nobody asks.
 * @return #TB_INPUT_FILE_OK, #TB_INPUT_FILE_MALFORMED or
 * #TB_INPUT_FILE_INVALID; on failure the input is as it was.
 */
TB_API tb_input_file_result tb_input_load(tb_input *input, const char *text, size_t length,
                                          const char *name, tb_input_file_error *error);

/**
 * @brief Make the actions and contexts of a file read through a VFS, as
 * tb_input_load().
 * @param input Required. @param vfs Required. @param path Required.
 * @param[out] error Why it failed; NULL when nobody asks.
 * @return As tb_input_load(), or #TB_INPUT_FILE_NOT_FOUND or
 * #TB_INPUT_FILE_INPUT_FAILED.
 */
TB_API tb_input_file_result tb_input_load_file(tb_input *input, struct tb_vfs *vfs,
                                               const char *path, tb_input_file_error *error);

/**
 * @brief Write every action and context of an input as a file that
 * tb_input_load() reads back to the same. The same definitions write the
 * same bytes.
 * @param input Required.
 * @param sink Required open stream that can write; borrowed.
 * @param[out] error Why it failed; NULL when nobody asks.
 * @return #TB_INPUT_FILE_OK or #TB_INPUT_FILE_OUTPUT_FAILED.
 */
TB_API tb_input_file_result tb_input_save(const tb_input *input, struct tb_stream *sink,
                                          tb_input_file_error *error);

#endif
