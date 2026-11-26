/* What a platform's raw HID backend and the protocol drivers of HID pads
 * share. A driver is standard C: it knows one family of pads, the commands
 * that set them up, their input reports and their rumble. A backend finds
 * the pads, opens them, reads their reports and sends what the driver
 * makes, through the platform's own calls (hid.dll and WinUSB on Windows).
 * A new pad is a new driver in the list of tb_hid_driver_find(); the
 * backends do not change. */
#ifndef TB_PLATFORM_SHARED_GAMEPAD_HID_H
#define TB_PLATFORM_SHARED_GAMEPAD_HID_H

#include "badger/gamepad.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The largest report a driver reads or makes: a DualSense's over
 * Bluetooth. A backend reads and writes reports of the lengths the
 * device's own descriptor gives, which may be longer. */
#define TB_HID_REPORT_CAPACITY 78u

/* A Windows device interface class's GUID, as its fields, so that standard
 * C can name one. */
typedef struct tb_hid_guid
{
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
} tb_hid_guid;

/* What a driver may do with the pad it drives. */
typedef struct tb_hid_link
{
    void *context;
    /* The pad is reached over Bluetooth, not USB. */
    bool bluetooth;
    /* Send a command on the pad's vendor channel, the interface beside its
     * HID interface that takes its setup commands, and wait for the reply.
     * Returns the reply's length, at most `capacity`, or 0 when it failed
     * or the pad has no channel: another program has it, or the platform
     * cannot open it. */
    size_t (*command)(void *context, const uint8_t *command, size_t length, uint8_t *reply,
                      size_t capacity);
    /* Send an output report, its ID first; whether it was sent. */
    bool (*output)(void *context, const uint8_t *report, size_t length);
    /* Read a feature report into `report`, its ID first. Returns its
     * length, at most `capacity`, or 0 when the pad refused. */
    size_t (*feature)(void *context, uint8_t id, uint8_t *report, size_t capacity);
} tb_hid_link;

/* A family of HID pads. Every function is required; `state` is
 * `state_size` bytes, zero when the pad was opened, that the backend keeps
 * for the pad until it goes. */
typedef struct tb_hid_driver
{
    const char *name;      /* The pads' name, in UTF-8, as tb_gamepad_info gives it. */
    uint32_t capabilities; /* #tb_gamepad_capability bits. */
    size_t state_size;
    /* The vendor channel's device interface class on Windows, given by the
     * pad's own descriptors; all zero for a pad without one. */
    tb_hid_guid channel_class;
    /* How long a rumble report lasts, in nanoseconds: the backend sends the
     * report again at least this often while the motors run. 0 for pads
     * whose motors run until the next report. */
    uint64_t rumble_period;
    /* Whether the driver drives a pad of this USB vendor and product. */
    bool (*matches)(uint16_t vendor, uint16_t product);
    /* Set a pad up after it was opened: read what it says of itself, and
     * send the commands that make it report. Called once, on the system's
     * thread; it may block for the commands' replies. */
    void (*start)(void *state, const tb_hid_link *link);
    /* Which physical pad it is, once started: a number that is the same
     * for the pad over USB and over Bluetooth, as its Bluetooth address,
     * so that a pad connected both ways is read once; 0 when not known. */
    uint64_t (*identity)(const void *state);
    /* Read an input report, its ID first, into a pad's state; false for a
     * report that is not one of its states, which changes nothing. The
     * report may be followed by zeros, to the length the device's
     * descriptor gives its longest. */
    bool (*parse)(void *state, const uint8_t *report, size_t length, tb_gamepad_state *pad);
    /* Make the output report that runs the motors, from 0 to 1 and already
     * scaled, both 0 to stop them. Returns its length, at most `capacity`. */
    size_t (*rumble)(void *state, float low_frequency, float high_frequency, uint8_t *report,
                     size_t capacity);
    /* Make the output report that shows the pad is the player of the
     * system's slot `player`, from 0, as its lights. Returns its length, at
     * most `capacity`, or 0 for a pad that shows none. */
    size_t (*player)(void *state, size_t player, uint8_t *report, size_t capacity);
} tb_hid_driver;

/* The driver of a pad, or NULL for a pad that no driver knows. */
const tb_hid_driver *tb_hid_driver_find(uint16_t vendor, uint16_t product);

#endif
