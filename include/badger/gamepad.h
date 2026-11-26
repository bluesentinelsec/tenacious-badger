/** @file gamepad.h
 * @brief Gamepads: the controllers a player holds, read in one standard
 * layout, with rumble where the platform has it.
 *
 * A gamepad system reads every gamepad the platform reports: XInput and
 * raw HID pads, as the Switch 2 Pro Controller and the DualSense, on Windows; evdev on Linux
 * and the Steam Deck; and the GameController framework on macOS. Each connected pad has a **slot**,
 * from 0, which it keeps until it is unplugged; a pad plugged in later takes the lowest free slot.
 * A pad's buttons and axes are named by their place on an Xbox-style pad, whatever their labels:
 * the bottom face button is south on an Xbox pad (A), a PlayStation pad (Cross) and a Nintendo pad
 * (B) alike.
 *
 * @code
 * tb_gamepad_system *gamepads = NULL;
 * tb_gamepad_error error = TB_GAMEPAD_ERROR_INIT;
 *
 * if (tb_gamepad_system_open(&gamepads, &error) != TB_GAMEPAD_OK)
 * {
 *     report(error.message); // and play on without gamepads
 * }
 * // Once a frame, on the main thread:
 * tb_gamepad_system_poll(gamepads);
 * tb_gamepad_event event = {0};
 * while (tb_gamepad_system_next_event(gamepads, &event)) { ... }
 * tb_gamepad_state state = {0};
 * tb_gamepad_get_state(gamepads, 0, &state);
 * if (tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_SOUTH)) { ... }
 * @endcode
 *
 * **Values.** A button is down or up. A stick's X and Y are from -1 to 1,
 * right and up positive; a trigger is from 0 to 1. Values are the pad's own,
 * scaled to those ranges and nothing more: dead zones are not applied here.
 * The input module applies them, from the `controls.gamepad.*` settings, in
 * its settings modifier (see `docs/input.md`), and a game that reads pads
 * directly applies its own.
 *
 * **Sticks and triggers as buttons.** tb_gamepad_analog_button() and
 * tb_gamepad_virtual_buttons() turn a stick's directions and the triggers
 * into buttons, with a threshold to press and a lower one to release, so
 * that a value that hovers at the threshold does not chatter.
 *
 * **Rumble.** tb_gamepad_rumble() runs a pad's low- and high-frequency
 * motors for a time, scaled by the system's vibration, which a game sets
 * from its `controls.gamepad.vibration` setting with
 * tb_gamepad_system_set_vibration(). Rumble stops when its time is up, when
 * the pad is unplugged, when the system loses the focus, and when the
 * system is closed. A pad that cannot rumble says so in its info: a
 * capability, never a fault.
 *
 * **The system's buttons.** The guide button (Xbox, PS, Steam, Nintendo's
 * Home) and the share or capture button belong to the operating system and
 * the launcher: Game Bar, the Steam overlay, screenshots, the system's own
 * gestures. A gamepad
 * system never reads them, never grabs a pad from the system, and never
 * reports them, so they keep working for the player while a game runs.
 *
 * See `docs/gamepad.md`.
 *
 * @par Threading
 * Stage-owned: a gamepad system is opened, polled, read and closed on one
 * thread, the main thread, which on macOS the GameController framework
 * requires. The functions that take no system (tb_gamepad_button_down(),
 * tb_gamepad_analog_button(), tb_gamepad_virtual_buttons(), and the names)
 * are thread-safe.
 */
#ifndef TB_GAMEPAD_H
#define TB_GAMEPAD_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The connection to the platform's gamepads. Opened by
 * tb_gamepad_system_open(); owned by the caller; closed by
 * tb_gamepad_system_close(). */
typedef struct tb_gamepad_system tb_gamepad_system;

/** @brief The slots a system has: the most pads it reports at once. XInput
 * reports at most four. */
#define TB_GAMEPAD_SLOT_COUNT 8u

/** @brief A gamepad's buttons, in the standard layout, by their place. */
typedef enum tb_gamepad_button
{
    TB_GAMEPAD_BUTTON_SOUTH,          /**< The bottom face button: A, Cross. */
    TB_GAMEPAD_BUTTON_EAST,           /**< The right face button: B, Circle. */
    TB_GAMEPAD_BUTTON_WEST,           /**< The left face button: X, Square. */
    TB_GAMEPAD_BUTTON_NORTH,          /**< The top face button: Y, Triangle. */
    TB_GAMEPAD_BUTTON_LEFT_SHOULDER,  /**< The left bumper: LB, L1. */
    TB_GAMEPAD_BUTTON_RIGHT_SHOULDER, /**< The right bumper: RB, R1. */
    TB_GAMEPAD_BUTTON_LEFT_STICK,     /**< The left stick, pressed. */
    TB_GAMEPAD_BUTTON_RIGHT_STICK,    /**< The right stick, pressed. */
    TB_GAMEPAD_BUTTON_DPAD_UP,        /**< The d-pad's up. */
    TB_GAMEPAD_BUTTON_DPAD_DOWN,      /**< The d-pad's down. */
    TB_GAMEPAD_BUTTON_DPAD_LEFT,      /**< The d-pad's left. */
    TB_GAMEPAD_BUTTON_DPAD_RIGHT,     /**< The d-pad's right. */
    TB_GAMEPAD_BUTTON_START,          /**< Start, Menu, Options: the right of the middle two. */
    TB_GAMEPAD_BUTTON_BACK,           /**< Back, View, Select: the left of the middle two. */
    TB_GAMEPAD_BUTTON_LEFT_PADDLE_1,  /**< The upper left back paddle: the Steam Deck's L4. */
    TB_GAMEPAD_BUTTON_LEFT_PADDLE_2,  /**< The lower left back paddle: L5. */
    TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1, /**< The upper right back paddle: R4. */
    TB_GAMEPAD_BUTTON_RIGHT_PADDLE_2, /**< The lower right back paddle: R5. */
    TB_GAMEPAD_BUTTON_COUNT           /**< The number of buttons; not a button. */
} tb_gamepad_button;

/** @brief A gamepad's axes. */
typedef enum tb_gamepad_axis
{
    TB_GAMEPAD_AXIS_LEFT_X,        /**< The left stick, from -1 to 1, right positive. */
    TB_GAMEPAD_AXIS_LEFT_Y,        /**< The left stick, from -1 to 1, up positive. */
    TB_GAMEPAD_AXIS_RIGHT_X,       /**< The right stick, right positive. */
    TB_GAMEPAD_AXIS_RIGHT_Y,       /**< The right stick, up positive. */
    TB_GAMEPAD_AXIS_LEFT_TRIGGER,  /**< The left trigger, from 0 to 1: LT, L2. */
    TB_GAMEPAD_AXIS_RIGHT_TRIGGER, /**< The right trigger, from 0 to 1: RT, R2. */
    TB_GAMEPAD_AXIS_COUNT          /**< The number of axes; not an axis. */
} tb_gamepad_axis;

/** @brief A gamepad as the last poll read it. A slot with no pad is all
 * zero. */
typedef struct tb_gamepad_state
{
    bool connected;   /**< A pad is in the slot. */
    uint32_t buttons; /**< Bit `1u << b` for each #tb_gamepad_button b that is down. */
    float axes[TB_GAMEPAD_AXIS_COUNT]; /**< By #tb_gamepad_axis, in their ranges. */
} tb_gamepad_state;

/** @brief What a pad can do, as bits of tb_gamepad_info's `capabilities`. */
typedef enum tb_gamepad_capability
{
    /** Its low- and high-frequency motors rumble, through tb_gamepad_rumble(). */
    TB_GAMEPAD_CAPABILITY_RUMBLE = 1u << 0,
    /** Motors in its triggers rumble: the Xbox One pad's impulse triggers.
     * No system reports it yet; see `docs/gamepad.md`. */
    TB_GAMEPAD_CAPABILITY_TRIGGER_RUMBLE = 1u << 1
} tb_gamepad_capability;

/** @brief Bytes of a pad's name, its NUL included. */
#define TB_GAMEPAD_NAME_CAPACITY 128

/** @brief What a connected pad is. */
typedef struct tb_gamepad_info
{
    /** Its name as the system gives it, in UTF-8: "Xbox Wireless Controller",
     * "Steam Deck". Cut at a character when too long; "" for an empty slot. */
    char name[TB_GAMEPAD_NAME_CAPACITY];
    uint16_t vendor;       /**< Its USB vendor ID, where the system says; else 0. */
    uint16_t product;      /**< Its USB product ID, where the system says; else 0. */
    uint32_t capabilities; /**< Its #tb_gamepad_capability bits. */
} tb_gamepad_info;

/** @brief What happened to a slot. */
typedef enum tb_gamepad_event_type
{
    TB_GAMEPAD_EVENT_CONNECTED,   /**< A pad came into the slot. */
    TB_GAMEPAD_EVENT_DISCONNECTED /**< The slot's pad went; its state is all zero. */
} tb_gamepad_event_type;

/** @brief A pad that came or went, in the order the polls saw them. */
typedef struct tb_gamepad_event
{
    tb_gamepad_event_type type; /**< What happened. */
    size_t slot;                /**< Below #TB_GAMEPAD_SLOT_COUNT. */
} tb_gamepad_event;

/** @brief What opening a gamepad system came to. */
typedef enum tb_gamepad_result
{
    TB_GAMEPAD_OK, /**< Done. */
    /** The platform's gamepad library is not there, as XInput on a Windows
     * without it. A capability, not a fault: a game plays on without
     * gamepads. */
    TB_GAMEPAD_UNAVAILABLE,
    TB_GAMEPAD_SYSTEM_ERROR /**< The system refused; the message says how. */
} tb_gamepad_result;

/** @brief Bytes of an error's message, its NUL included. */
#define TB_GAMEPAD_MESSAGE_CAPACITY 256

/** @brief Why a gamepad system could not be opened. */
typedef struct tb_gamepad_error
{
    tb_gamepad_result result; /**< As the function returned. */
    /** What went wrong, in UTF-8: "no XInput: xinput1_4.dll: ...". */
    char message[TB_GAMEPAD_MESSAGE_CAPACITY];
} tb_gamepad_error;

/** @brief An error before a call. */
#define TB_GAMEPAD_ERROR_INIT {TB_GAMEPAD_OK, {0}}

/** @brief How long and how strongly a pad rumbles. */
typedef struct tb_gamepad_rumble_desc
{
    /** The low-frequency motor, the large one on the left: from 0 to 1. */
    float low_frequency;
    /** The high-frequency motor, the small one on the right: from 0 to 1. */
    float high_frequency;
    /** How long, in seconds; finite and not negative. 0 stops the motors;
     * longer than an hour is an hour. */
    double seconds;
} tb_gamepad_rumble_desc;

/* ---------------------------------------------------------------------------
 * The gamepad system.
 * ------------------------------------------------------------------------- */

/**
 * @brief Connect to the platform's gamepads. Pads already plugged in are
 * reported by the first poll, as connected.
 * @param[out] system Receives the gamepad system, or NULL on failure;
 * required.
 * @param[out] error Receives why it failed; NULL when nobody asks.
 * @return #TB_GAMEPAD_OK, #TB_GAMEPAD_UNAVAILABLE or
 * #TB_GAMEPAD_SYSTEM_ERROR.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_gamepad_result tb_gamepad_system_open(tb_gamepad_system **system,
                                                tb_gamepad_error *error);

/** @brief Stop every pad's rumble and close a gamepad system.
 * @param system The system; NULL is ignored. */
TB_API void tb_gamepad_system_close(tb_gamepad_system *system);

/**
 * @brief Read every pad: their buttons and axes, pads plugged in and
 * unplugged since the last poll, as events, and rumble whose time is up,
 * stopped. Call it once a frame.
 * @param system Required non-null.
 */
TB_API void tb_gamepad_system_poll(tb_gamepad_system *system);

/**
 * @brief Take the next event the polls queued.
 * @param system Required non-null.
 * @param[out] event Receives the event; required non-null.
 * @return Whether there was one.
 */
TB_API bool tb_gamepad_system_next_event(tb_gamepad_system *system, tb_gamepad_event *event);

/**
 * @brief Set how strongly pads rumble: what tb_gamepad_rumble() multiplies
 * its motors by. A game sets it from its `controls.gamepad.vibration`
 * setting; rumble running now takes the new scale.
 * @param system Required non-null.
 * @param scale From 0, none, to 1, as asked; a system starts at 1. Values
 * outside are clamped.
 */
TB_API void tb_gamepad_system_set_vibration(tb_gamepad_system *system, float scale);

/**
 * @brief Say whether the game has the focus. Without it every pad's rumble
 * stops, and tb_gamepad_rumble() starts none, so that a pad does not shake
 * while the player is in another program. Buttons and axes are read
 * either way. A system starts with the focus.
 * @param system Required non-null.
 * @param focused Whether the game's window has the focus.
 */
TB_API void tb_gamepad_system_set_focus(tb_gamepad_system *system, bool focused);

/**
 * @brief A slot's pad, as the last poll read it.
 * @param system Required non-null.
 * @param slot Below #TB_GAMEPAD_SLOT_COUNT.
 * @param[out] state Receives the state, all zero for an empty slot;
 * required non-null.
 */
TB_API void tb_gamepad_get_state(const tb_gamepad_system *system, size_t slot,
                                 tb_gamepad_state *state);

/**
 * @brief What a slot's pad is.
 * @param system Required non-null.
 * @param slot Below #TB_GAMEPAD_SLOT_COUNT.
 * @param[out] info Receives what it is, all zero for an empty slot;
 * required non-null.
 */
TB_API void tb_gamepad_get_info(const tb_gamepad_system *system, size_t slot,
                                tb_gamepad_info *info);

/**
 * @brief Rumble a pad's motors for a time, in place of any rumble it had,
 * each motor multiplied by the system's vibration.
 * @param system Required non-null.
 * @param slot Below #TB_GAMEPAD_SLOT_COUNT.
 * @param rumble Required non-null; motors outside 0 to 1 are clamped.
 * @return Whether the pad is rumbling: false for an empty slot, a pad
 * without #TB_GAMEPAD_CAPABILITY_RUMBLE, a system without the focus, and
 * a rumble of no time or no strength, which stops the pad.
 */
TB_API bool tb_gamepad_rumble(tb_gamepad_system *system, size_t slot,
                              const tb_gamepad_rumble_desc *rumble);

/* ---------------------------------------------------------------------------
 * Buttons from values: the same on every platform.
 * ------------------------------------------------------------------------- */

/** @brief Name a result: "unavailable". @param result Any value.
 * @return A string with static storage duration; "unknown result" for no
 * result.
 * @par Threading Thread-safe. */
TB_API const char *tb_gamepad_result_name(tb_gamepad_result result);

/** @brief Name a button: "south", "left_paddle_1". @param button Any value.
 * @return A string with static storage duration; "" for no button.
 * @par Threading Thread-safe. */
TB_API const char *tb_gamepad_button_name(tb_gamepad_button button);

/** @brief Name an axis: "left_x", "right_trigger". @param axis Any value.
 * @return A string with static storage duration; "" for no axis.
 * @par Threading Thread-safe. */
TB_API const char *tb_gamepad_axis_name(tb_gamepad_axis axis);

/** @brief Whether a button of a state is down. @param state Required
 * non-null. @param button Below #TB_GAMEPAD_BUTTON_COUNT.
 * @par Threading Thread-safe. */
TB_API bool tb_gamepad_button_down(const tb_gamepad_state *state, tb_gamepad_button button);

/**
 * @brief Where an analog value becomes a button: down at `press` and above,
 * up again only below `release`, so that a value hovering at `press` does
 * not chatter.
 */
typedef struct tb_gamepad_threshold
{
    float press;   /**< Down at this and above; above 0, at most 1. */
    float release; /**< Up below this; from 0 to `press`. */
} tb_gamepad_threshold;

/**
 * @brief The default threshold of a stick's direction: down at 0.5, up below
 * 0.4. Unreal presses its `Gamepad_LeftStick_Up` and the like at XInput's
 * dead zones (about 0.24 and 0.27) without hysteresis, which presses the
 * side of a stick pushed some 75 degrees from it. At 0.5, Enhanced Input's
 * default actuation, a direction is down when an action on the axis would
 * be actuated, and a stick at full tilt presses the directions within 60
 * degrees of it: both of a diagonal, one of a push along an axis. The 0.1
 * of hysteresis is well above a stick's noise.
 */
#define TB_GAMEPAD_STICK_THRESHOLD_DEFAULT {0.5f, 0.4f}

/**
 * @brief The default threshold of a trigger as a button: down at 0.2, up
 * below 0.1. Unreal's `Gamepad_LeftTrigger` is down past XInput's
 * `XINPUT_GAMEPAD_TRIGGER_THRESHOLD`, 30 of 255 (0.12), without hysteresis.
 * A trigger is a button the player squeezes deliberately, so it presses
 * early, as there; a little later, so that a worn trigger at rest never
 * presses, and with its release below XInput's threshold.
 */
#define TB_GAMEPAD_TRIGGER_THRESHOLD_DEFAULT {0.2f, 0.1f}

/**
 * @brief Turn an analog value into a button, with hysteresis.
 * @param value The value: a trigger's, or one direction of a stick's axis,
 * positive toward the direction (a stick's Y for up, its negated Y for
 * down). Finite.
 * @param was_down The button's last state.
 * @param threshold Required non-null and valid.
 * @return Whether the button is down.
 * @par Threading Thread-safe.
 */
TB_API bool tb_gamepad_analog_button(float value, bool was_down,
                                     const tb_gamepad_threshold *threshold);

/** @brief Buttons a pad's sticks and triggers make, as bits `1u << b` of
 * the result of tb_gamepad_virtual_buttons(). */
typedef enum tb_gamepad_virtual_button
{
    TB_GAMEPAD_VIRTUAL_LEFT_STICK_UP,     /**< The left stick pushed up. */
    TB_GAMEPAD_VIRTUAL_LEFT_STICK_DOWN,   /**< Down. */
    TB_GAMEPAD_VIRTUAL_LEFT_STICK_LEFT,   /**< Left. */
    TB_GAMEPAD_VIRTUAL_LEFT_STICK_RIGHT,  /**< Right. */
    TB_GAMEPAD_VIRTUAL_RIGHT_STICK_UP,    /**< The right stick pushed up. */
    TB_GAMEPAD_VIRTUAL_RIGHT_STICK_DOWN,  /**< Down. */
    TB_GAMEPAD_VIRTUAL_RIGHT_STICK_LEFT,  /**< Left. */
    TB_GAMEPAD_VIRTUAL_RIGHT_STICK_RIGHT, /**< Right. */
    TB_GAMEPAD_VIRTUAL_LEFT_TRIGGER,      /**< The left trigger squeezed. */
    TB_GAMEPAD_VIRTUAL_RIGHT_TRIGGER,     /**< The right trigger squeezed. */
    TB_GAMEPAD_VIRTUAL_COUNT              /**< The number of them; not one. */
} tb_gamepad_virtual_button;

/** @brief The thresholds of the virtual buttons. */
typedef struct tb_gamepad_virtual_thresholds
{
    tb_gamepad_threshold stick;   /**< Of each stick's four directions. */
    tb_gamepad_threshold trigger; /**< Of each trigger. */
} tb_gamepad_virtual_thresholds;

/** @brief The default thresholds: #TB_GAMEPAD_STICK_THRESHOLD_DEFAULT and
 * #TB_GAMEPAD_TRIGGER_THRESHOLD_DEFAULT. */
#define TB_GAMEPAD_VIRTUAL_THRESHOLDS_DEFAULT                                                      \
    {TB_GAMEPAD_STICK_THRESHOLD_DEFAULT, TB_GAMEPAD_TRIGGER_THRESHOLD_DEFAULT}

/**
 * @brief The buttons a pad's sticks and triggers make, each through
 * tb_gamepad_analog_button(). A stick's directions are of each axis alone,
 * as Unreal's: a diagonal presses two.
 * @param state Required non-null; a disconnected one makes none.
 * @param previous The bits this returned for the pad last time; 0 at first.
 * @param thresholds NULL for #TB_GAMEPAD_VIRTUAL_THRESHOLDS_DEFAULT.
 * @return Bit `1u << b` for each #tb_gamepad_virtual_button b that is down.
 * @par Threading Thread-safe.
 */
TB_API uint32_t tb_gamepad_virtual_buttons(const tb_gamepad_state *state, uint32_t previous,
                                           const tb_gamepad_virtual_thresholds *thresholds);

#endif
