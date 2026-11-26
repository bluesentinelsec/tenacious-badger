/* macOS gamepads: the GameController framework, reached from C through the
 * Objective-C runtime, with libobjc and GameController loaded at run time.
 *
 * Each poll lists +[GCController controllers] and compares it with the
 * controllers the slots hold, retained: one that is new takes the lowest
 * free slot, and one that is gone empties its slot. That is hot-plugging
 * without observers of GCControllerDidConnectNotification, which the
 * framework posts on the same run loop that updates the list, so the two
 * see the same thing. Only controllers with an extended gamepad profile,
 * the standard layout, are read; its elements are already in the standard
 * ranges, sticks up positive. Apple names the face buttons by place.
 *
 * The home and share buttons are left to the system: the engine never reads
 * them and never changes their preferredSystemGestureState, so their
 * gestures (Launchpad, screenshots) keep working.
 *
 * Rumble needs Core Haptics through GCController's haptics, which the
 * engine does not drive yet, so no pad here reports
 * #TB_GAMEPAD_CAPABILITY_RUMBLE. */
#include "gamepad_macos.h"

#include "badger/assert.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <stdint.h>
#include <string.h>

typedef void *tb_gc_id;
typedef void *tb_gc_sel;

typedef struct tb_gc_runtime
{
    tb_library *objc;
    tb_library *framework;
    tb_gc_id (*get_class)(const char *name);
    tb_gc_sel (*register_name)(const char *name);
    void (*send)(void);
    void *(*pool_push)(void);
    void (*pool_pop)(void *pool);
} tb_gc_runtime;

typedef struct tb_gc_slot
{
    tb_gc_id controller; /* Retained; NULL for an empty slot. */
    bool simulated;      /* A test's, which the framework does not list. */
    bool seen;           /* Listed by this poll. */
    tb_gamepad_pad pad;
} tb_gc_slot;

typedef struct tb_gc_gamepads
{
    tb_gc_runtime runtime;
    tb_gc_id controller_class;
    uint64_t serial;
    tb_gc_slot slots[TB_GAMEPAD_SLOT_COUNT];
} tb_gc_gamepads;

/* ---------------------------------------------------------------------------
 * Messages.
 * ------------------------------------------------------------------------- */

static tb_gc_id tb_gc_send(const tb_gc_runtime *runtime, tb_gc_id receiver, const char *selector)
{
    return ((tb_gc_id (*)(tb_gc_id, tb_gc_sel))runtime->send)(receiver,
                                                              runtime->register_name(selector));
}

static tb_gc_id tb_gc_send_index(const tb_gc_runtime *runtime, tb_gc_id receiver,
                                 const char *selector, unsigned long index)
{
    return ((tb_gc_id (*)(tb_gc_id, tb_gc_sel, unsigned long))runtime->send)(
        receiver, runtime->register_name(selector), index);
}

static unsigned long tb_gc_send_count(const tb_gc_runtime *runtime, tb_gc_id receiver,
                                      const char *selector)
{
    return ((unsigned long (*)(tb_gc_id, tb_gc_sel))runtime->send)(
        receiver, runtime->register_name(selector));
}

static bool tb_gc_responds(const tb_gc_runtime *runtime, tb_gc_id receiver, const char *selector)
{
    return ((signed char (*)(tb_gc_id, tb_gc_sel, tb_gc_sel))runtime->send)(
               receiver, runtime->register_name("respondsToSelector:"),
               runtime->register_name(selector)) != 0;
}

/* A button's state; a nil button, which a pad without it gives, is up. */
static bool tb_gc_pressed(const tb_gc_runtime *runtime, tb_gc_id button)
{
    if (button == NULL)
    {
        return false;
    }
    return ((signed char (*)(tb_gc_id, tb_gc_sel))runtime->send)(
               button, runtime->register_name("isPressed")) != 0;
}

static float tb_gc_value(const tb_gc_runtime *runtime, tb_gc_id element)
{
    if (element == NULL)
    {
        return 0.0f;
    }
    return ((float (*)(tb_gc_id, tb_gc_sel))runtime->send)(element,
                                                           runtime->register_name("value"));
}

/* ---------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

typedef struct tb_gc_button_row
{
    const char *selector;
    tb_gamepad_button button;
} tb_gc_button_row;

/* The buttons of GCExtendedGamepad. */
static const tb_gc_button_row tb_gc_buttons[] = {
    {"buttonA", TB_GAMEPAD_BUTTON_SOUTH},
    {"buttonB", TB_GAMEPAD_BUTTON_EAST},
    {"buttonX", TB_GAMEPAD_BUTTON_WEST},
    {"buttonY", TB_GAMEPAD_BUTTON_NORTH},
    {"leftShoulder", TB_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"rightShoulder", TB_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"leftThumbstickButton", TB_GAMEPAD_BUTTON_LEFT_STICK},
    {"rightThumbstickButton", TB_GAMEPAD_BUTTON_RIGHT_STICK},
    {"buttonMenu", TB_GAMEPAD_BUTTON_START},
    {"buttonOptions", TB_GAMEPAD_BUTTON_BACK},
};

/* The d-pad's buttons, of its GCControllerDirectionPad. */
static const tb_gc_button_row tb_gc_dpad[] = {
    {"up", TB_GAMEPAD_BUTTON_DPAD_UP},
    {"down", TB_GAMEPAD_BUTTON_DPAD_DOWN},
    {"left", TB_GAMEPAD_BUTTON_DPAD_LEFT},
    {"right", TB_GAMEPAD_BUTTON_DPAD_RIGHT},
};

/* The Xbox Elite pad's paddles, of GCXboxGamepad: P1 upper right, P2 lower
 * right, P3 upper left, P4 lower left. */
static const tb_gc_button_row tb_gc_paddles[] = {
    {"paddleButton1", TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1},
    {"paddleButton2", TB_GAMEPAD_BUTTON_RIGHT_PADDLE_2},
    {"paddleButton3", TB_GAMEPAD_BUTTON_LEFT_PADDLE_1},
    {"paddleButton4", TB_GAMEPAD_BUTTON_LEFT_PADDLE_2},
};

static uint32_t tb_gc_read_buttons(const tb_gc_runtime *runtime, tb_gc_id owner,
                                   const tb_gc_button_row *rows, size_t count)
{
    uint32_t buttons = 0;

    for (size_t index = 0; index < count; index++)
    {
        if (tb_gc_pressed(runtime, tb_gc_send(runtime, owner, rows[index].selector)))
        {
            buttons |= 1u << (unsigned)rows[index].button;
        }
    }
    return buttons;
}

static void tb_gc_read_stick(const tb_gc_runtime *runtime, tb_gc_id gamepad, const char *selector,
                             float *x, float *y)
{
    const tb_gc_id stick = tb_gc_send(runtime, gamepad, selector);

    *x = tb_gc_value(runtime, tb_gc_send(runtime, stick, "xAxis"));
    *y = tb_gc_value(runtime, tb_gc_send(runtime, stick, "yAxis"));
}

static void tb_gc_read(const tb_gc_runtime *runtime, tb_gc_id gamepad, tb_gamepad_state *state)
{
    float *const axes = state->axes;

    *state = (tb_gamepad_state){.connected = true};
    state->buttons = tb_gc_read_buttons(runtime, gamepad, tb_gc_buttons,
                                        sizeof(tb_gc_buttons) / sizeof(tb_gc_buttons[0]));
    state->buttons |= tb_gc_read_buttons(runtime, tb_gc_send(runtime, gamepad, "dpad"), tb_gc_dpad,
                                         sizeof(tb_gc_dpad) / sizeof(tb_gc_dpad[0]));
    if (tb_gc_responds(runtime, gamepad, "paddleButton1"))
    {
        state->buttons |= tb_gc_read_buttons(runtime, gamepad, tb_gc_paddles,
                                             sizeof(tb_gc_paddles) / sizeof(tb_gc_paddles[0]));
    }
    tb_gc_read_stick(runtime, gamepad, "leftThumbstick", &axes[TB_GAMEPAD_AXIS_LEFT_X],
                     &axes[TB_GAMEPAD_AXIS_LEFT_Y]);
    tb_gc_read_stick(runtime, gamepad, "rightThumbstick", &axes[TB_GAMEPAD_AXIS_RIGHT_X],
                     &axes[TB_GAMEPAD_AXIS_RIGHT_Y]);
    axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER] =
        tb_gc_value(runtime, tb_gc_send(runtime, gamepad, "leftTrigger"));
    axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER] =
        tb_gc_value(runtime, tb_gc_send(runtime, gamepad, "rightTrigger"));
}

/* A controller's name: its vendor's name for it, as "Xbox Wireless
 * Controller", else its category, as "DualSense". */
static void tb_gc_name(const tb_gc_runtime *runtime, tb_gc_id controller, tb_gamepad_info *info)
{
    tb_gc_id text = tb_gc_send(runtime, controller, "vendorName");
    const char *name = NULL;

    if (text == NULL)
    {
        text = tb_gc_send(runtime, controller, "productCategory");
    }
    if (text != NULL)
    {
        name = (const char *)tb_gc_send(runtime, text, "UTF8String");
    }
    if (name == NULL)
    {
        name = "Game controller";
    }
    tb_gamepad_copy_name(info->name, name, tb_str_length(name));
}

static size_t tb_gc_take_slot(tb_gc_gamepads *gamepads, tb_gc_id controller, bool simulated)
{
    const tb_gc_runtime *const runtime = &gamepads->runtime;

    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        tb_gc_slot *const held = &gamepads->slots[slot];
        if (held->controller != NULL)
        {
            continue;
        }
        gamepads->serial++;
        *held = (tb_gc_slot){.controller = tb_gc_send(runtime, controller, "retain"),
                             .simulated = simulated,
                             .seen = true};
        held->pad.serial = gamepads->serial;
        tb_gc_name(runtime, controller, &held->pad.info);
        return slot;
    }
    return TB_GAMEPAD_SLOT_COUNT;
}

static void tb_gc_release_slot(tb_gc_gamepads *gamepads, tb_gc_slot *held)
{
    (void)tb_gc_send(&gamepads->runtime, held->controller, "release");
    *held = (tb_gc_slot){0};
}

/* Mark a listed controller as seen, or give it a slot. */
static void tb_gc_take(tb_gc_gamepads *gamepads, tb_gc_id controller)
{
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        if (gamepads->slots[slot].controller == controller)
        {
            gamepads->slots[slot].seen = true;
            return;
        }
    }
    (void)tb_gc_take_slot(gamepads, controller, false);
}

static void tb_gc_list(tb_gc_gamepads *gamepads)
{
    const tb_gc_runtime *const runtime = &gamepads->runtime;
    const tb_gc_id controllers = tb_gc_send(runtime, gamepads->controller_class, "controllers");
    const unsigned long count = tb_gc_send_count(runtime, controllers, "count");

    for (unsigned long index = 0; index < count; index++)
    {
        const tb_gc_id controller = tb_gc_send_index(runtime, controllers, "objectAtIndex:", index);
        if (tb_gc_send(runtime, controller, "extendedGamepad") != NULL)
        {
            tb_gc_take(gamepads, controller);
        }
    }
}

static void tb_gc_poll(void *native, uint64_t now, tb_gamepad_pad *pads)
{
    tb_gc_gamepads *const gamepads = native;
    const tb_gc_runtime *const runtime = &gamepads->runtime;
    void *const pool = runtime->pool_push();

    (void)now;
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        gamepads->slots[slot].seen = gamepads->slots[slot].simulated;
    }
    tb_gc_list(gamepads);
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        tb_gc_slot *const held = &gamepads->slots[slot];
        if (held->controller != NULL && !held->seen)
        {
            tb_gc_release_slot(gamepads, held);
        }
        if (held->controller != NULL)
        {
            tb_gc_read(runtime, tb_gc_send(runtime, held->controller, "extendedGamepad"),
                       &held->pad.state);
        }
        pads[slot] = held->pad;
    }
    runtime->pool_pop(pool);
}

static void tb_gc_rumble(void *native, size_t slot, float low_frequency, float high_frequency,
                         double seconds)
{
    /* coverage-off: no pad here reports rumble, so the front never asks */
    (void)native;
    (void)slot;
    (void)low_frequency;
    (void)high_frequency;
    (void)seconds;
    /* coverage-on */
}

static void tb_gc_close(void *native)
{
    tb_gc_gamepads *const gamepads = native;

    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        if (gamepads->slots[slot].controller != NULL)
        {
            tb_gc_release_slot(gamepads, &gamepads->slots[slot]);
        }
    }
    tb_library_close(gamepads->runtime.framework);
    tb_library_close(gamepads->runtime.objc);
    tb_free(gamepads);
}

static const tb_gamepad_ops tb_gc_ops = {
    .close = tb_gc_close,
    .poll = tb_gc_poll,
    .rumble = tb_gc_rumble,
};

size_t tb_macos_gamepads_attach(void *native, void *controller)
{
    TB_ASSERT(native != NULL && controller != NULL);
    return tb_gc_take_slot(native, controller, true);
}

static bool tb_gc_load(tb_gc_runtime *runtime, tb_library_error *error)
{
    tb_library_function functions[5] = {0};
    const tb_library_binding bindings[5] = {
        {.name = "objc_getClass", .function = &functions[0], .required = true},
        {.name = "sel_registerName", .function = &functions[1], .required = true},
        {.name = "objc_msgSend", .function = &functions[2], .required = true},
        {.name = "objc_autoreleasePoolPush", .function = &functions[3], .required = true},
        {.name = "objc_autoreleasePoolPop", .function = &functions[4], .required = true},
    };

    if (tb_library_open("libobjc.A.dylib", TB_LIBRARY_SEARCH_SYSTEM, &runtime->objc, error) !=
            TB_LIBRARY_OK ||
        tb_library_bind(runtime->objc, bindings, 5, error) != TB_LIBRARY_OK ||
        tb_library_open("GameController.framework/GameController", TB_LIBRARY_SEARCH_SYSTEM,
                        &runtime->framework, error) != TB_LIBRARY_OK)
    {
        /* coverage-off: every macOS the engine supports has both */
        return false;
        /* coverage-on */
    }
    runtime->get_class = (tb_gc_id (*)(const char *))functions[0];
    runtime->register_name = (tb_gc_sel (*)(const char *))functions[1];
    runtime->send = functions[2];
    runtime->pool_push = (void *(*)(void))functions[3];
    runtime->pool_pop = (void (*)(void *))functions[4];
    return true;
}

tb_gamepad_result tb_native_gamepad_open(const tb_gamepad_ops **ops, void **native,
                                         tb_gamepad_error *error)
{
    tb_gc_gamepads *const gamepads = tb_calloc(1, sizeof(*gamepads));
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;

    if (!tb_gc_load(&gamepads->runtime, &library_error))
    {
        /* coverage-off: every macOS the engine supports has both */
        tb_library_close(gamepads->runtime.framework);
        tb_library_close(gamepads->runtime.objc);
        tb_free(gamepads);
        return tb_gamepad_fail(error, TB_GAMEPAD_UNAVAILABLE, "no GameController: %s",
                               library_error.message);
        /* coverage-on */
    }
    gamepads->controller_class = gamepads->runtime.get_class("GCController");
    *ops = &tb_gc_ops;
    *native = gamepads;
    return TB_GAMEPAD_OK;
}
