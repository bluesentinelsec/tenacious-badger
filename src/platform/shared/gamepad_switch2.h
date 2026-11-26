/* Private to the HID backends and their tests: the protocol of Nintendo's
 * Switch 2 controllers, in standard C. See gamepad_switch2.c. */
#ifndef TB_PLATFORM_SHARED_GAMEPAD_SWITCH2_H
#define TB_PLATFORM_SHARED_GAMEPAD_SWITCH2_H

#include "gamepad_hid.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* USB IDs: Nintendo's vendor, and the Switch 2 controllers' products. The
 * driver drives the Pro Controller; the others are named for the evdev
 * mapping and the documentation. */
#define TB_SWITCH2_VENDOR 0x057eu
#define TB_SWITCH2_PRODUCT_JOYCON_RIGHT 0x2066u
#define TB_SWITCH2_PRODUCT_JOYCON_LEFT 0x2067u
#define TB_SWITCH2_PRODUCT_PRO 0x2069u
#define TB_SWITCH2_PRODUCT_GAMECUBE 0x2073u

/* The input reports: the format every Switch 2 controller has, and the
 * Pro Controller's own, which the driver selects. */
#define TB_SWITCH2_REPORT_COMMON 0x05u
#define TB_SWITCH2_REPORT_PRO 0x09u
/* The Pro Controller's output report, which carries rumble. */
#define TB_SWITCH2_REPORT_RUMBLE 0x02u

/* Bytes of an input and an output report, the ID included. */
#define TB_SWITCH2_REPORT_SIZE 64u

/* Bytes of a command's header, and the most a command or reply has. */
#define TB_SWITCH2_HEADER_SIZE 8u
#define TB_SWITCH2_COMMAND_CAPACITY 64u

/* Commands: their IDs and the subcommands the driver sends. */
#define TB_SWITCH2_COMMAND_FLASH 0x02u
#define TB_SWITCH2_FLASH_READ 0x04u
#define TB_SWITCH2_COMMAND_INIT 0x03u
#define TB_SWITCH2_INIT_SELECT_REPORT 0x0au
#define TB_SWITCH2_INIT_USB 0x0du
#define TB_SWITCH2_COMMAND_LED 0x09u
#define TB_SWITCH2_LED_PATTERN 0x07u
#define TB_SWITCH2_COMMAND_FEATURES 0x0cu
#define TB_SWITCH2_FEATURES_SET_MASK 0x02u
#define TB_SWITCH2_FEATURES_ENABLE 0x04u
#define TB_SWITCH2_COMMAND_RUMBLE_SETUP 0x11u
#define TB_SWITCH2_RUMBLE_SETUP_QUERY 0x01u

/* Where the flash holds the sticks' calibrations: the factory's, and the
 * player's own, which a console writes and which wins when it is there. */
#define TB_SWITCH2_FLASH_FACTORY_LEFT 0x130a8u
#define TB_SWITCH2_FLASH_FACTORY_RIGHT 0x130e8u
#define TB_SWITCH2_FLASH_USER_LEFT 0x1fc040u
#define TB_SWITCH2_FLASH_USER_RIGHT 0x1fc080u
/* Bytes of a stick's calibration, and of a player's, which has a magic
 * number first. */
#define TB_SWITCH2_CALIBRATION_SIZE 9u
#define TB_SWITCH2_USER_CALIBRATION_SIZE 11u

/* A stick's raw values: 12 bits, from 0 to 4095. */
#define TB_SWITCH2_STICK_CENTER 2048
/* How far a stick reaches from its center when the pad's calibration is
 * not read: the factory calibrations published for these pads put it
 * between 1500 and 1600, so a stick reaches 1 a little before its edge
 * rather than never. */
#define TB_SWITCH2_STICK_DEFAULT_EXTENT 1500

/* One axis of a stick, in the stick's raw values: its center, and how far
 * it reaches below and above it. */
typedef struct tb_switch2_axis
{
    int32_t neutral;
    int32_t negative;
    int32_t positive;
} tb_switch2_axis;

/* A stick's calibration. */
typedef struct tb_switch2_stick
{
    tb_switch2_axis x;
    tb_switch2_axis y;
} tb_switch2_stick;

/* A pad the driver drives: the driver's state. */
typedef struct tb_switch2
{
    tb_switch2_stick sticks[2]; /* Left, right. */
    uint8_t sequence;           /* Of the rumble reports, from 0 to 15. */
    bool configured;            /* The vendor channel took the setup commands. */
} tb_switch2;

/* The Pro Controller's driver. */
extern const tb_hid_driver tb_switch2_driver;

/* Make a command: its header and `length` bytes of data. Returns its
 * length, or 0 when it does not fit `capacity`. */
size_t tb_switch2_command(uint8_t command, uint8_t subcommand, const uint8_t *data, size_t length,
                          uint8_t *out, size_t capacity);

/* Whether a reply answers a command and says it was done. */
bool tb_switch2_reply_ok(const uint8_t *reply, size_t length, uint8_t command, uint8_t subcommand);

/* Read a stick's calibration, 9 bytes of packed 12-bit values; false, and
 * `stick` unchanged, for flash that was never written. */
bool tb_switch2_parse_calibration(const uint8_t *data, tb_switch2_stick *stick);

/* An axis's raw value, from -1 to 1. */
float tb_switch2_axis_value(const tb_switch2_axis *axis, int32_t raw);

#endif
