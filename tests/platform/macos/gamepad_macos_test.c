/* What the macOS backend reads of a GameController controller, which CI
 * has none of: a controller that +[GCController controllerWithExtendedGamepad]
 * makes, whose elements the test sets as a player would press them, put in
 * a slot as one the framework lists would be. */
#include "platform/macos/gamepad_macos.h"

#include "badger/library.h"

#include "tb_test.h"

#include <stdint.h>

typedef void *id;
typedef void *sel;

typedef struct objc
{
    tb_library *runtime;
    tb_library *framework;
    id (*get_class)(const char *name);
    sel (*register_name)(const char *name);
    void (*send)(void);
} objc;

static objc load_objc(void)
{
    objc loaded = {0};
    tb_library_function functions[3] = {0};
    const tb_library_binding bindings[3] = {
        {.name = "objc_getClass", .function = &functions[0], .required = true},
        {.name = "sel_registerName", .function = &functions[1], .required = true},
        {.name = "objc_msgSend", .function = &functions[2], .required = true},
    };

    TB_TEST_REQUIRE_INT(
        tb_library_open("libobjc.A.dylib", TB_LIBRARY_SEARCH_SYSTEM, &loaded.runtime, NULL), ==,
        TB_LIBRARY_OK);
    TB_TEST_REQUIRE_INT(tb_library_bind(loaded.runtime, bindings, 3, NULL), ==, TB_LIBRARY_OK);
    TB_TEST_REQUIRE_INT(tb_library_open("GameController.framework/GameController",
                                        TB_LIBRARY_SEARCH_SYSTEM, &loaded.framework, NULL),
                        ==, TB_LIBRARY_OK);
    loaded.get_class = (id (*)(const char *))functions[0];
    loaded.register_name = (sel (*)(const char *))functions[1];
    loaded.send = functions[2];
    return loaded;
}

static id message(const objc *runtime, id receiver, const char *selector)
{
    return ((id (*)(id, sel))runtime->send)(receiver, runtime->register_name(selector));
}

static void message_float(const objc *runtime, id receiver, const char *selector, float value)
{
    ((void (*)(id, sel, float))runtime->send)(receiver, runtime->register_name(selector), value);
}

static void message_floats(const objc *runtime, id receiver, const char *selector, float x, float y)
{
    ((void (*)(id, sel, float, float))runtime->send)(receiver, runtime->register_name(selector), x,
                                                     y);
}

static void press(const objc *runtime, id gamepad, const char *button)
{
    message_float(runtime, message(runtime, gamepad, button), "setValue:", 1.0f);
}

static uint64_t zero_clock(void)
{
    return 0;
}

TB_TEST(platform_macos_gamepad, an_extended_gamepad_reads_in_the_standard_layout)
{
    const objc runtime = load_objc();
    const tb_gamepad_ops *ops = NULL;
    void *native = NULL;
    tb_gamepad_system *system = NULL;
    id controller = NULL;
    id gamepad = NULL;
    tb_gamepad_state state = {0};
    tb_gamepad_info info = {0};
    tb_gamepad_event event = {0};

    TB_TEST_REQUIRE_INT(tb_native_gamepad_open(&ops, &native, NULL), ==, TB_GAMEPAD_OK);
    system = tb_gamepad_system_make(ops, native, zero_clock);
    controller =
        message(&runtime, runtime.get_class("GCController"), "controllerWithExtendedGamepad");
    TB_TEST_REQUIRE(controller != NULL);
    gamepad = message(&runtime, controller, "extendedGamepad");
    TB_TEST_REQUIRE(gamepad != NULL);
    TB_TEST_REQUIRE_UINT(tb_macos_gamepads_attach(native, controller), ==, 0);

    press(&runtime, gamepad, "buttonA");
    press(&runtime, gamepad, "buttonY");
    press(&runtime, gamepad, "leftShoulder");
    message_float(&runtime, message(&runtime, gamepad, "rightTrigger"), "setValue:", 0.75f);
    /* A simulated d-pad is set as a direction pad, whose up follows its Y. */
    message_floats(&runtime, message(&runtime, gamepad, "dpad"), "setValueForXAxis:yAxis:", 0.0f,
                   1.0f);
    message_floats(&runtime, message(&runtime, gamepad, "leftThumbstick"),
                   "setValueForXAxis:yAxis:", -1.0f, 0.5f);

    tb_gamepad_system_poll(system);
    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    TB_TEST_EXPECT_INT(event.type, ==, TB_GAMEPAD_EVENT_CONNECTED);
    tb_gamepad_get_state(system, 0, &state);
    tb_gamepad_get_info(system, 0, &info);
    TB_TEST_EXPECT(state.connected);
    TB_TEST_EXPECT(tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_SOUTH));
    TB_TEST_EXPECT(tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_NORTH));
    TB_TEST_EXPECT(tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_LEFT_SHOULDER));
    TB_TEST_EXPECT(tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_DPAD_UP));
    TB_TEST_EXPECT(!tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_EAST));
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], 0.75f, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_LEFT_X], -1.0f, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_LEFT_Y], 0.5f, 1e-3f);
    TB_TEST_EXPECT(info.name[0] != '\0');
    /* Rumble is an unavailable capability here, for now. */
    TB_TEST_EXPECT_HEX(info.capabilities, ==, 0);

    /* Every slot taken, another waits. */
    for (size_t slot = 1; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        TB_TEST_EXPECT_UINT(tb_macos_gamepads_attach(native, controller), ==, slot);
    }
    TB_TEST_EXPECT_UINT(tb_macos_gamepads_attach(native, controller), ==, TB_GAMEPAD_SLOT_COUNT);
    tb_gamepad_system_close(system);
    tb_library_close(runtime.framework);
    tb_library_close(runtime.runtime);
}
