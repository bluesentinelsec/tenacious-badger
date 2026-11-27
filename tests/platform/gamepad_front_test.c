/* The shared front of the gamepad interface, over a backend of the test's
 * own: events as pads come and go, rumble scaled by the vibration and
 * stopped when its time is up, when its pad goes, when the focus goes, and
 * when the system closes; and the helpers that scale a pad's own values. */
#include "platform/shared/gamepad.h"

#include "badger/str.h"

#include "tb_test.h"

#include <string.h>

#define FAKE_NANOSECONDS_PER_SECOND 1000000000u

/* The backend: pads the test sets, and the rumble the front sent. */
typedef struct fake_backend
{
    tb_gamepad_pad pads[TB_GAMEPAD_SLOT_COUNT];
    size_t polls;
    size_t rumbles;
    size_t last_slot;
    float low;
    float high;
    double seconds;
    bool closed;
} fake_backend;

/* The test's clock; one test at a time uses it. */
static uint64_t fake_now = 0;

static uint64_t fake_clock(void)
{
    return fake_now;
}

static void fake_close(void *native)
{
    fake_backend *const backend = native;

    backend->closed = true;
}

static void fake_poll(void *native, uint64_t now, tb_gamepad_pad *pads)
{
    fake_backend *const backend = native;

    (void)now;
    backend->polls++;
    memcpy(pads, backend->pads, sizeof(backend->pads));
}

static void fake_rumble(void *native, size_t slot, float low_frequency, float high_frequency,
                        double seconds)
{
    fake_backend *const backend = native;

    backend->rumbles++;
    backend->last_slot = slot;
    backend->low = low_frequency;
    backend->high = high_frequency;
    backend->seconds = seconds;
}

static const tb_gamepad_ops fake_ops = {
    .close = fake_close, .poll = fake_poll, .rumble = fake_rumble};

static void plug(fake_backend *backend, size_t slot, uint64_t serial, bool rumble)
{
    backend->pads[slot] = (tb_gamepad_pad){.serial = serial};
    backend->pads[slot].info.capabilities = rumble ? TB_GAMEPAD_CAPABILITY_RUMBLE : 0u;
    tb_gamepad_copy_name(backend->pads[slot].info.name, "Pad", 3);
}

static void expect_event(tb_gamepad_system *system, tb_gamepad_event_type type, size_t slot)
{
    tb_gamepad_event event = {0};

    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    TB_TEST_EXPECT_INT(event.type, ==, type);
    TB_TEST_EXPECT_UINT(event.slot, ==, slot);
}

/* A pad that comes is an event and connected; one that goes is an event
 * and all zero; one replaced between two polls, by its serial, is both. */
TB_TEST(platform_gamepad_front, pads_that_come_and_go_are_events)
{
    fake_backend backend = {0};
    tb_gamepad_system *const system = tb_gamepad_system_make(&fake_ops, &backend, fake_clock);
    tb_gamepad_event event = {0};
    tb_gamepad_state state = {0};
    tb_gamepad_info info = {0};

    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(!tb_gamepad_system_next_event(system, &event));

    plug(&backend, 2, 7, false);
    backend.pads[2].state.buttons = 1u << TB_GAMEPAD_BUTTON_START;
    tb_gamepad_system_poll(system);
    expect_event(system, TB_GAMEPAD_EVENT_CONNECTED, 2);
    TB_TEST_EXPECT(!tb_gamepad_system_next_event(system, &event));
    tb_gamepad_get_state(system, 2, &state);
    tb_gamepad_get_info(system, 2, &info);
    TB_TEST_EXPECT(state.connected);
    TB_TEST_EXPECT(tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_START));
    TB_TEST_EXPECT_STRING(info.name, ==, "Pad");

    /* A pad gone and another come between two polls. */
    plug(&backend, 2, 8, false);
    tb_gamepad_system_poll(system);
    expect_event(system, TB_GAMEPAD_EVENT_DISCONNECTED, 2);
    expect_event(system, TB_GAMEPAD_EVENT_CONNECTED, 2);

    /* What a backend leaves of an empty slot is not reported. */
    backend.pads[2] = (tb_gamepad_pad){.state.buttons = 1u};
    tb_gamepad_system_poll(system);
    expect_event(system, TB_GAMEPAD_EVENT_DISCONNECTED, 2);
    tb_gamepad_get_state(system, 2, &state);
    tb_gamepad_get_info(system, 2, &info);
    TB_TEST_EXPECT(!state.connected);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 0);
    TB_TEST_EXPECT_STRING(info.name, ==, "");

    tb_gamepad_system_close(system);
    TB_TEST_EXPECT(backend.closed);
}

/* The motors are clamped, scaled by the vibration, and stopped when their
 * time is up; a new vibration rescales what runs for what time is left. */
TB_TEST(platform_gamepad_front, rumble_is_scaled_and_timed)
{
    fake_backend backend = {0};
    tb_gamepad_system *system = NULL;

    fake_now = 5u * FAKE_NANOSECONDS_PER_SECOND;
    system = tb_gamepad_system_make(&fake_ops, &backend, fake_clock);
    plug(&backend, 1, 1, true);
    tb_gamepad_system_poll(system);
    tb_gamepad_system_set_vibration(system, 0.5f);
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 0);

    TB_TEST_EXPECT(tb_gamepad_rumble(system, 1, &(tb_gamepad_rumble_desc){2.0f, 0.5f, 0.25}));
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 1);
    TB_TEST_EXPECT_UINT(backend.last_slot, ==, 1);
    TB_TEST_EXPECT_FLOAT_NEAR(backend.low, 0.5f, 1e-6f);
    TB_TEST_EXPECT_FLOAT_NEAR(backend.high, 0.25f, 1e-6f);
    TB_TEST_EXPECT_FLOAT_NEAR((float)backend.seconds, 0.25f, 1e-6f);

    fake_now += FAKE_NANOSECONDS_PER_SECOND / 10u;
    tb_gamepad_system_set_vibration(system, 2.0f);
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 2);
    TB_TEST_EXPECT_FLOAT_NEAR(backend.low, 1.0f, 1e-6f);
    TB_TEST_EXPECT_FLOAT_NEAR((float)backend.seconds, 0.15f, 1e-6f);

    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 2);
    fake_now += FAKE_NANOSECONDS_PER_SECOND / 5u;
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 3);
    TB_TEST_EXPECT_FLOAT(backend.low, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(backend.high, ==, 0.0f);
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 3);

    /* No vibration stops what runs. */
    TB_TEST_EXPECT(tb_gamepad_rumble(system, 1, &(tb_gamepad_rumble_desc){1.0f, 1.0f, 10.0}));
    tb_gamepad_system_set_vibration(system, 0.0f);
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 5);
    TB_TEST_EXPECT_FLOAT(backend.low, ==, 0.0f);
    TB_TEST_EXPECT(!tb_gamepad_rumble(system, 1, &(tb_gamepad_rumble_desc){1.0f, 1.0f, 10.0}));

    tb_gamepad_system_close(system);
}

/* A rumble of no time or no strength stops one that runs, and starts none. */
TB_TEST(platform_gamepad_front, a_rumble_of_nothing_stops)
{
    fake_backend backend = {0};
    tb_gamepad_system *const system = tb_gamepad_system_make(&fake_ops, &backend, fake_clock);

    plug(&backend, 0, 1, true);
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){0.3f, 0.0f, 1.0}));
    TB_TEST_EXPECT(!tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){0.3f, 0.3f, 0.0}));
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 2);
    TB_TEST_EXPECT_FLOAT(backend.low, ==, 0.0f);
    TB_TEST_EXPECT(!tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){-1.0f, 0.0f, 1.0}));
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 3);
    /* An hour at most. */
    TB_TEST_EXPECT(tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){1.0f, 0.0f, 1e9}));
    TB_TEST_EXPECT_FLOAT_NEAR((float)backend.seconds, 3600.0f, 1e-3f);
    tb_gamepad_system_close(system);
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 5);
    TB_TEST_EXPECT_FLOAT(backend.low, ==, 0.0f);
}

/* An empty slot and a pad without motors do not rumble, and nothing is
 * sent to them. */
TB_TEST(platform_gamepad_front, only_pads_with_motors_rumble)
{
    fake_backend backend = {0};
    tb_gamepad_system *const system = tb_gamepad_system_make(&fake_ops, &backend, fake_clock);
    const tb_gamepad_rumble_desc rumble = {1.0f, 1.0f, 1.0};

    plug(&backend, 3, 1, false);
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(!tb_gamepad_rumble(system, 0, &rumble));
    TB_TEST_EXPECT(!tb_gamepad_rumble(system, 3, &rumble));
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 0);
    tb_gamepad_system_close(system);
}

/* A pad that goes takes its rumble with it: nothing is sent to its slot,
 * and the pad that comes next does not rumble. */
TB_TEST(platform_gamepad_front, rumble_stops_when_its_pad_goes)
{
    fake_backend backend = {0};
    tb_gamepad_system *const system = tb_gamepad_system_make(&fake_ops, &backend, fake_clock);

    plug(&backend, 0, 1, true);
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){1.0f, 1.0f, 10.0}));
    backend.pads[0] = (tb_gamepad_pad){0};
    tb_gamepad_system_poll(system);
    plug(&backend, 0, 2, true);
    tb_gamepad_system_poll(system);
    fake_now += 20u * FAKE_NANOSECONDS_PER_SECOND;
    tb_gamepad_system_poll(system);
    tb_gamepad_system_close(system);
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 1);
}

/* Without the focus every pad stops and none starts; the focus back
 * starts nothing by itself. */
TB_TEST(platform_gamepad_front, rumble_stops_without_the_focus)
{
    fake_backend backend = {0};
    tb_gamepad_system *const system = tb_gamepad_system_make(&fake_ops, &backend, fake_clock);
    const tb_gamepad_rumble_desc rumble = {1.0f, 1.0f, 10.0};

    plug(&backend, 0, 1, true);
    plug(&backend, 1, 2, true);
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(tb_gamepad_rumble(system, 0, &rumble));
    TB_TEST_EXPECT(tb_gamepad_rumble(system, 1, &rumble));
    tb_gamepad_system_set_focus(system, false);
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 4);
    TB_TEST_EXPECT_FLOAT(backend.low, ==, 0.0f);
    TB_TEST_EXPECT(!tb_gamepad_rumble(system, 0, &rumble));
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 4);
    tb_gamepad_system_set_focus(system, true);
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 4);
    TB_TEST_EXPECT(tb_gamepad_rumble(system, 0, &rumble));
    tb_gamepad_system_close(system);
    TB_TEST_EXPECT_UINT(backend.rumbles, ==, 6);
}

/* Errors are written only when asked for. */
TB_TEST(platform_gamepad_front, failures_fill_in_an_error)
{
    tb_gamepad_error error = TB_GAMEPAD_ERROR_INIT;

    TB_TEST_EXPECT_INT(tb_gamepad_fail(NULL, TB_GAMEPAD_UNAVAILABLE, "x"), ==,
                       TB_GAMEPAD_UNAVAILABLE);
    TB_TEST_EXPECT_INT(tb_gamepad_fail(&error, TB_GAMEPAD_SYSTEM_ERROR, "no %s", "pads"), ==,
                       TB_GAMEPAD_SYSTEM_ERROR);
    TB_TEST_EXPECT_INT(error.result, ==, TB_GAMEPAD_SYSTEM_ERROR);
    TB_TEST_EXPECT_STRING(error.message, ==, "no pads");
}

/* A stick's range scales each half to its own end, so XInput's -32768 to
 * 32767 reaches -1 and 1 and rests at 0; evdev's 0 to 255 rests at 128. */
TB_TEST(platform_gamepad_front, stick_values_scale_to_minus_one_to_one)
{
    TB_TEST_EXPECT_FLOAT(tb_gamepad_stick_value(0, INT16_MIN, INT16_MAX, false), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_stick_value(INT16_MIN, INT16_MIN, INT16_MAX, false), ==, -1.0f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_stick_value(INT16_MAX, INT16_MIN, INT16_MAX, false), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_stick_value(128, 0, 255, false), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_stick_value(0, 0, 255, false), ==, -1.0f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_stick_value(255, 0, 255, false), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_stick_value(255, 0, 255, true), ==, -1.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_gamepad_stick_value(64, 0, 255, false), -0.5f, 1e-6f);
    /* Out of the range is clamped; a range of one value, or none, is 0. */
    TB_TEST_EXPECT_FLOAT(tb_gamepad_stick_value(400, 0, 255, false), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_stick_value(5, 5, 5, false), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_stick_value(5, 9, 1, false), ==, 0.0f);
    /* A range of two values. */
    TB_TEST_EXPECT_FLOAT(tb_gamepad_stick_value(1, 0, 1, false), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_stick_value(0, 0, 1, false), ==, -1.0f);
}

TB_TEST(platform_gamepad_front, trigger_values_scale_to_zero_to_one)
{
    TB_TEST_EXPECT_FLOAT(tb_gamepad_trigger_value(0, 0, 255), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_trigger_value(255, 0, 255), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_gamepad_trigger_value(512, 0, 1023), 0.5f, 1e-3f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_trigger_value(-4, 0, 255), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_trigger_value(900, 0, 255), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_gamepad_trigger_value(3, 3, 3), ==, 0.0f);
}

/* A name too long is cut at a character, and bytes that are not UTF-8 are
 * question marks. */
TB_TEST(platform_gamepad_front, names_are_cut_at_a_character_and_made_utf8)
{
    char name[TB_GAMEPAD_NAME_CAPACITY] = {0};
    char long_name[TB_GAMEPAD_NAME_CAPACITY + 8] = {0};

    tb_gamepad_copy_name(name, "Pad \xff\xfe!", 7);
    TB_TEST_EXPECT_STRING(name, ==,
                          "Pad ?"
                          "?!");
    tb_gamepad_copy_name(name, "Caf\xc3\xa9", 5);
    TB_TEST_EXPECT_STRING(name, ==, "Caf\xc3\xa9");
    tb_gamepad_copy_name(name, "Pad\0junk", 8);
    TB_TEST_EXPECT_STRING(name, ==, "Pad");

    /* An e with an accent across the last byte that fits is left out. */
    memset(long_name, 'a', TB_GAMEPAD_NAME_CAPACITY - 2);
    long_name[TB_GAMEPAD_NAME_CAPACITY - 2] = '\xc3';
    long_name[TB_GAMEPAD_NAME_CAPACITY - 1] = '\xa9';
    tb_gamepad_copy_name(name, long_name, TB_GAMEPAD_NAME_CAPACITY);
    TB_TEST_EXPECT_UINT(tb_str_length(name), ==, TB_GAMEPAD_NAME_CAPACITY - 2);
}

/* Steam's list names pads by vendor and product, in hex, between commas. */
TB_TEST(platform_gamepad_front, steam_names_the_pads_to_leave_alone)
{
    const char *const list = "0x28DE/0x1205, 0x045e/0x0b12,junk,0x1/,/0x2,054c/0ce6";

    TB_TEST_EXPECT(tb_gamepad_ignored(list, 0x28de, 0x1205));
    TB_TEST_EXPECT(tb_gamepad_ignored(list, 0x045e, 0x0b12));
    TB_TEST_EXPECT(tb_gamepad_ignored(list, 0x054c, 0x0ce6));
    TB_TEST_EXPECT(!tb_gamepad_ignored(list, 0x28de, 0x11ff));
    TB_TEST_EXPECT(!tb_gamepad_ignored(list, 0x0001, 0x0000));
    TB_TEST_EXPECT(!tb_gamepad_ignored(NULL, 0x28de, 0x1205));
    TB_TEST_EXPECT(!tb_gamepad_ignored("", 0x28de, 0x1205));
}

/* A pad's family, for prompts, is the backend's when it knows, else its USB
 * IDs'; on a Steam Deck, the pad Steam stands in for is the Deck's own
 * controls, and a pad of its own IDs keeps its family. */
TB_TEST(platform_gamepad_front, a_pads_family_comes_from_its_backend_or_its_ids)
{
    fake_backend backend = {0};
    tb_gamepad_system *const system = tb_gamepad_system_make(&fake_ops, &backend, fake_clock);
    tb_gamepad_info info = {0};

    plug(&backend, 0, 1, false);
    backend.pads[0].info.vendor = 0x054c;
    backend.pads[0].info.product = 0x0ce6;
    plug(&backend, 1, 2, false);
    backend.pads[1].info.family = TB_GAMEPAD_FAMILY_XBOX;
    plug(&backend, 2, 3, false);
    backend.pads[2].info.vendor = 0x28de;
    backend.pads[2].info.product = 0x11ff;
    tb_gamepad_system_poll(system);
    tb_gamepad_get_info(system, 0, &info);
    TB_TEST_EXPECT_INT(info.family, ==, TB_GAMEPAD_FAMILY_PLAYSTATION);
    tb_gamepad_get_info(system, 1, &info);
    TB_TEST_EXPECT_INT(info.family, ==, TB_GAMEPAD_FAMILY_XBOX);
    tb_gamepad_get_info(system, 2, &info);
    TB_TEST_EXPECT_INT(info.family, ==, TB_GAMEPAD_FAMILY_GENERIC);
    tb_gamepad_get_info(system, 3, &info);
    TB_TEST_EXPECT_INT(info.family, ==, TB_GAMEPAD_FAMILY_GENERIC);

    tb_gamepad_system_set_steam_deck(system, true);
    tb_gamepad_get_info(system, 0, &info);
    TB_TEST_EXPECT_INT(info.family, ==, TB_GAMEPAD_FAMILY_PLAYSTATION);
    tb_gamepad_get_info(system, 1, &info);
    TB_TEST_EXPECT_INT(info.family, ==, TB_GAMEPAD_FAMILY_STEAM_DECK);
    tb_gamepad_get_info(system, 2, &info);
    TB_TEST_EXPECT_INT(info.family, ==, TB_GAMEPAD_FAMILY_STEAM_DECK);
    /* An empty slot is all zero, on a Deck too. */
    tb_gamepad_get_info(system, 3, &info);
    TB_TEST_EXPECT_INT(info.family, ==, TB_GAMEPAD_FAMILY_GENERIC);
    tb_gamepad_system_close(system);
}

/* The GameController framework's categories, and names, give families by
 * their words, whatever their case; Steam sets SteamDeck to 1 on a Deck. */
TB_TEST(platform_gamepad_front, families_from_names_and_the_steam_deck_variable)
{
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_name("Xbox One"), ==, TB_GAMEPAD_FAMILY_XBOX);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_name("DualSense"), ==, TB_GAMEPAD_FAMILY_PLAYSTATION);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_name("DualShock 4"), ==,
                       TB_GAMEPAD_FAMILY_PLAYSTATION);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_name("Switch Pro Controller"), ==,
                       TB_GAMEPAD_FAMILY_NINTENDO);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_name("Nintendo Switch Joy-Con (L/R)"), ==,
                       TB_GAMEPAD_FAMILY_NINTENDO);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_name("STEAM DECK Controller"), ==,
                       TB_GAMEPAD_FAMILY_STEAM_DECK);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_name("PlayStation Format"), ==,
                       TB_GAMEPAD_FAMILY_PLAYSTATION);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_name("Generic USB Joystick"), ==,
                       TB_GAMEPAD_FAMILY_GENERIC);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_name(NULL), ==, TB_GAMEPAD_FAMILY_GENERIC);
    TB_TEST_EXPECT(tb_gamepad_steam_deck_variable("1"));
    TB_TEST_EXPECT(!tb_gamepad_steam_deck_variable("0"));
    TB_TEST_EXPECT(!tb_gamepad_steam_deck_variable(NULL));
}
