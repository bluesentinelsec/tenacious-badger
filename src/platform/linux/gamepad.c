/* Linux and Steam Deck gamepads: evdev, the event devices of /dev/input.
 *
 * Every device node is opened without blocking and read without grabbing:
 * the engine never asks for EVIOCGRAB, so the desktop, Steam and other
 * programs see the pad too, and its guide and share buttons stay theirs.
 * A device is a gamepad when it has the south button and a stick's axes
 * (gamepad_evdev.c). inotify on /dev/input reports nodes that come, go, or
 * have their permissions set: udev sets them a moment after a node is made,
 * so a node that cannot be opened yet is tried again then.
 *
 * Under Steam, each physical pad also appears as a virtual pad that Steam
 * Input makes, and Steam names the physical ones in
 * SDL_GAMECONTROLLER_IGNORE_DEVICES for the games it starts; those are left
 * alone, so that each pad is seen once.
 *
 * Rumble is force feedback of type FF_RUMBLE: one effect a pad, uploaded
 * with EVIOCSFF and played by writing it as an EV_FF event, on pads whose
 * node could be opened for writing. */
#define _GNU_SOURCE
#include "gamepad_evdev.h"
#include "gamepad_linux.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

TB_STATIC_ASSERT(EV_KEY == TB_EVDEV_EV_KEY && EV_ABS == TB_EVDEV_EV_ABS &&
                     EV_FF == TB_EVDEV_EV_FF && EV_SYN == TB_EVDEV_EV_SYN,
                 "the kernel's event types");
TB_STATIC_ASSERT(SYN_REPORT == TB_EVDEV_SYN_REPORT && SYN_DROPPED == TB_EVDEV_SYN_DROPPED,
                 "the kernel's synchronization codes");
TB_STATIC_ASSERT(BTN_SOUTH == TB_EVDEV_BTN_SOUTH && BTN_EAST == TB_EVDEV_BTN_EAST &&
                     BTN_NORTH == TB_EVDEV_BTN_NORTH && BTN_WEST == TB_EVDEV_BTN_WEST &&
                     BTN_X == TB_EVDEV_BTN_NORTH && BTN_Y == TB_EVDEV_BTN_WEST,
                 "the kernel's face buttons");
TB_STATIC_ASSERT(BTN_TL == TB_EVDEV_BTN_TL && BTN_TR2 == TB_EVDEV_BTN_TR2 &&
                     BTN_SELECT == TB_EVDEV_BTN_SELECT && BTN_MODE == TB_EVDEV_BTN_MODE &&
                     BTN_THUMBR == TB_EVDEV_BTN_THUMBR,
                 "the kernel's gamepad buttons");
TB_STATIC_ASSERT(BTN_DPAD_UP == TB_EVDEV_BTN_DPAD_UP && BTN_DPAD_RIGHT == TB_EVDEV_BTN_DPAD_RIGHT &&
                     BTN_TRIGGER_HAPPY1 == TB_EVDEV_BTN_TRIGGER_HAPPY1 &&
                     BTN_TRIGGER_HAPPY8 == TB_EVDEV_BTN_TRIGGER_HAPPY8,
                 "the kernel's d-pad and extra buttons");
TB_STATIC_ASSERT(KEY_CNT == TB_EVDEV_KEY_COUNT && ABS_CNT == TB_EVDEV_ABS_COUNT &&
                     FF_CNT == TB_EVDEV_FF_COUNT && FF_RUMBLE == TB_EVDEV_FF_RUMBLE,
                 "the kernel's counts");
TB_STATIC_ASSERT(ABS_RX == TB_EVDEV_ABS_RX && ABS_RZ == TB_EVDEV_ABS_RZ &&
                     ABS_GAS == TB_EVDEV_ABS_GAS && ABS_BRAKE == TB_EVDEV_ABS_BRAKE &&
                     ABS_HAT0Y == TB_EVDEV_ABS_HAT0Y && ABS_HAT2X == TB_EVDEV_ABS_HAT2X &&
                     ABS_HAT2Y == TB_EVDEV_ABS_HAT2Y,
                 "the kernel's axes");

/* The directory of the event devices, and the prefix of their nodes. */
#define TB_LINUX_INPUT_DIRECTORY "/dev/input"
#define TB_LINUX_EVENT_PREFIX "event"

/* The variable in which Steam names the pads it stands in for. */
#define TB_LINUX_IGNORE_VARIABLE "SDL_GAMECONTROLLER_IGNORE_DEVICES"

/* Bytes of a node's path, and of its name. */
#define TB_LINUX_PATH_CAPACITY 256
#define TB_LINUX_NODE_CAPACITY 64

/* Events read from a device at a time. */
#define TB_LINUX_EVENT_BATCH 64

/* What inotify watches the directory for. */
#define TB_LINUX_WATCHED (IN_CREATE | IN_ATTRIB | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM)

/* The longest an effect plays, in milliseconds: its length's largest value. */
#define TB_LINUX_LONGEST_EFFECT 0xffffu

/* The largest magnitude of a motor. */
#define TB_LINUX_MOTOR_MAX 65535.0f

/* No effect uploaded. */
#define TB_LINUX_NO_EFFECT (-1)

typedef struct tb_linux_pad
{
    bool open;
    int descriptor;
    char node[TB_LINUX_NODE_CAPACITY]; /* As "event12". */
    tb_evdev_device device;
    tb_evdev_layout layout;
    tb_gamepad_state state;
    tb_gamepad_info info;
    uint64_t serial;
    bool dropped;   /* Events were dropped: wait for a report, then read the state again. */
    bool writable;  /* Opened for writing, as rumble needs. */
    int16_t effect; /* The rumble effect uploaded, or TB_LINUX_NO_EFFECT. */
} tb_linux_pad;

typedef struct tb_linux_gamepads
{
    tb_linux_device_calls calls;
    char directory[TB_LINUX_PATH_CAPACITY];
    char *ignored; /* NULL for none. */
    int watch;     /* inotify's descriptor, or -1 without it. */
    bool scanned;  /* The directory was listed, at the first poll. */
    uint64_t serial;
    tb_linux_pad pads[TB_GAMEPAD_SLOT_COUNT]; /* By slot. */
} tb_linux_gamepads;

/* ---------------------------------------------------------------------------
 * Devices.
 * ------------------------------------------------------------------------- */

static void tb_linux_close_pad(tb_linux_gamepads *gamepads, tb_linux_pad *pad)
{
    if (pad->effect != TB_LINUX_NO_EFFECT)
    {
        (void)gamepads->calls.ioctl(pad->descriptor, EVIOCRMFF, (void *)(intptr_t)pad->effect);
    }
    (void)gamepads->calls.close(pad->descriptor);
    *pad = (tb_linux_pad){.descriptor = -1, .effect = TB_LINUX_NO_EFFECT};
}

/* Read what a device has: its keys, axes and effects, its identity, and
 * each axis's range. */
static void tb_linux_describe(const tb_linux_gamepads *gamepads, int descriptor,
                              tb_evdev_device *device)
{
    struct input_id identity = {0};
    struct input_absinfo range = {0};

    *device = (tb_evdev_device){0};
    (void)gamepads->calls.ioctl(descriptor, EVIOCGBIT(EV_KEY, sizeof(device->keys)), device->keys);
    (void)gamepads->calls.ioctl(descriptor, EVIOCGBIT(EV_ABS, sizeof(device->axes)), device->axes);
    (void)gamepads->calls.ioctl(descriptor, EVIOCGBIT(EV_FF, sizeof(device->effects)),
                                device->effects);
    if (gamepads->calls.ioctl(descriptor, EVIOCGID, &identity) == 0)
    {
        device->vendor = identity.vendor;
        device->product = identity.product;
    }
    for (uint32_t code = 0; code < TB_EVDEV_ABS_COUNT; code++)
    {
        if (tb_evdev_has(device->axes, TB_EVDEV_ABS_COUNT, code) &&
            gamepads->calls.ioctl(descriptor, EVIOCGABS(code), &range) == 0)
        {
            device->minimum[code] = range.minimum;
            device->maximum[code] = range.maximum;
        }
    }
}

/* Read a pad's whole state from the device: when it is opened, and after
 * its events were dropped. */
static void tb_linux_resync(const tb_linux_gamepads *gamepads, tb_linux_pad *pad)
{
    uint8_t keys_down[TB_EVDEV_KEY_COUNT / 8u] = {0};
    int32_t values[TB_EVDEV_ABS_COUNT] = {0};
    struct input_absinfo range = {0};

    (void)gamepads->calls.ioctl(pad->descriptor, EVIOCGKEY(sizeof(keys_down)), keys_down);
    for (uint32_t code = 0; code < TB_EVDEV_ABS_COUNT; code++)
    {
        if (tb_evdev_has(pad->device.axes, TB_EVDEV_ABS_COUNT, code) &&
            gamepads->calls.ioctl(pad->descriptor, EVIOCGABS(code), &range) == 0)
        {
            values[code] = range.value;
        }
    }
    tb_evdev_resync(&pad->device, &pad->layout, keys_down, values, &pad->state);
}

static void tb_linux_name(const tb_linux_gamepads *gamepads, tb_linux_pad *pad)
{
    char name[TB_GAMEPAD_NAME_CAPACITY] = {0};

    if (gamepads->calls.ioctl(pad->descriptor, EVIOCGNAME(sizeof(name) - 1u), name) < 0)
    {
        (void)tb_fmt_buffer(name, sizeof(name), "%s", pad->node);
    }
    tb_gamepad_copy_name(pad->info.name, name, tb_str_length(name));
}

static int tb_linux_open_node(const tb_linux_gamepads *gamepads, const char *node, bool *writable)
{
    char path[TB_LINUX_PATH_CAPACITY] = {0};
    int descriptor = -1;

    (void)tb_fmt_buffer(path, sizeof(path), "%s/%s", gamepads->directory, node);
    descriptor = gamepads->calls.open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    *writable = descriptor >= 0;
    if (descriptor < 0)
    {
        descriptor = gamepads->calls.open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    return descriptor;
}

static tb_linux_pad *tb_linux_find(tb_linux_gamepads *gamepads, const char *node)
{
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        if (gamepads->pads[slot].open && tb_str_equal(gamepads->pads[slot].node, node))
        {
            return &gamepads->pads[slot];
        }
    }
    return NULL;
}

static tb_linux_pad *tb_linux_free_slot(tb_linux_gamepads *gamepads)
{
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        if (!gamepads->pads[slot].open)
        {
            return &gamepads->pads[slot];
        }
    }
    return NULL;
}

/* Open a node of the directory, and keep it when it is a gamepad that is
 * not to be left alone. A node already open, or one that cannot be opened
 * yet, is passed over. */
static void tb_linux_try_open(tb_linux_gamepads *gamepads, const char *node)
{
    tb_linux_pad *const pad = tb_linux_free_slot(gamepads);
    tb_evdev_device device = {0};
    bool writable = false;
    int descriptor = -1;

    if (!tb_str_starts_with(node, TB_LINUX_EVENT_PREFIX) ||
        tb_str_length(node) >= TB_LINUX_NODE_CAPACITY || tb_linux_find(gamepads, node) != NULL ||
        pad == NULL)
    {
        return;
    }
    descriptor = tb_linux_open_node(gamepads, node, &writable);
    if (descriptor < 0)
    {
        return;
    }
    tb_linux_describe(gamepads, descriptor, &device);
    if (!tb_evdev_is_gamepad(&device) ||
        tb_gamepad_ignored(gamepads->ignored, device.vendor, device.product))
    {
        (void)gamepads->calls.close(descriptor);
        return;
    }
    *pad = (tb_linux_pad){.open = true,
                          .descriptor = descriptor,
                          .device = device,
                          .writable = writable,
                          .effect = TB_LINUX_NO_EFFECT};
    (void)tb_fmt_buffer(pad->node, sizeof(pad->node), "%s", node);
    tb_evdev_layout_make(&pad->device, &pad->layout);
    tb_linux_resync(gamepads, pad);
    tb_linux_name(gamepads, pad);
    pad->info.vendor = device.vendor;
    pad->info.product = device.product;
    if (writable && tb_evdev_can_rumble(&device))
    {
        pad->info.capabilities |= TB_GAMEPAD_CAPABILITY_RUMBLE;
    }
    gamepads->serial++;
    pad->serial = gamepads->serial;
}

static void tb_linux_forget(tb_linux_gamepads *gamepads, const char *node)
{
    tb_linux_pad *const pad = tb_linux_find(gamepads, node);

    if (pad != NULL)
    {
        tb_linux_close_pad(gamepads, pad);
    }
}

/* ---------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

static void tb_linux_take_event(const tb_linux_gamepads *gamepads, tb_linux_pad *pad,
                                const struct input_event *event)
{
    if (pad->dropped)
    {
        /* What came between the drop and the next report is incomplete:
         * the device's whole state is read again at the report. */
        if (event->type == EV_SYN && event->code == SYN_REPORT)
        {
            pad->dropped = false;
            tb_linux_resync(gamepads, pad);
        }
        return;
    }
    if (event->type == EV_SYN && event->code == SYN_DROPPED)
    {
        pad->dropped = true;
        return;
    }
    tb_evdev_apply(&pad->device, &pad->layout, event->type, event->code, event->value, &pad->state);
}

/* Read what a pad sent since the last poll; a pad that is gone is closed. */
static void tb_linux_read_pad(tb_linux_gamepads *gamepads, tb_linux_pad *pad)
{
    struct input_event events[TB_LINUX_EVENT_BATCH] = {0};

    for (;;)
    {
        const ssize_t got = gamepads->calls.read(pad->descriptor, events, sizeof(events));
        if (got < 0 && errno == EINTR)
        {
            continue;
        }
        if (got < 0 && errno == EAGAIN)
        {
            return;
        }
        if (got <= 0)
        {
            /* ENODEV: the pad was unplugged. */
            tb_linux_close_pad(gamepads, pad);
            return;
        }
        for (size_t index = 0; index < (size_t)got / sizeof(events[0]); index++)
        {
            tb_linux_take_event(gamepads, pad, &events[index]);
        }
        if ((size_t)got < sizeof(events))
        {
            return;
        }
    }
}

static void tb_linux_take_change(tb_linux_gamepads *gamepads, const struct inotify_event *change)
{
    if (change->len == 0)
    {
        return;
    }
    if ((change->mask & (IN_DELETE | IN_MOVED_FROM)) != 0)
    {
        tb_linux_forget(gamepads, change->name);
        return;
    }
    tb_linux_try_open(gamepads, change->name);
}

/* Nodes that came, went, or became readable since the last poll. */
static void tb_linux_watch(tb_linux_gamepads *gamepads)
{
    _Alignas(struct inotify_event) char buffer[4096] = {0};

    if (gamepads->watch < 0)
    {
        return;
    }
    for (;;)
    {
        const ssize_t got = read(gamepads->watch, buffer, sizeof(buffer));
        size_t offset = 0;
        if (got <= 0)
        {
            return;
        }
        while (offset + sizeof(struct inotify_event) <= (size_t)got)
        {
            const struct inotify_event *const change =
                (const struct inotify_event *)(const void *)(buffer + offset);
            tb_linux_take_change(gamepads, change);
            offset += sizeof(struct inotify_event) + change->len;
        }
    }
}

static void tb_linux_scan(tb_linux_gamepads *gamepads)
{
    DIR *const directory = opendir(gamepads->directory);

    gamepads->scanned = true;
    if (directory == NULL)
    {
        return;
    }
    for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory))
    {
        tb_linux_try_open(gamepads, entry->d_name);
    }
    (void)closedir(directory);
}

static void tb_linux_poll(void *native, uint64_t now, tb_gamepad_pad *pads)
{
    tb_linux_gamepads *const gamepads = native;

    (void)now;
    if (!gamepads->scanned)
    {
        tb_linux_scan(gamepads);
    }
    tb_linux_watch(gamepads);
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        tb_linux_pad *const pad = &gamepads->pads[slot];
        if (pad->open)
        {
            tb_linux_read_pad(gamepads, pad);
        }
        pads[slot] = (tb_gamepad_pad){0};
        if (pad->open)
        {
            pads[slot] =
                (tb_gamepad_pad){.state = pad->state, .info = pad->info, .serial = pad->serial};
        }
    }
}

/* ---------------------------------------------------------------------------
 * Rumble.
 * ------------------------------------------------------------------------- */

static void tb_linux_play(const tb_linux_gamepads *gamepads, const tb_linux_pad *pad, int32_t play)
{
    struct input_event event = {0};

    event.type = EV_FF;
    event.code = (uint16_t)pad->effect;
    event.value = play;
    (void)gamepads->calls.write(pad->descriptor, &event, sizeof(event));
}

static void tb_linux_rumble(void *native, size_t slot, float low_frequency, float high_frequency,
                            double seconds)
{
    tb_linux_gamepads *const gamepads = native;
    tb_linux_pad *const pad = &gamepads->pads[slot];
    struct ff_effect effect = {0};
    double milliseconds = seconds * 1000.0;

    TB_ASSERT(slot < TB_GAMEPAD_SLOT_COUNT && pad->open);
    if (low_frequency <= 0.0f && high_frequency <= 0.0f)
    {
        if (pad->effect != TB_LINUX_NO_EFFECT)
        {
            tb_linux_play(gamepads, pad, 0);
        }
        return;
    }
    if (milliseconds > (double)TB_LINUX_LONGEST_EFFECT)
    {
        milliseconds = (double)TB_LINUX_LONGEST_EFFECT;
    }
    effect.type = FF_RUMBLE;
    effect.id = pad->effect;
    effect.replay.length = (uint16_t)(milliseconds + 0.5);
    effect.u.rumble.strong_magnitude = (uint16_t)(low_frequency * TB_LINUX_MOTOR_MAX);
    effect.u.rumble.weak_magnitude = (uint16_t)(high_frequency * TB_LINUX_MOTOR_MAX);
    if (gamepads->calls.ioctl(pad->descriptor, EVIOCSFF, &effect) < 0)
    {
        return;
    }
    pad->effect = effect.id;
    tb_linux_play(gamepads, pad, 1);
}

static void tb_linux_close(void *native)
{
    tb_linux_gamepads *const gamepads = native;

    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        if (gamepads->pads[slot].open)
        {
            tb_linux_close_pad(gamepads, &gamepads->pads[slot]);
        }
    }
    if (gamepads->watch >= 0)
    {
        (void)close(gamepads->watch);
    }
    tb_free(gamepads->ignored);
    tb_free(gamepads);
}

const tb_gamepad_ops tb_linux_gamepad_ops = {
    .close = tb_linux_close,
    .poll = tb_linux_poll,
    .rumble = tb_linux_rumble,
};

void *tb_linux_gamepads_make(const tb_linux_device_calls *calls, const char *directory,
                             const char *ignored)
{
    tb_linux_gamepads *const gamepads = tb_calloc(1, sizeof(*gamepads));

    TB_ASSERT(calls != NULL && directory != NULL);
    gamepads->calls = *calls;
    (void)tb_fmt_buffer(gamepads->directory, sizeof(gamepads->directory), "%s", directory);
    if (ignored != NULL)
    {
        gamepads->ignored = tb_str_dup(NULL, ignored);
    }
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        gamepads->pads[slot] = (tb_linux_pad){.descriptor = -1, .effect = TB_LINUX_NO_EFFECT};
    }
    /* Watched before it is listed, so that a node made between the two is
     * not missed. Without inotify, or without the directory, pads are read
     * as they were at the first poll. */
    gamepads->watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (gamepads->watch >= 0 &&
        inotify_add_watch(gamepads->watch, gamepads->directory, TB_LINUX_WATCHED) < 0)
    {
        (void)close(gamepads->watch);
        gamepads->watch = -1;
    }
    return gamepads;
}

/* ---------------------------------------------------------------------------
 * The system's calls.
 * ------------------------------------------------------------------------- */

static int tb_linux_system_open(const char *path, int flags)
{
    return open(path, flags);
}

static ssize_t tb_linux_system_read(int descriptor, void *buffer, size_t size)
{
    return read(descriptor, buffer, size);
}

static ssize_t tb_linux_system_write(int descriptor, const void *buffer, size_t size)
{
    return write(descriptor, buffer, size);
}

static int tb_linux_system_ioctl(int descriptor, unsigned long request, void *argument)
{
    return ioctl(descriptor, request, argument);
}

tb_gamepad_result tb_native_gamepad_open(const tb_gamepad_ops **ops, void **native,
                                         tb_gamepad_error *error)
{
    static const tb_linux_device_calls calls = {
        .open = tb_linux_system_open,
        .close = close,
        .read = tb_linux_system_read,
        .write = tb_linux_system_write,
        .ioctl = tb_linux_system_ioctl,
    };

    (void)error;
    *ops = &tb_linux_gamepad_ops;
    *native =
        tb_linux_gamepads_make(&calls, TB_LINUX_INPUT_DIRECTORY, getenv(TB_LINUX_IGNORE_VARIABLE));
    return TB_GAMEPAD_OK;
}
