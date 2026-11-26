/* One backend over several, as Windows reads XInput and raw HID side by
 * side: each child's pads take the lowest free slots of the system, keep
 * them while they stay, and give them up when they go; rumble reaches the
 * child that has the pad; closing closes every child. */
#include "platform/shared/gamepad_mux.h"

#include "tb_test.h"

#include <string.h>

/* A child: pads the test sets, in the child's own slots. */
typedef struct fake_child
{
    tb_gamepad_pad pads[TB_GAMEPAD_SLOT_COUNT];
    size_t polls;
    size_t rumbles;
    size_t rumble_slot;
    float low;
    size_t players;     /* Pads shown which player they are. */
    size_t player_slot; /* The child's slot of the last. */
    size_t player;
    bool closed;
} fake_child;

static uint64_t fake_now = 0;

static uint64_t fake_clock(void)
{
    return fake_now;
}

static void fake_close(void *native)
{
    fake_child *const child = native;

    child->closed = true;
}

static void fake_poll(void *native, uint64_t now, tb_gamepad_pad *pads)
{
    fake_child *const child = native;

    (void)now;
    child->polls++;
    memcpy(pads, child->pads, sizeof(child->pads));
}

static void fake_rumble(void *native, size_t slot, float low_frequency, float high_frequency,
                        double seconds)
{
    fake_child *const child = native;

    (void)high_frequency;
    (void)seconds;
    child->rumbles++;
    child->rumble_slot = slot;
    child->low = low_frequency;
}

static void fake_player(void *native, size_t slot, size_t player)
{
    fake_child *const child = native;

    child->players++;
    child->player_slot = slot;
    child->player = player;
}

static const tb_gamepad_ops fake_ops = {
    .close = fake_close, .poll = fake_poll, .rumble = fake_rumble};
/* A child whose pads have lights. */
static const tb_gamepad_ops fake_lit_ops = {
    .close = fake_close, .poll = fake_poll, .rumble = fake_rumble, .player = fake_player};

static void plug(fake_child *child, size_t slot, uint64_t serial, uint16_t product)
{
    child->pads[slot] = (tb_gamepad_pad){.serial = serial};
    child->pads[slot].info.product = product;
    child->pads[slot].info.capabilities = TB_GAMEPAD_CAPABILITY_RUMBLE;
}

static uint16_t product_at(const tb_gamepad_system *system, size_t slot)
{
    tb_gamepad_info info = {0};

    tb_gamepad_get_info(system, slot, &info);
    return info.product;
}

static bool connected_at(const tb_gamepad_system *system, size_t slot)
{
    tb_gamepad_state state = {0};

    tb_gamepad_get_state(system, slot, &state);
    return state.connected;
}

/* Pads of two children share the system's slots, the lowest free first,
 * whichever child has them: an XInput pad in its user 2 is in slot 0 when
 * it is the only pad. */
TB_TEST(platform_gamepad_mux, pads_take_the_lowest_free_slots)
{
    fake_child first = {0};
    fake_child second = {0};
    const tb_gamepad_mux_child children[2] = {{&fake_ops, &first}, {&fake_ops, &second}};
    tb_gamepad_system *const system =
        tb_gamepad_system_make(&tb_gamepad_mux_ops, tb_gamepad_mux_make(children, 2), fake_clock);
    tb_gamepad_event event = {0};

    plug(&first, 2, 1, 0x0a);
    plug(&second, 0, 1, 0x0b);
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_UINT(first.polls, ==, 1u);
    TB_TEST_EXPECT_UINT(second.polls, ==, 1u);
    TB_TEST_EXPECT_HEX(product_at(system, 0), ==, 0x0a);
    TB_TEST_EXPECT_HEX(product_at(system, 1), ==, 0x0b);
    TB_TEST_EXPECT(connected_at(system, 0) && connected_at(system, 1));
    TB_TEST_EXPECT(!connected_at(system, 2));
    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    TB_TEST_EXPECT_UINT(event.slot, ==, 0u);
    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    TB_TEST_EXPECT_UINT(event.slot, ==, 1u);

    /* The first child's pad goes: its slot empties, and the other pad keeps
     * its own. A pad that comes next takes the freed slot. */
    first.pads[2] = (tb_gamepad_pad){0};
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(!connected_at(system, 0));
    TB_TEST_EXPECT_HEX(product_at(system, 1), ==, 0x0b);
    plug(&second, 5, 2, 0x0c);
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_HEX(product_at(system, 0), ==, 0x0c);

    /* A state the child changes is the slot's at the next poll. */
    second.pads[5].state.buttons = 1u << TB_GAMEPAD_BUTTON_SOUTH;
    tb_gamepad_system_poll(system);
    {
        tb_gamepad_state state = {0};
        tb_gamepad_get_state(system, 0, &state);
        TB_TEST_EXPECT(tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_SOUTH));
    }
    tb_gamepad_system_close(system);
    TB_TEST_EXPECT(first.closed);
    TB_TEST_EXPECT(second.closed);
}

/* A pad that comes is shown the system's slot as its player, through the
 * child that has it, in the child's own slot; a child without lights is
 * never asked. */
TB_TEST(platform_gamepad_mux, the_player_reaches_the_child)
{
    fake_child unlit = {0};
    fake_child lit = {0};
    const tb_gamepad_mux_child children[2] = {{&fake_ops, &unlit}, {&fake_lit_ops, &lit}};
    tb_gamepad_system *const system =
        tb_gamepad_system_make(&tb_gamepad_mux_ops, tb_gamepad_mux_make(children, 2), fake_clock);

    plug(&unlit, 0, 1, 0x0a);
    plug(&lit, 3, 1, 0x0b);
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_UINT(unlit.players, ==, 0u);
    TB_TEST_REQUIRE_UINT(lit.players, ==, 1u);
    TB_TEST_EXPECT_UINT(lit.player_slot, ==, 3u);
    TB_TEST_EXPECT_UINT(lit.player, ==, 1u);
    /* Only when it comes. */
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_UINT(lit.players, ==, 1u);
    tb_gamepad_system_close(system);
}

/* A child that replaces a pad in its slot between two polls, as a pad
 * unplugged and another plugged in, makes two events: the slot's pad went
 * and another came. */
TB_TEST(platform_gamepad_mux, a_replaced_pad_is_a_new_connection)
{
    fake_child child = {0};
    const tb_gamepad_mux_child children[1] = {{&fake_ops, &child}};
    tb_gamepad_system *const system =
        tb_gamepad_system_make(&tb_gamepad_mux_ops, tb_gamepad_mux_make(children, 1), fake_clock);
    tb_gamepad_event event = {0};

    plug(&child, 0, 1, 0x0a);
    tb_gamepad_system_poll(system);
    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    plug(&child, 0, 2, 0x0b);
    tb_gamepad_system_poll(system);
    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    TB_TEST_EXPECT_INT(event.type, ==, TB_GAMEPAD_EVENT_DISCONNECTED);
    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    TB_TEST_EXPECT_INT(event.type, ==, TB_GAMEPAD_EVENT_CONNECTED);
    TB_TEST_EXPECT_UINT(event.slot, ==, 0u);
    TB_TEST_EXPECT_HEX(product_at(system, 0), ==, 0x0b);
    tb_gamepad_system_close(system);
}

/* With every slot taken, a pad waits for one to free. */
TB_TEST(platform_gamepad_mux, a_pad_waits_for_a_free_slot)
{
    fake_child first = {0};
    fake_child second = {0};
    const tb_gamepad_mux_child children[2] = {{&fake_ops, &first}, {&fake_ops, &second}};
    tb_gamepad_system *const system =
        tb_gamepad_system_make(&tb_gamepad_mux_ops, tb_gamepad_mux_make(children, 2), fake_clock);

    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        plug(&first, slot, slot + 1, 0x0a);
    }
    plug(&second, 0, 1, 0x0b);
    tb_gamepad_system_poll(system);
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        TB_TEST_EXPECT_HEX(product_at(system, slot), ==, 0x0a);
    }
    first.pads[3] = (tb_gamepad_pad){0};
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_HEX(product_at(system, 3), ==, 0x0b);
    tb_gamepad_system_close(system);
}

/* Rumble goes to the child that has the slot's pad, in the child's own
 * slot. */
TB_TEST(platform_gamepad_mux, rumble_reaches_the_child)
{
    fake_child first = {0};
    fake_child second = {0};
    const tb_gamepad_mux_child children[2] = {{&fake_ops, &first}, {&fake_ops, &second}};
    tb_gamepad_system *const system =
        tb_gamepad_system_make(&tb_gamepad_mux_ops, tb_gamepad_mux_make(children, 2), fake_clock);

    plug(&first, 3, 1, 0x0a);
    plug(&second, 6, 1, 0x0b);
    tb_gamepad_system_poll(system);
    TB_TEST_REQUIRE(tb_gamepad_rumble(system, 1, &(tb_gamepad_rumble_desc){0.5f, 0.5f, 1.0}));
    TB_TEST_EXPECT_UINT(second.rumbles, ==, 1u);
    TB_TEST_EXPECT_UINT(second.rumble_slot, ==, 6u);
    TB_TEST_EXPECT_FLOAT(second.low, ==, 0.5f);
    TB_TEST_EXPECT_UINT(first.rumbles, ==, 0u);
    TB_TEST_REQUIRE(tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){0.25f, 0.0f, 1.0}));
    TB_TEST_EXPECT_UINT(first.rumble_slot, ==, 3u);
    tb_gamepad_system_close(system);
    /* Closing stopped both. */
    TB_TEST_EXPECT_FLOAT(first.low, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(second.low, ==, 0.0f);
}
