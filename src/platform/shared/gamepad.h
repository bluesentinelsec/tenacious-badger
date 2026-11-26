/* What the shared front of the gamepad interface and each platform's
 * backend share: the table of functions a backend fills in, what it reports
 * of each slot, and the helpers that scale a pad's own values to the
 * standard ranges. */
#ifndef TB_PLATFORM_SHARED_GAMEPAD_H
#define TB_PLATFORM_SHARED_GAMEPAD_H

#include "badger/gamepad.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A slot as a backend reports it. */
typedef struct tb_gamepad_pad
{
    tb_gamepad_state state; /* All zero, `connected` included, for an empty slot. */
    tb_gamepad_info info;   /* All zero for an empty slot. */
    /* Which connection the slot holds: a number the backend gives each pad
     * it connects, never the same twice, so that a pad unplugged and another
     * plugged in between two polls are seen as two; 0 for none. */
    uint64_t serial;
} tb_gamepad_pad;

/* What a backend does; each function is required, and called on the
 * system's thread. */
typedef struct tb_gamepad_ops
{
    /* Release the backend's state; rumble has been stopped. */
    void (*close)(void *native);
    /* Read every pad into `pads`, #TB_GAMEPAD_SLOT_COUNT of them, which
     * hold what the last poll left: connect new pads to the lowest free
     * slot, keep each pad in its slot while it stays, and empty the slots
     * of pads that went. `now` is the clock's, in nanoseconds. */
    void (*poll)(void *native, uint64_t now, tb_gamepad_pad *pads);
    /* Run a connected pad's motors, from 0 to 1 and already scaled, for
     * `seconds`, in place of what they ran; both 0 stop them. The front
     * stops them when the time is up, so a backend whose pads take no time
     * may ignore it. Called only for a pad with
     * #TB_GAMEPAD_CAPABILITY_RUMBLE. */
    void (*rumble)(void *native, size_t slot, float low_frequency, float high_frequency,
                   double seconds);
    /* Show on the pad in `slot` that it is the system's player `player`,
     * from 0, as its lights, where it has lights the backend sets. Called
     * when a pad comes into a slot. NULL for a backend that sets none. */
    void (*player)(void *native, size_t slot, size_t player);
} tb_gamepad_ops;

/* The clock the front times rumble by: tb_platform_clock_nanoseconds(), or
 * a test's. */
typedef uint64_t tb_gamepad_clock(void);

/* Connect a gamepad system to the platform's gamepads: give `*ops` and
 * `*native`. Each platform adapter defines it. */
tb_gamepad_result tb_native_gamepad_open(const tb_gamepad_ops **ops, void **native,
                                         tb_gamepad_error *error);

/* Make a gamepad system over a backend, as tb_gamepad_system_open() does
 * over the platform's: for tests, with a backend and a clock of their own.
 * Never NULL; exhaustion calls the memory failure handler. */
tb_gamepad_system *tb_gamepad_system_make(const tb_gamepad_ops *ops, void *native,
                                          tb_gamepad_clock *clock);

/* Fill in an error, formatted as tb_fmt_buffer() does; returns `result`.
 * `error` may be NULL. */
tb_gamepad_result tb_gamepad_fail(tb_gamepad_error *error, tb_gamepad_result result,
                                  const char *format, ...);

/* A stick's axis from a pad's range to -1 to 1: the middle of the range is
 * 0, and each half scales to its own end, so that a range one longer on one
 * side, as -32768 to 32767, reaches both -1 and 1. `negate` turns a range
 * whose larger values are down into one whose are up. A range of one value
 * gives 0. */
float tb_gamepad_stick_value(int32_t value, int32_t minimum, int32_t maximum, bool negate);

/* A trigger from a pad's range to 0 to 1, clamped. A range of one value
 * gives 0. */
float tb_gamepad_trigger_value(int32_t value, int32_t minimum, int32_t maximum);

/* Copy a name the system gave into a pad's info: cut at a character to
 * fit, and with any byte that is not valid UTF-8 replaced by '?'. */
void tb_gamepad_copy_name(char *name, const char *source, size_t length);

/* Whether a list of pads to leave alone names a device: entries of
 * `0xVVVV/0xPPPP`, by USB vendor and product, between commas. Steam sets
 * such a list in SDL_GAMECONTROLLER_IGNORE_DEVICES for the games it
 * starts, to name the physical pads its own virtual pads stand for, so
 * that each pad is seen once. `list` may be NULL, for none. */
bool tb_gamepad_ignored(const char *list, uint16_t vendor, uint16_t product);

#endif
