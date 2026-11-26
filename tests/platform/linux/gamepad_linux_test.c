/* What the Linux backend does with event devices, which CI has none of: the
 * system calls on a device node are replaced with devices of the test's
 * own, in a directory of the test's own that inotify watches as it watches
 * /dev/input. Gamepads are told from other devices, read, resynchronized
 * after a drop, rumbled, and seen to come and go. */
#define _GNU_SOURCE
#include "platform/linux/gamepad_evdev.h"
#include "platform/linux/gamepad_linux.h"

#include "badger/file.h"
#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/str.h"

#include "tb_test.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAKE_DEVICES 12u
#define FAKE_QUEUE 160u
#define FAKE_WRITES 16u
#define FAKE_FIRST_DESCRIPTOR 100

typedef struct fake_device
{
    char node[32];
    const char *name; /* NULL: EVIOCGNAME fails. */
    tb_evdev_device description;
    uint8_t keys_down[KEY_CNT / 8];
    int32_t values[ABS_CNT];
    bool readable;
    bool writable;
    bool gone;        /* Reads fail with ENODEV: unplugged. */
    bool interrupted; /* The next read fails with EINTR. */
    bool open;
    struct input_event queue[FAKE_QUEUE];
    size_t queued;
    size_t taken;
    struct ff_effect effect;
    size_t uploads;
    int removed; /* The effect EVIOCRMFF removed, or -1. */
    struct input_event writes[FAKE_WRITES];
    size_t write_count;
} fake_device;

typedef struct fake_system
{
    char directory[64];
    fake_device devices[FAKE_DEVICES];
    size_t device_count;
    size_t closes;
} fake_system;

/* One test at a time uses it. */
static fake_system fake = {0};

static uint64_t fake_now = 0;

static uint64_t fake_clock(void)
{
    return fake_now;
}

static fake_device *fake_by_descriptor(int descriptor)
{
    const size_t index = (size_t)(descriptor - FAKE_FIRST_DESCRIPTOR);

    TB_TEST_REQUIRE_UINT(index, <, fake.device_count);
    return &fake.devices[index];
}

static int fake_open(const char *path, int flags)
{
    for (size_t index = 0; index < fake.device_count; index++)
    {
        fake_device *const device = &fake.devices[index];
        char expected[128] = {0};
        (void)tb_fmt_buffer(expected, sizeof(expected), "%s/%s", fake.directory, device->node);
        if (!tb_str_equal(expected, path))
        {
            continue;
        }
        if (!device->readable || ((flags & O_ACCMODE) == O_RDWR && !device->writable))
        {
            errno = EACCES;
            return -1;
        }
        device->open = true;
        return FAKE_FIRST_DESCRIPTOR + (int)index;
    }
    errno = ENOENT;
    return -1;
}

static int fake_close(int descriptor)
{
    fake_by_descriptor(descriptor)->open = false;
    fake.closes++;
    return 0;
}

static ssize_t fake_read(int descriptor, void *buffer, size_t size)
{
    fake_device *const device = fake_by_descriptor(descriptor);
    struct input_event *const events = buffer;
    size_t count = 0;

    if (device->gone)
    {
        errno = ENODEV;
        return -1;
    }
    if (device->interrupted)
    {
        device->interrupted = false;
        errno = EINTR;
        return -1;
    }
    if (device->taken == device->queued)
    {
        errno = EAGAIN;
        return -1;
    }
    while (device->taken < device->queued && (count + 1u) * sizeof(events[0]) <= size)
    {
        events[count] = device->queue[device->taken];
        count++;
        device->taken++;
    }
    return (ssize_t)(count * sizeof(events[0]));
}

static ssize_t fake_write(int descriptor, const void *buffer, size_t size)
{
    fake_device *const device = fake_by_descriptor(descriptor);

    TB_TEST_REQUIRE_UINT(size, ==, sizeof(struct input_event));
    TB_TEST_REQUIRE_UINT(device->write_count, <, FAKE_WRITES);
    memcpy(&device->writes[device->write_count], buffer, size);
    device->write_count++;
    return (ssize_t)size;
}

static int fake_bits(const uint8_t *bits, size_t length, unsigned size, void *argument)
{
    const size_t copied = size < length ? size : length;

    memcpy(argument, bits, copied);
    return (int)copied;
}

static int fake_ioctl(int descriptor, unsigned long request, void *argument)
{
    fake_device *const device = fake_by_descriptor(descriptor);
    const tb_evdev_device *const description = &device->description;
    const unsigned number = _IOC_NR(request);
    const unsigned size = _IOC_SIZE(request);

    TB_TEST_REQUIRE_UINT(_IOC_TYPE(request), ==, 'E');
    if (number == _IOC_NR(EVIOCGBIT(EV_KEY, 0)))
    {
        return fake_bits(description->keys, sizeof(description->keys), size, argument);
    }
    if (number == _IOC_NR(EVIOCGBIT(EV_ABS, 0)))
    {
        return fake_bits(description->axes, sizeof(description->axes), size, argument);
    }
    if (number == _IOC_NR(EVIOCGBIT(EV_FF, 0)))
    {
        return fake_bits(description->effects, sizeof(description->effects), size, argument);
    }
    if (number == _IOC_NR(EVIOCGKEY(0)))
    {
        return fake_bits(device->keys_down, sizeof(device->keys_down), size, argument);
    }
    if (number == _IOC_NR(EVIOCGID))
    {
        *(struct input_id *)argument = (struct input_id){
            .bustype = BUS_USB, .vendor = description->vendor, .product = description->product};
        return 0;
    }
    if (number == _IOC_NR(EVIOCGNAME(0)))
    {
        if (device->name == NULL)
        {
            errno = ENOTTY;
            return -1;
        }
        (void)tb_fmt_buffer(argument, size, "%s", device->name);
        return (int)tb_str_length(device->name) + 1;
    }
    if (number >= _IOC_NR(EVIOCGABS(0)) && number < _IOC_NR(EVIOCGABS(ABS_CNT)))
    {
        const unsigned code = number - _IOC_NR(EVIOCGABS(0));
        *(struct input_absinfo *)argument =
            (struct input_absinfo){.value = device->values[code],
                                   .minimum = description->minimum[code],
                                   .maximum = description->maximum[code]};
        return 0;
    }
    if (request == EVIOCSFF)
    {
        struct ff_effect *const effect = argument;
        if (effect->id == -1)
        {
            effect->id = 0;
        }
        device->effect = *effect;
        device->uploads++;
        return 0;
    }
    if (request == EVIOCRMFF)
    {
        device->removed = (int)(intptr_t)argument;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

static const tb_linux_device_calls fake_calls = {
    .open = fake_open,
    .close = fake_close,
    .read = fake_read,
    .write = fake_write,
    .ioctl = fake_ioctl,
};

static void set_bit(uint8_t *bits, uint32_t code)
{
    bits[code / 8u] = (uint8_t)(bits[code / 8u] | (1u << (code % 8u)));
}

static tb_path_view view(const char *text)
{
    return (tb_path_view){text, tb_str_length(text)};
}

static void touch(const char *node)
{
    char path[128] = {0};

    (void)tb_fmt_buffer(path, sizeof(path), "%s/%s", fake.directory, node);
    TB_TEST_REQUIRE_UINT(tb_fs_write_file(view(path), "", 0), ==, TB_FS_OK);
}

static void remove_node(const char *node)
{
    char path[128] = {0};

    (void)tb_fmt_buffer(path, sizeof(path), "%s/%s", fake.directory, node);
    TB_TEST_REQUIRE_UINT(tb_fs_remove(view(path)), ==, TB_FS_OK);
}

/* An Xbox pad on xpad, at a node of the directory. */
static fake_device *add_xpad(const char *node, bool writable)
{
    fake_device *const device = &fake.devices[fake.device_count];
    static const uint32_t keys[] = {BTN_SOUTH,  BTN_EAST,  BTN_NORTH, BTN_WEST,   BTN_TL,    BTN_TR,
                                    BTN_SELECT, BTN_START, BTN_MODE,  BTN_THUMBL, BTN_THUMBR};
    static const uint32_t sticks[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};

    TB_TEST_REQUIRE_UINT(fake.device_count, <, FAKE_DEVICES);
    fake.device_count++;
    *device = (fake_device){
        .name = "Microsoft X-Box One S pad", .readable = true, .writable = writable, .removed = -1};
    (void)tb_fmt_buffer(device->node, sizeof(device->node), "%s", node);
    device->description.vendor = 0x045e;
    device->description.product = 0x0b12;
    for (size_t index = 0; index < TB_TEST_COUNT_OF(keys); index++)
    {
        set_bit(device->description.keys, keys[index]);
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(sticks); index++)
    {
        set_bit(device->description.axes, sticks[index]);
        device->description.minimum[sticks[index]] = -32768;
        device->description.maximum[sticks[index]] = 32767;
    }
    set_bit(device->description.axes, ABS_Z);
    set_bit(device->description.axes, ABS_RZ);
    device->description.maximum[ABS_Z] = 1023;
    device->description.maximum[ABS_RZ] = 1023;
    set_bit(device->description.effects, FF_RUMBLE);
    touch(node);
    return device;
}

static void push(fake_device *device, uint16_t type, uint16_t code, int32_t value)
{
    TB_TEST_REQUIRE_UINT(device->queued, <, FAKE_QUEUE);
    device->queue[device->queued] =
        (struct input_event){.type = type, .code = code, .value = value};
    device->queued++;
}

static tb_gamepad_system *fake_open_system(const char *ignored)
{
    char template[] = "/tmp/tb_gamepads_XXXXXX";

    fake = (fake_system){0};
    fake_now = 0;
    TB_TEST_REQUIRE(mkdtemp(template) != NULL);
    (void)tb_fmt_buffer(fake.directory, sizeof(fake.directory), "%s", template);
    return tb_gamepad_system_make(&tb_linux_gamepad_ops,
                                  tb_linux_gamepads_make(&fake_calls, fake.directory, ignored),
                                  fake_clock);
}

/* The directory is left empty by the test, and removed. */
static void fake_close_system(tb_gamepad_system *system)
{
    tb_gamepad_system_close(system);
    for (size_t index = 0; index < fake.device_count; index++)
    {
        char path[128] = {0};
        (void)tb_fmt_buffer(path, sizeof(path), "%s/%s", fake.directory, fake.devices[index].node);
        (void)tb_fs_remove(view(path));
    }
    (void)tb_fs_remove(view(fake.directory));
}

static void expect_event(tb_gamepad_system *system, tb_gamepad_event_type type, size_t slot)
{
    tb_gamepad_event event = {0};

    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    TB_TEST_EXPECT_INT(event.type, ==, type);
    TB_TEST_EXPECT_UINT(event.slot, ==, slot);
}

/* The first poll lists the directory: a gamepad is kept and read; a
 * keyboard, a node that is not an event device, and one that cannot be
 * opened are passed over. */
TB_TEST(platform_linux_gamepad, the_first_poll_finds_the_gamepads)
{
    tb_gamepad_system *const system = fake_open_system(NULL);
    fake_device *const pad = add_xpad("event4", true);
    fake_device *const keyboard = &fake.devices[fake.device_count];
    tb_gamepad_state state = {0};
    tb_gamepad_info info = {0};

    fake.device_count++;
    *keyboard = (fake_device){.name = "Keyboard", .readable = true, .writable = true};
    (void)tb_fmt_buffer(keyboard->node, sizeof(keyboard->node), "event1");
    set_bit(keyboard->description.keys, KEY_A);
    touch("event1");
    touch("mouse0");
    touch("event9");
    pad->keys_down[BTN_EAST / 8] = (uint8_t)(1u << (BTN_EAST % 8));
    pad->values[ABS_X] = 32767;

    tb_gamepad_system_poll(system);
    expect_event(system, TB_GAMEPAD_EVENT_CONNECTED, 0);
    tb_gamepad_get_state(system, 0, &state);
    tb_gamepad_get_info(system, 0, &info);
    TB_TEST_EXPECT(tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_EAST));
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_X], ==, 1.0f);
    TB_TEST_EXPECT_STRING(info.name, ==, "Microsoft X-Box One S pad");
    TB_TEST_EXPECT_UINT(info.vendor, ==, 0x045e);
    TB_TEST_EXPECT_UINT(info.product, ==, 0x0b12);
    TB_TEST_EXPECT_HEX(info.capabilities, ==, TB_GAMEPAD_CAPABILITY_RUMBLE);
    TB_TEST_EXPECT(pad->open);
    TB_TEST_EXPECT(!keyboard->open);

    /* Its events, as they come; the guide button is not one. */
    push(pad, EV_KEY, BTN_SOUTH, 1);
    push(pad, EV_KEY, BTN_MODE, 1);
    push(pad, EV_ABS, ABS_Y, -32768);
    push(pad, EV_SYN, SYN_REPORT, 0);
    pad->interrupted = true;
    tb_gamepad_system_poll(system);
    tb_gamepad_get_state(system, 0, &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==,
                       (1u << TB_GAMEPAD_BUTTON_SOUTH) | (1u << TB_GAMEPAD_BUTTON_EAST));
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_Y], ==, 1.0f);
    fake_close_system(system);
    TB_TEST_EXPECT(!pad->open);
}

/* After SYN_DROPPED, what comes before the next report is not applied: the
 * device's whole state is read again at the report. More events than one
 * read takes are all read. */
TB_TEST(platform_linux_gamepad, a_drop_reads_the_state_again)
{
    tb_gamepad_system *const system = fake_open_system(NULL);
    fake_device *const pad = add_xpad("event0", true);
    tb_gamepad_state state = {0};

    tb_gamepad_system_poll(system);
    push(pad, EV_SYN, SYN_DROPPED, 0);
    push(pad, EV_KEY, BTN_SOUTH, 1);
    push(pad, EV_SYN, SYN_REPORT, 0);
    pad->keys_down[BTN_NORTH / 8] = (uint8_t)(1u << (BTN_NORTH % 8));
    tb_gamepad_system_poll(system);
    tb_gamepad_get_state(system, 0, &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 1u << TB_GAMEPAD_BUTTON_WEST);

    for (int32_t index = 0; index < 100; index++)
    {
        push(pad, EV_KEY, BTN_TL, index % 2 == 0 ? 1 : 0);
    }
    push(pad, EV_KEY, BTN_TR, 1);
    tb_gamepad_system_poll(system);
    tb_gamepad_get_state(system, 0, &state);
    TB_TEST_EXPECT(tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_RIGHT_SHOULDER));
    TB_TEST_EXPECT(!tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_LEFT_SHOULDER));
    TB_TEST_EXPECT_UINT(pad->taken, ==, pad->queued);
    fake_close_system(system);
}

/* Rumble is one FF_RUMBLE effect, uploaded and played; the low-frequency
 * motor is the strong one. Stopping writes a stop of it, and closing the
 * pad removes it. */
TB_TEST(platform_linux_gamepad, rumble_is_a_force_feedback_effect)
{
    tb_gamepad_system *const system = fake_open_system(NULL);
    fake_device *const pad = add_xpad("event0", true);

    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){1.0f, 0.5f, 0.25}));
    TB_TEST_EXPECT_UINT(pad->uploads, ==, 1);
    TB_TEST_EXPECT_UINT(pad->effect.type, ==, FF_RUMBLE);
    TB_TEST_EXPECT_UINT(pad->effect.u.rumble.strong_magnitude, ==, 65535);
    TB_TEST_EXPECT_UINT(pad->effect.u.rumble.weak_magnitude, ==, 32767);
    TB_TEST_EXPECT_UINT(pad->effect.replay.length, ==, 250);
    TB_TEST_REQUIRE_UINT(pad->write_count, ==, 1);
    TB_TEST_EXPECT_UINT(pad->writes[0].type, ==, EV_FF);
    TB_TEST_EXPECT_INT(pad->writes[0].value, ==, 1);

    /* The same effect again, longer than an effect's length can say. */
    TB_TEST_EXPECT(tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){0.5f, 0.5f, 100.0}));
    TB_TEST_EXPECT_INT(pad->effect.id, ==, 0);
    TB_TEST_EXPECT_UINT(pad->effect.replay.length, ==, 0xffff);

    tb_gamepad_system_set_focus(system, false);
    TB_TEST_REQUIRE_UINT(pad->write_count, ==, 3);
    TB_TEST_EXPECT_INT(pad->writes[2].value, ==, 0);
    fake_close_system(system);
    TB_TEST_EXPECT_INT(pad->removed, ==, 0);
}

/* A node that can be read but not written is a pad without rumble. */
TB_TEST(platform_linux_gamepad, a_pad_read_only_cannot_rumble)
{
    tb_gamepad_system *const system = fake_open_system(NULL);
    tb_gamepad_info info = {0};

    (void)add_xpad("event0", false);
    tb_gamepad_system_poll(system);
    tb_gamepad_get_info(system, 0, &info);
    TB_TEST_EXPECT(info.name[0] != '\0');
    TB_TEST_EXPECT_HEX(info.capabilities, ==, 0);
    TB_TEST_EXPECT(!tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){1.0f, 1.0f, 1.0}));
    fake_close_system(system);
}

/* inotify reports a node that comes, takes the lowest free slot; one whose
 * permissions are set later is tried again then; one removed, or one whose
 * reads fail as an unplugged pad's do, is gone. */
TB_TEST(platform_linux_gamepad, pads_come_and_go_as_their_nodes_do)
{
    tb_gamepad_system *const system = fake_open_system(NULL);
    fake_device *first = NULL;
    fake_device *second = NULL;
    fake_device *third = NULL;
    char path[128] = {0};

    tb_gamepad_system_poll(system);
    first = add_xpad("event0", true);
    second = add_xpad("event1", true);
    second->readable = false;
    tb_gamepad_system_poll(system);
    expect_event(system, TB_GAMEPAD_EVENT_CONNECTED, 0);
    TB_TEST_EXPECT(!second->open);

    /* udev sets the permissions: an attribute changes. */
    second->readable = true;
    (void)tb_fmt_buffer(path, sizeof(path), "%s/event1", fake.directory);
    TB_TEST_REQUIRE_INT(chmod(path, 0644), ==, 0);
    tb_gamepad_system_poll(system);
    expect_event(system, TB_GAMEPAD_EVENT_CONNECTED, 1);

    remove_node("event0");
    tb_gamepad_system_poll(system);
    expect_event(system, TB_GAMEPAD_EVENT_DISCONNECTED, 0);
    TB_TEST_EXPECT(!first->open);

    /* The free slot is the lowest. */
    third = add_xpad("event2", true);
    tb_gamepad_system_poll(system);
    expect_event(system, TB_GAMEPAD_EVENT_CONNECTED, 0);
    TB_TEST_EXPECT(third->open);

    second->gone = true;
    tb_gamepad_system_poll(system);
    expect_event(system, TB_GAMEPAD_EVENT_DISCONNECTED, 1);
    TB_TEST_EXPECT(!second->open);
    fake_close_system(system);
}

/* The pads Steam stands in for are left alone; a pad without a name is
 * named by its node; and pads past the last slot wait. */
TB_TEST(platform_linux_gamepad, ignored_unnamed_and_too_many_pads)
{
    tb_gamepad_system *system = fake_open_system("0x045e/0x0b12");
    fake_device *pad = add_xpad("event0", true);
    tb_gamepad_info info = {0};
    tb_gamepad_state state = {0};
    size_t connected = 0;

    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(!pad->open);
    fake_close_system(system);

    system = fake_open_system(NULL);
    pad = add_xpad("event0", true);
    pad->name = NULL;
    tb_gamepad_system_poll(system);
    tb_gamepad_get_info(system, 0, &info);
    TB_TEST_EXPECT_STRING(info.name, ==, "event0");
    fake_close_system(system);

    system = fake_open_system(NULL);
    for (size_t index = 0; index <= TB_GAMEPAD_SLOT_COUNT; index++)
    {
        char node[16] = {0};
        (void)tb_fmt_buffer(node, sizeof(node), "event%zu", index);
        (void)add_xpad(node, true);
    }
    tb_gamepad_system_poll(system);
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        tb_gamepad_get_state(system, slot, &state);
        connected += state.connected ? 1u : 0u;
    }
    TB_TEST_EXPECT_UINT(connected, ==, TB_GAMEPAD_SLOT_COUNT);
    fake_close_system(system);
}
