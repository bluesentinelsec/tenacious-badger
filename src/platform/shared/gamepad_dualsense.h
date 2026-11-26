/* Private to the HID backends and their tests: the protocol of Sony's
 * DualSense controllers, in standard C. See gamepad_dualsense.c. */
#ifndef TB_PLATFORM_SHARED_GAMEPAD_DUALSENSE_H
#define TB_PLATFORM_SHARED_GAMEPAD_DUALSENSE_H

#include "gamepad_hid.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* USB IDs: Sony's vendor, the DualSense and the DualSense Edge. Over
 * Bluetooth the pads give the same. */
#define TB_DUALSENSE_VENDOR 0x054cu
#define TB_DUALSENSE_PRODUCT 0x0ce6u
#define TB_DUALSENSE_PRODUCT_EDGE 0x0df2u

/* Input reports: over USB the full report; over Bluetooth the simple one,
 * which has the same ID, until the calibration is read, then the full one
 * with a CRC-32. */
#define TB_DUALSENSE_REPORT_USB 0x01u
#define TB_DUALSENSE_REPORT_SIMPLE 0x01u
#define TB_DUALSENSE_REPORT_BLUETOOTH 0x31u
/* Output reports, which carry rumble and the lights. */
#define TB_DUALSENSE_OUTPUT_USB 0x02u
#define TB_DUALSENSE_OUTPUT_BLUETOOTH 0x31u
/* Feature reports: the motion sensors' calibration, which Bluetooth's full
 * reports wait for; the pad's Bluetooth address; its firmware. */
#define TB_DUALSENSE_FEATURE_CALIBRATION 0x05u
#define TB_DUALSENSE_FEATURE_PAIRING 0x09u
#define TB_DUALSENSE_FEATURE_FIRMWARE 0x20u

/* Bytes of the reports, their IDs included: the input reports over USB and
 * Bluetooth, the simple one, the output reports, and the feature reports
 * (over Bluetooth their last four bytes are a CRC-32). */
#define TB_DUALSENSE_USB_SIZE 64u
#define TB_DUALSENSE_BLUETOOTH_SIZE 78u
#define TB_DUALSENSE_SIMPLE_SIZE 10u
#define TB_DUALSENSE_OUTPUT_USB_SIZE 48u
#define TB_DUALSENSE_OUTPUT_BLUETOOTH_SIZE 78u
#define TB_DUALSENSE_CALIBRATION_SIZE 41u
#define TB_DUALSENSE_PAIRING_SIZE 20u
#define TB_DUALSENSE_FIRMWARE_SIZE 64u

/* The first byte of each CRC-32 over Bluetooth: of input, output and
 * feature reports. */
#define TB_DUALSENSE_SEED_INPUT 0xa1u
#define TB_DUALSENSE_SEED_OUTPUT 0xa2u
#define TB_DUALSENSE_SEED_FEATURE 0xa3u

/* Bytes of a Bluetooth address. */
#define TB_DUALSENSE_ADDRESS_SIZE 6u

/* A pad the driver drives: the driver's state. */
typedef struct tb_dualsense
{
    bool bluetooth; /* Reached over Bluetooth, whose reports have CRCs. */
    bool edge;      /* A DualSense Edge, with back buttons. */
    /* The firmware takes the newer flag for its two-motor emulation:
     * every Edge, and a DualSense from update 2.21. */
    bool vibration_v2;
    uint8_t sequence; /* Of the Bluetooth output reports, from 0 to 15. */
    uint64_t address; /* Its Bluetooth address; 0 until it is read. */
} tb_dualsense;

/* The DualSense's driver, and the DualSense Edge's. */
extern const tb_hid_driver tb_dualsense_driver;
extern const tb_hid_driver tb_dualsense_edge_driver;

/* The CRC-32 a Bluetooth report carries in its last four bytes: of its
 * seed and its first `length` bytes. */
uint32_t tb_dualsense_crc(uint8_t seed, const uint8_t *report, size_t length);

/* Whether a Bluetooth report of `size` bytes, at least five, ends in the
 * CRC-32 of its seed and the rest. */
bool tb_dualsense_crc_ok(uint8_t seed, const uint8_t *report, size_t size);

#endif
