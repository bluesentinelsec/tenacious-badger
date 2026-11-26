/* Windows raw HID gamepads: the pads that XInput does not read and a
 * protocol driver of src/platform/shared knows, as the Switch 2 Pro
 * Controller and the DualSense. cfgmgr32.dll, hid.dll and winusb.dll are
 * loaded at run time from the system directory; never linked.
 *
 * **Finding pads.** The HID device interfaces are listed every half second,
 * which takes about a tenth of a millisecond, and a pad that came is opened
 * at the next listing. Only interfaces whose path names the vendor and
 * product of a pad a driver knows are opened: over USB as
 * `VID_054C&PID_0CE6`, over Bluetooth, whose HID interfaces are under the
 * HID service's class `{00001124-...}`, as `VID&0002054C_PID&0CE6`.
 * XInput's own HID collections, whose paths have `IG_` in them, are never
 * read here, so that no pad is seen twice, and neither are the pads Steam
 * names in SDL_GAMECONTROLLER_IGNORE_DEVICES. A pad that goes is closed
 * when a read of it fails.
 *
 * **One pad, two ways.** A pad connected over USB and Bluetooth at once has
 * two HID interfaces. A driver that knows a pad's identity, as the
 * DualSense's knows its Bluetooth address, says so once the pad is started;
 * an interface whose pad is open already through another is closed again
 * and passed over while that one stays open. USB interfaces are opened
 * before Bluetooth ones found by the same listing, so a pad found both ways
 * at once is read over its cable.
 *
 * **Reading.** Each pad has one overlapped read outstanding, of the length
 * the device's descriptor gives its longest input report; a poll takes
 * every report that has come since the last, without waiting, and the last
 * one read is the pad's state. Output reports are written at the length the
 * descriptor gives too, a shorter report followed by zeros.
 *
 * **Setting a pad up.** A driver may send commands on the pad's vendor
 * channel, as the Switch 2 controllers need: the USB interface beside the
 * HID one, bound to WinUSB by the pad's own descriptors, and found by the
 * device container both interfaces are in. It is opened while the driver
 * starts the pad, which blocks the poll for the replies, and closed after,
 * so that other programs can have it. WinUSB lets one program have it at a
 * time: when Steam has it, the driver goes on without it.
 *
 * **Rumble.** A report that runs the motors is written to the pad's HID
 * interface. A pad whose reports last only a few milliseconds, as the
 * Switch 2 controllers', has them sent again by a thread of the backend's
 * while its motors run, and never past the time the rumble was asked for,
 * so the pad stops even when the game stops polling. */
#define _CRT_SECURE_NO_WARNINGS /* getenv returns borrowed storage by design. */

#include "gamepad_rawhid.h"

#include "badger/assert.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/str.h"
#include "badger/thread.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

/* The variable in which Steam names the pads it stands in for. */
#define TB_HID_IGNORE_VARIABLE "SDL_GAMECONTROLLER_IGNORE_DEVICES"

/* How often a list of interfaces is asked for again when it grew between
 * asking its size and asking for it. */
#define TB_HID_LIST_ATTEMPTS 4

/* Characters of a device's instance ID. */
#define TB_HID_INSTANCE_CAPACITY 256

/* The direction bit of a USB endpoint's address: set for IN. */
#define TB_HID_ENDPOINT_IN 0x80u

/* Hex digits of a vendor or product in a path. */
#define TB_HID_ID_DIGITS 4

#define TB_HID_NANOSECONDS_PER_SECOND 1e9

/* Where a HID interface's path names the Bluetooth HID service, whose
 * class is {00001124-0000-1000-8000-00805F9B34FB}. */
static const wchar_t tb_hid_bluetooth_service[] = L"{00001124-";

/* GUID_DEVINTERFACE_HID. */
static const GUID tb_hid_interface_class = {
    0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

/* DEVPKEY_Device_InstanceId and DEVPKEY_Device_ContainerId, from
 * devpkey.h, which defines them only where INITGUID is. */
static const DEVPROPKEY tb_hid_instance_key = {
    {0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};
static const DEVPROPKEY tb_hid_container_key = {
    {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

struct tb_hid_gamepads;

typedef struct tb_hid_pad
{
    bool open;
    struct tb_hid_gamepads *owner;
    wchar_t *path; /* The HID interface's, as the listing gave it. */
    bool bluetooth;
    HANDLE file;
    bool writable; /* Opened for writing, with output reports, as rumble needs. */
    HANDLE read_event;
    HANDLE write_event;
    OVERLAPPED read;
    bool reading; /* `read` is outstanding. */
    /* The lengths of the device's longest reports, from its descriptor;
     * an input report's buffer, and an output report's, zeros after the
     * report. */
    size_t input_length;
    size_t output_length;
    size_t feature_length;
    uint8_t *buffer;
    uint8_t *output;
    uint64_t identity; /* As its driver gives it; 0 for not known. */
    /* The vendor channel, while the driver starts the pad. */
    HANDLE usb_file;
    WINUSB_INTERFACE_HANDLE usb;
    UCHAR pipe_in;
    UCHAR pipe_out;
    const tb_hid_driver *driver;
    void *driver_state;
    tb_gamepad_pad pad;
    /* Rumble that is sent again and again, under the backend's lock. */
    bool rumbling;
    float low_frequency;
    float high_frequency;
    uint64_t until;     /* The clock's nanoseconds at which it stops. */
    uint64_t next_send; /* When its report is next due. */
} tb_hid_pad;

/* An interface that is not opened while its pad is open through another:
 * its path, and the pad's identity. */
typedef struct tb_hid_duplicate
{
    wchar_t *path; /* NULL for none. */
    uint64_t identity;
    bool listed; /* By the listing being looked at. */
} tb_hid_duplicate;

typedef struct tb_hid_gamepads
{
    tb_hid_calls calls;
    tb_library *cfgmgr; /* The libraries; NULL for a test's functions. */
    tb_library *hid;
    tb_library *winusb;
    char *ignored; /* NULL for none. */
    tb_gamepad_clock *clock;
    bool rumble_thread;
    uint64_t next_scan;
    uint64_t serial;
    tb_hid_pad pads[TB_GAMEPAD_SLOT_COUNT];
    tb_hid_duplicate duplicates[TB_GAMEPAD_SLOT_COUNT];
    /* Guards the pads against the rumble thread. */
    tb_mutex lock;
    tb_condition wake;
    tb_thread thread;
    bool thread_running;
    bool quit;
} tb_hid_gamepads;

/* ---------------------------------------------------------------------------
 * Paths and interfaces.
 * ------------------------------------------------------------------------- */

/* Where `pattern`, of upper-case letters, is in `text`, in either case; NULL
 * for nowhere. */
static const wchar_t *tb_hid_find(const wchar_t *text, const wchar_t *pattern)
{
    const size_t length = wcslen(pattern);

    for (const wchar_t *at = text; *at != L'\0'; at++)
    {
        size_t matched = 0;
        while (matched < length && at[matched] != L'\0' &&
               (wchar_t)towupper(at[matched]) == pattern[matched])
        {
            matched++;
        }
        if (matched == length)
        {
            return at;
        }
    }
    return NULL;
}

/* Four hex digits; false when they are not. */
static bool tb_hid_hex(const wchar_t *text, uint16_t *value)
{
    uint32_t result = 0;

    for (size_t index = 0; index < TB_HID_ID_DIGITS; index++)
    {
        const wchar_t digit = (wchar_t)towupper(text[index]);
        uint32_t nibble = 0;
        if (digit >= L'0' && digit <= L'9')
        {
            nibble = (uint32_t)(digit - L'0');
        }
        else if (digit >= L'A' && digit <= L'F')
        {
            nibble = (uint32_t)(digit - L'A') + 10u;
        }
        else
        {
            return false;
        }
        result = (result << 4) | nibble;
    }
    *value = (uint16_t)result;
    return true;
}

/* The vendor and product after a prefix, with `skipped` digits between
 * the vendor's prefix and its own, and the product's prefix right after
 * the vendor. */
static bool tb_hid_ids_after(const wchar_t *path, const wchar_t *vendor_prefix, size_t skipped,
                             const wchar_t *product_prefix, uint16_t *vendor, uint16_t *product)
{
    const wchar_t *const at = tb_hid_find(path, vendor_prefix);
    const wchar_t *vendor_at = NULL;
    const wchar_t *product_at = NULL;
    uint16_t source = 0;

    if (at == NULL)
    {
        return false;
    }
    vendor_at = at + wcslen(vendor_prefix);
    if (skipped != 0 && !tb_hid_hex(vendor_at, &source))
    {
        return false;
    }
    vendor_at += skipped;
    if (!tb_hid_hex(vendor_at, vendor))
    {
        return false;
    }
    product_at = vendor_at + TB_HID_ID_DIGITS;
    if (tb_hid_find(product_at, product_prefix) != product_at)
    {
        return false;
    }
    return tb_hid_hex(product_at + wcslen(product_prefix), product);
}

/* The vendor and product a path names: over USB as `VID_057E&PID_2069`;
 * over Bluetooth as `VID&0002054C_PID&0CE6`, the four digits before the
 * vendor saying whose list of vendors it is from. */
static bool tb_hid_path_ids(const wchar_t *path, uint16_t *vendor, uint16_t *product)
{
    return tb_hid_ids_after(path, L"VID_", 0, L"&PID_", vendor, product) ||
           tb_hid_ids_after(path, L"_VID&", TB_HID_ID_DIGITS, L"_PID&", vendor, product);
}

static bool tb_hid_is_bluetooth(const wchar_t *path)
{
    return tb_hid_find(path, tb_hid_bluetooth_service) != NULL;
}

/* The present interfaces of a class: paths, each ended by a NUL, and the
 * list by another. NULL for none, or when the system would not say; the
 * caller frees it. */
static wchar_t *tb_hid_list(const tb_hid_gamepads *hid, const GUID *interface_class)
{
    GUID listed = *interface_class;

    for (int attempt = 0; attempt < TB_HID_LIST_ATTEMPTS; attempt++)
    {
        ULONG size = 0;
        wchar_t *buffer = NULL;
        CONFIGRET result =
            hid->calls.list_size(&size, &listed, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result != CR_SUCCESS || size <= 1)
        {
            return NULL;
        }
        buffer = tb_calloc(size, sizeof(*buffer));
        result = hid->calls.list(&listed, NULL, buffer, size, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result == CR_SUCCESS)
        {
            /* The list's end, even when the system wrote less than it said. */
            buffer[size - 1] = L'\0';
            if (size >= 2)
            {
                buffer[size - 2] = L'\0';
            }
            return buffer;
        }
        tb_free(buffer);
        if (result != CR_BUFFER_SMALL)
        {
            return NULL;
        }
    }
    return NULL;
}

/* The device container an interface's device is in: every interface of one
 * physical device shares it. */
static bool tb_hid_container(const tb_hid_gamepads *hid, const wchar_t *path, GUID *container)
{
    wchar_t instance[TB_HID_INSTANCE_CAPACITY] = {0};
    ULONG size = sizeof(instance) - sizeof(instance[0]);
    DEVPROPTYPE type = 0;
    DEVINST node = 0;

    if (hid->calls.interface_property(path, &tb_hid_instance_key, &type, (PBYTE)instance, &size,
                                      0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING)
    {
        return false;
    }
    if (hid->calls.locate(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
    {
        return false;
    }
    size = sizeof(*container);
    return hid->calls.node_property(node, &tb_hid_container_key, &type, (PBYTE)container, &size,
                                    0) == CR_SUCCESS &&
           type == DEVPROP_TYPE_GUID && size == sizeof(*container);
}

/* ---------------------------------------------------------------------------
 * A pad's vendor channel.
 * ------------------------------------------------------------------------- */

/* Open a WinUSB interface and find its bulk pipes. */
static bool tb_hid_open_usb(tb_hid_gamepads *hid, tb_hid_pad *pad, const wchar_t *path)
{
    USB_INTERFACE_DESCRIPTOR descriptor = {0};
    WINUSB_INTERFACE_HANDLE usb = NULL;
    ULONG timeout = TB_HID_COMMAND_TIMEOUT_MILLISECONDS;
    UCHAR pipe_in = 0;
    UCHAR pipe_out = 0;
    /* Another program that has the interface makes this fail. */
    const HANDLE file = hid->calls.create_file(
        path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);

    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    if (!hid->calls.usb_initialize(file, &usb))
    {
        goto close_file;
    }
    if (!hid->calls.usb_query_interface(usb, 0, &descriptor))
    {
        goto free_usb;
    }
    for (UCHAR index = 0; index < descriptor.bNumEndpoints; index++)
    {
        WINUSB_PIPE_INFORMATION pipe = {0};
        if (!hid->calls.usb_query_pipe(usb, 0, index, &pipe) || pipe.PipeType != UsbdPipeTypeBulk)
        {
            continue;
        }
        if ((pipe.PipeId & TB_HID_ENDPOINT_IN) != 0)
        {
            pipe_in = pipe.PipeId;
        }
        else
        {
            pipe_out = pipe.PipeId;
        }
    }
    if (pipe_in == 0 || pipe_out == 0)
    {
        goto free_usb;
    }
    (void)hid->calls.usb_set_pipe_policy(usb, pipe_in, PIPE_TRANSFER_TIMEOUT, sizeof(timeout),
                                         &timeout);
    (void)hid->calls.usb_set_pipe_policy(usb, pipe_out, PIPE_TRANSFER_TIMEOUT, sizeof(timeout),
                                         &timeout);
    pad->usb_file = file;
    pad->usb = usb;
    pad->pipe_in = pipe_in;
    pad->pipe_out = pipe_out;
    return true;

free_usb:
    (void)hid->calls.usb_free(usb);
close_file:
    (void)hid->calls.close_handle(file);
    return false;
}

/* Open the vendor channel of a pad whose driver has one: the interface of
 * its class in the same device container as the pad's HID interface. */
static void tb_hid_open_channel(tb_hid_gamepads *hid, tb_hid_pad *pad)
{
    static const tb_hid_guid none = {0};
    const tb_hid_guid *const channel = &pad->driver->channel_class;
    GUID channel_class = {0};
    GUID container = {0};
    wchar_t *list = NULL;

    if (memcmp(channel, &none, sizeof(none)) == 0 || !tb_hid_container(hid, pad->path, &container))
    {
        return;
    }
    channel_class.Data1 = channel->data1;
    channel_class.Data2 = channel->data2;
    channel_class.Data3 = channel->data3;
    memcpy(channel_class.Data4, channel->data4, sizeof(channel_class.Data4));
    list = tb_hid_list(hid, &channel_class);
    if (list == NULL)
    {
        return;
    }
    for (const wchar_t *path = list; *path != L'\0'; path += wcslen(path) + 1)
    {
        GUID other = {0};
        if (tb_hid_container(hid, path, &other) &&
            memcmp(&other, &container, sizeof(container)) == 0 && tb_hid_open_usb(hid, pad, path))
        {
            break;
        }
    }
    tb_free(list);
}

static void tb_hid_close_channel(tb_hid_gamepads *hid, tb_hid_pad *pad)
{
    if (pad->usb == NULL)
    {
        return;
    }
    (void)hid->calls.usb_free(pad->usb);
    (void)hid->calls.close_handle(pad->usb_file);
    pad->usb = NULL;
    pad->usb_file = NULL;
}

/* The link's command: write it to the channel's OUT pipe, read the reply
 * from its IN pipe. */
static size_t tb_hid_command(void *context, const uint8_t *command, size_t length, uint8_t *reply,
                             size_t capacity)
{
    tb_hid_pad *const pad = context;
    const tb_hid_calls *const calls = &pad->owner->calls;
    uint8_t message[TB_HID_REPORT_CAPACITY] = {0};
    ULONG written = 0;
    ULONG read = 0;

    if (pad->usb == NULL || length > sizeof(message) || capacity > TB_HID_REPORT_CAPACITY)
    {
        return 0;
    }
    memcpy(message, command, length);
    if (!calls->usb_write(pad->usb, pad->pipe_out, message, (ULONG)length, &written, NULL) ||
        written != length)
    {
        return 0;
    }
    if (!calls->usb_read(pad->usb, pad->pipe_in, reply, (ULONG)capacity, &read, NULL) ||
        read > capacity)
    {
        return 0;
    }
    return read;
}

/* The link's output: an overlapped write to the HID interface, waited for
 * a little. */
static bool tb_hid_output(void *context, const uint8_t *report, size_t length)
{
    tb_hid_pad *const pad = context;
    const tb_hid_calls *const calls = &pad->owner->calls;
    OVERLAPPED overlapped = {.hEvent = pad->write_event};
    DWORD written = 0;

    if (!pad->writable || length > pad->output_length)
    {
        return false;
    }
    memset(pad->output, 0, pad->output_length);
    memcpy(pad->output, report, length);
    if (!calls->write_file(pad->file, pad->output, (DWORD)pad->output_length, NULL, &overlapped) &&
        GetLastError() != ERROR_IO_PENDING)
    {
        return false;
    }
    if (calls->overlapped_result(pad->file, &overlapped, &written,
                                 TB_HID_WRITE_TIMEOUT_MILLISECONDS, FALSE))
    {
        return written == pad->output_length;
    }
    if (GetLastError() == WAIT_TIMEOUT)
    {
        /* The write must be over before `overlapped` goes. */
        (void)calls->cancel_io(pad->file, &overlapped);
        (void)calls->overlapped_result(pad->file, &overlapped, &written, INFINITE, FALSE);
    }
    return false;
}

/* The link's feature: HidD_GetFeature, into a buffer of the length the
 * descriptor gives the longest. */
static size_t tb_hid_feature(void *context, uint8_t id, uint8_t *report, size_t capacity)
{
    tb_hid_pad *const pad = context;
    const tb_hid_calls *const calls = &pad->owner->calls;
    uint8_t *buffer = NULL;
    size_t length = 0;

    if (pad->feature_length == 0)
    {
        return 0;
    }
    buffer = tb_calloc(pad->feature_length, 1);
    buffer[0] = id;
    if (calls->get_feature(pad->file, buffer, (ULONG)pad->feature_length))
    {
        length = pad->feature_length < capacity ? pad->feature_length : capacity;
        memcpy(report, buffer, length);
    }
    tb_free(buffer);
    return length;
}

/* ---------------------------------------------------------------------------
 * Pads.
 * ------------------------------------------------------------------------- */

static void tb_hid_close_pad(tb_hid_gamepads *hid, tb_hid_pad *pad)
{
    DWORD transferred = 0;

    if (pad->reading)
    {
        /* The read must be over before its buffer goes. */
        (void)hid->calls.cancel_io(pad->file, &pad->read);
        (void)hid->calls.overlapped_result(pad->file, &pad->read, &transferred, INFINITE, FALSE);
    }
    (void)hid->calls.close_handle(pad->read_event);
    (void)hid->calls.close_handle(pad->write_event);
    (void)hid->calls.close_handle(pad->file);
    tb_free(pad->driver_state);
    tb_free(pad->buffer);
    tb_free(pad->output);
    tb_free(pad->path);
    *pad = (tb_hid_pad){0};
}

/* Whether a pad of an identity is open, other than `except`. */
static bool tb_hid_identity_open(const tb_hid_gamepads *hid, uint64_t identity,
                                 const tb_hid_pad *except)
{
    if (identity == 0)
    {
        return false;
    }
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        const tb_hid_pad *const pad = &hid->pads[slot];
        if (pad != except && pad->open && pad->identity == identity)
        {
            return true;
        }
    }
    return false;
}

static void tb_hid_forget(tb_hid_duplicate *duplicate)
{
    tb_free(duplicate->path);
    *duplicate = (tb_hid_duplicate){0};
}

/* Remember an interface whose pad is open through another. With no room,
 * it is started again at the next listing, and closed again. */
static void tb_hid_remember(tb_hid_gamepads *hid, const wchar_t *path, uint64_t identity)
{
    const size_t length = wcslen(path) + 1;

    for (size_t index = 0; index < TB_GAMEPAD_SLOT_COUNT; index++)
    {
        tb_hid_duplicate *const duplicate = &hid->duplicates[index];
        if (duplicate->path == NULL)
        {
            duplicate->path = tb_calloc(length, sizeof(wchar_t));
            memcpy(duplicate->path, path, length * sizeof(wchar_t));
            duplicate->identity = identity;
            duplicate->listed = true;
            return;
        }
    }
}

/* Whether an interface is one whose pad is open through another; an
 * interface whose pad is no longer open is forgotten, to be opened. */
static bool tb_hid_is_duplicate(tb_hid_gamepads *hid, const wchar_t *path)
{
    for (size_t index = 0; index < TB_GAMEPAD_SLOT_COUNT; index++)
    {
        tb_hid_duplicate *const duplicate = &hid->duplicates[index];
        if (duplicate->path == NULL || wcscmp(duplicate->path, path) != 0)
        {
            continue;
        }
        if (tb_hid_identity_open(hid, duplicate->identity, NULL))
        {
            duplicate->listed = true;
            return true;
        }
        tb_hid_forget(duplicate);
        return false;
    }
    return false;
}

static bool tb_hid_is_open(const tb_hid_gamepads *hid, const wchar_t *path)
{
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        if (hid->pads[slot].open && wcscmp(hid->pads[slot].path, path) == 0)
        {
            return true;
        }
    }
    return false;
}

static tb_hid_pad *tb_hid_free_pad(tb_hid_gamepads *hid)
{
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        if (!hid->pads[slot].open)
        {
            return &hid->pads[slot];
        }
    }
    return NULL;
}

/* Open a HID interface for reading and, where the system allows it,
 * writing. */
static HANDLE tb_hid_open_file(const tb_hid_gamepads *hid, const wchar_t *path, bool *writable)
{
    HANDLE file = hid->calls.create_file(path, GENERIC_READ | GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                         FILE_FLAG_OVERLAPPED, NULL);

    *writable = file != INVALID_HANDLE_VALUE;
    if (file == INVALID_HANDLE_VALUE)
    {
        file = hid->calls.create_file(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    }
    return file;
}

/* The lengths of a device's longest reports, from its descriptor; false
 * for a device that has no input reports, or reports longer than the
 * backend reads. */
static bool tb_hid_lengths(const tb_hid_gamepads *hid, HANDLE file, HIDP_CAPS *caps)
{
    PHIDP_PREPARSED_DATA data = NULL;
    bool read = false;

    if (!hid->calls.preparsed(file, &data))
    {
        return false;
    }
    read = hid->calls.caps(data, caps) == HIDP_STATUS_SUCCESS;
    (void)hid->calls.free_preparsed(data);
    return read && caps->InputReportByteLength > 0 &&
           caps->InputReportByteLength <= TB_HID_REPORT_LIMIT &&
           caps->OutputReportByteLength <= TB_HID_REPORT_LIMIT &&
           caps->FeatureReportByteLength <= TB_HID_REPORT_LIMIT;
}

/* Start a pad's driver, through its vendor channel while it is open. */
static void tb_hid_start_pad(tb_hid_gamepads *hid, tb_hid_pad *pad)
{
    const tb_hid_link link = {.context = pad,
                              .bluetooth = pad->bluetooth,
                              .command = tb_hid_command,
                              .output = tb_hid_output,
                              .feature = tb_hid_feature};

    pad->driver_state = tb_calloc(1, pad->driver->state_size);
    tb_hid_open_channel(hid, pad);
    pad->driver->start(pad->driver_state, &link);
    tb_hid_close_channel(hid, pad);
    pad->identity = pad->driver->identity(pad->driver_state);
    hid->serial++;
    pad->pad.serial = hid->serial;
    pad->pad.state = (tb_gamepad_state){.connected = true};
    pad->pad.info.capabilities = pad->driver->capabilities;
    if (!pad->writable)
    {
        pad->pad.info.capabilities &= ~(uint32_t)TB_GAMEPAD_CAPABILITY_RUMBLE;
    }
    tb_gamepad_copy_name(pad->pad.info.name, pad->driver->name, tb_str_length(pad->driver->name));
}

/* Open a pad of a driver's in a free slot, if the system has one. */
static void tb_hid_open_pad(tb_hid_gamepads *hid, const wchar_t *path, const tb_hid_driver *driver)
{
    tb_hid_pad *const pad = tb_hid_free_pad(hid);
    HIDD_ATTRIBUTES attributes = {.Size = sizeof(attributes)};
    HIDP_CAPS caps = {0};
    bool writable = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE read_event = NULL;
    HANDLE write_event = NULL;
    const size_t length = wcslen(path) + 1;

    if (pad == NULL)
    {
        return;
    }
    file = tb_hid_open_file(hid, path, &writable);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    /* The device's own IDs, which the path only named. */
    if (!hid->calls.attributes(file, &attributes) ||
        tb_hid_driver_find(attributes.VendorID, attributes.ProductID) != driver ||
        !tb_hid_lengths(hid, file, &caps))
    {
        goto close_file;
    }
    read_event = hid->calls.create_event(NULL, TRUE, FALSE, NULL);
    write_event = hid->calls.create_event(NULL, TRUE, FALSE, NULL);
    if (read_event == NULL || write_event == NULL)
    {
        goto close_events;
    }
    *pad = (tb_hid_pad){.open = true,
                        .owner = hid,
                        .path = tb_calloc(length, sizeof(wchar_t)),
                        .bluetooth = tb_hid_is_bluetooth(path),
                        .file = file,
                        .writable = writable && caps.OutputReportByteLength > 0,
                        .read_event = read_event,
                        .write_event = write_event,
                        .input_length = caps.InputReportByteLength,
                        .output_length = caps.OutputReportByteLength,
                        .feature_length = caps.FeatureReportByteLength,
                        .buffer = tb_calloc(caps.InputReportByteLength, 1),
                        .output = tb_calloc(caps.OutputReportByteLength + 1u, 1),
                        .driver = driver};
    memcpy(pad->path, path, length * sizeof(wchar_t));
    pad->pad.info.vendor = attributes.VendorID;
    pad->pad.info.product = attributes.ProductID;
    tb_hid_start_pad(hid, pad);
    if (tb_hid_identity_open(hid, pad->identity, pad))
    {
        tb_hid_remember(hid, path, pad->identity);
        tb_hid_close_pad(hid, pad);
    }
    return;

close_events:
    if (read_event != NULL)
    {
        (void)hid->calls.close_handle(read_event);
    }
    if (write_event != NULL)
    {
        (void)hid->calls.close_handle(write_event);
    }
close_file:
    (void)hid->calls.close_handle(file);
}

/* Look at an interface the listing gave: open it if it is a pad a driver
 * knows that is not open yet. */
static void tb_hid_consider(tb_hid_gamepads *hid, const wchar_t *path)
{
    uint16_t vendor = 0;
    uint16_t product = 0;
    const tb_hid_driver *driver = NULL;

    /* XInput's pads, which XInput reads. */
    if (tb_hid_find(path, L"&IG_") != NULL)
    {
        return;
    }
    if (!tb_hid_path_ids(path, &vendor, &product))
    {
        return;
    }
    driver = tb_hid_driver_find(vendor, product);
    if (driver == NULL || tb_gamepad_ignored(hid->ignored, vendor, product) ||
        tb_hid_is_open(hid, path) || tb_hid_is_duplicate(hid, path))
    {
        return;
    }
    tb_hid_open_pad(hid, path, driver);
}

static void tb_hid_scan(tb_hid_gamepads *hid)
{
    wchar_t *const list = tb_hid_list(hid, &tb_hid_interface_class);

    for (size_t index = 0; index < TB_GAMEPAD_SLOT_COUNT; index++)
    {
        hid->duplicates[index].listed = false;
    }
    /* USB first, then Bluetooth, so that a pad found both ways at once is
     * read over its cable. */
    for (size_t pass = 0; pass < 2 && list != NULL; pass++)
    {
        for (const wchar_t *path = list; *path != L'\0'; path += wcslen(path) + 1)
        {
            if (tb_hid_is_bluetooth(path) == (pass == 1))
            {
                tb_hid_consider(hid, path);
            }
        }
    }
    /* An interface that went is forgotten. */
    for (size_t index = 0; index < TB_GAMEPAD_SLOT_COUNT; index++)
    {
        if (hid->duplicates[index].path != NULL && !hid->duplicates[index].listed)
        {
            tb_hid_forget(&hid->duplicates[index]);
        }
    }
    tb_free(list);
}

/* Take every report a pad has sent since the last poll. A read that fails
 * is a pad that went. */
static void tb_hid_read_pad(tb_hid_gamepads *hid, tb_hid_pad *pad)
{
    for (size_t count = 0; count < TB_HID_READS_PER_POLL; count++)
    {
        DWORD received = 0;
        if (!pad->reading)
        {
            pad->read = (OVERLAPPED){.hEvent = pad->read_event};
            if (!hid->calls.read_file(pad->file, pad->buffer, (DWORD)pad->input_length, NULL,
                                      &pad->read) &&
                GetLastError() != ERROR_IO_PENDING)
            {
                tb_hid_close_pad(hid, pad);
                return;
            }
            pad->reading = true;
        }
        if (!hid->calls.overlapped_result(pad->file, &pad->read, &received, 0, FALSE))
        {
            const DWORD error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE || error == WAIT_TIMEOUT)
            {
                return;
            }
            pad->reading = false;
            tb_hid_close_pad(hid, pad);
            return;
        }
        pad->reading = false;
        (void)pad->driver->parse(pad->driver_state, pad->buffer, received, &pad->pad.state);
    }
}

static void tb_hid_poll(void *native, uint64_t now, tb_gamepad_pad *pads)
{
    tb_hid_gamepads *const hid = native;

    tb_mutex_lock(&hid->lock);
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        if (hid->pads[slot].open)
        {
            tb_hid_read_pad(hid, &hid->pads[slot]);
        }
    }
    if (now >= hid->next_scan)
    {
        hid->next_scan = now + TB_HID_SCAN_NANOSECONDS;
        tb_hid_scan(hid);
    }
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        pads[slot] = hid->pads[slot].open ? hid->pads[slot].pad : (tb_gamepad_pad){0};
    }
    tb_mutex_unlock(&hid->lock);
}

/* ---------------------------------------------------------------------------
 * Rumble.
 * ------------------------------------------------------------------------- */

static void tb_hid_send_rumble(tb_hid_pad *pad, float low_frequency, float high_frequency)
{
    uint8_t report[TB_HID_REPORT_CAPACITY] = {0};
    const size_t length = pad->driver->rumble(pad->driver_state, low_frequency, high_frequency,
                                              report, sizeof(report));

    (void)tb_hid_output(pad, report, length);
}

/* Send what is due, stop what is over, and say when the next report is
 * due: 0 for none. Under the lock. */
static uint64_t tb_hid_refresh_locked(tb_hid_gamepads *hid, uint64_t now)
{
    uint64_t earliest = 0;

    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        tb_hid_pad *const pad = &hid->pads[slot];
        uint64_t due = 0;
        if (!pad->open || !pad->rumbling)
        {
            continue;
        }
        if (now >= pad->until)
        {
            tb_hid_send_rumble(pad, 0.0f, 0.0f);
            pad->rumbling = false;
            continue;
        }
        if (now >= pad->next_send)
        {
            tb_hid_send_rumble(pad, pad->low_frequency, pad->high_frequency);
            pad->next_send = now + pad->driver->rumble_period;
        }
        due = pad->next_send < pad->until ? pad->next_send : pad->until;
        if (earliest == 0 || due < earliest)
        {
            earliest = due;
        }
    }
    return earliest;
}

void tb_hid_gamepads_refresh(void *native, uint64_t now)
{
    tb_hid_gamepads *const hid = native;

    tb_mutex_lock(&hid->lock);
    (void)tb_hid_refresh_locked(hid, now);
    tb_mutex_unlock(&hid->lock);
}

/* The rumble thread: sends the reports that are due, and sleeps until the
 * next, or until a rumble starts. */
static void tb_hid_rumble_main(void *argument)
{
    tb_hid_gamepads *const hid = argument;

    tb_mutex_lock(&hid->lock);
    while (!hid->quit)
    {
        const uint64_t now = hid->clock();
        const uint64_t next = tb_hid_refresh_locked(hid, now);
        if (next == 0)
        {
            tb_condition_wait(&hid->wake, &hid->lock);
            continue;
        }
        tb_mutex_unlock(&hid->lock);
        tb_platform_sleep(next > now ? next - now : 0);
        tb_mutex_lock(&hid->lock);
    }
    tb_mutex_unlock(&hid->lock);
}

static void tb_hid_rumble(void *native, size_t slot, float low_frequency, float high_frequency,
                          double seconds)
{
    tb_hid_gamepads *const hid = native;
    tb_hid_pad *pad = NULL;
    uint64_t now = 0;

    TB_ASSERT(slot < TB_GAMEPAD_SLOT_COUNT);
    tb_mutex_lock(&hid->lock);
    pad = &hid->pads[slot];
    TB_ASSERT_MSG(pad->open, "a slot with a pad");
    now = hid->clock();
    tb_hid_send_rumble(pad, low_frequency, high_frequency);
    pad->rumbling =
        pad->driver->rumble_period != 0 && (low_frequency > 0.0f || high_frequency > 0.0f);
    pad->low_frequency = low_frequency;
    pad->high_frequency = high_frequency;
    pad->until = now + (uint64_t)(seconds * TB_HID_NANOSECONDS_PER_SECOND);
    pad->next_send = now + pad->driver->rumble_period;
    if (pad->rumbling && hid->rumble_thread)
    {
        if (!hid->thread_running)
        {
            tb_thread_create_with_options(&hid->thread,
                                          &(tb_thread_options){.name = "gamepad rumble"},
                                          tb_hid_rumble_main, hid);
            hid->thread_running = true;
        }
        tb_condition_wake_one(&hid->wake);
    }
    tb_mutex_unlock(&hid->lock);
}

/* Show on a pad which player it is, where its driver can. */
static void tb_hid_player(void *native, size_t slot, size_t player)
{
    tb_hid_gamepads *const hid = native;
    tb_hid_pad *pad = NULL;

    TB_ASSERT(slot < TB_GAMEPAD_SLOT_COUNT);
    tb_mutex_lock(&hid->lock);
    pad = &hid->pads[slot];
    if (pad->open)
    {
        uint8_t report[TB_HID_REPORT_CAPACITY] = {0};
        const size_t length =
            pad->driver->player(pad->driver_state, player, report, sizeof(report));
        if (length != 0)
        {
            (void)tb_hid_output(pad, report, length);
        }
    }
    tb_mutex_unlock(&hid->lock);
}

/* ---------------------------------------------------------------------------
 * The backend.
 * ------------------------------------------------------------------------- */

static void tb_hid_close(void *native)
{
    tb_hid_gamepads *const hid = native;

    if (hid->thread_running)
    {
        tb_mutex_lock(&hid->lock);
        hid->quit = true;
        tb_condition_wake_all(&hid->wake);
        tb_mutex_unlock(&hid->lock);
        tb_thread_join(&hid->thread);
    }
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        if (hid->pads[slot].open)
        {
            tb_hid_close_pad(hid, &hid->pads[slot]);
        }
        tb_hid_forget(&hid->duplicates[slot]);
    }
    tb_library_close(hid->winusb);
    tb_library_close(hid->hid);
    tb_library_close(hid->cfgmgr);
    tb_free(hid->ignored);
    tb_free(hid);
}

const tb_gamepad_ops tb_hid_gamepad_ops = {
    .close = tb_hid_close,
    .poll = tb_hid_poll,
    .rumble = tb_hid_rumble,
    .player = tb_hid_player,
};

void *tb_hid_gamepads_make(const tb_hid_calls *calls, const char *ignored, tb_gamepad_clock *clock,
                           bool rumble_thread)
{
    tb_hid_gamepads *const hid = tb_calloc(1, sizeof(*hid));

    TB_ASSERT(calls != NULL && clock != NULL);
    hid->calls = *calls;
    hid->clock = clock;
    hid->rumble_thread = rumble_thread;
    if (ignored != NULL)
    {
        hid->ignored = tb_str_dup(NULL, ignored);
    }
    return hid;
}

/* Load a system library and bind its functions. */
static bool tb_hid_load(const char *name, const tb_library_binding *bindings, size_t count,
                        tb_library **library, tb_gamepad_error *error)
{
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;

    if (tb_library_open(name, TB_LIBRARY_SEARCH_SYSTEM, library, &library_error) != TB_LIBRARY_OK)
    {
        /* coverage-off: every Windows since 8 has the three */
        (void)tb_gamepad_fail(error, TB_GAMEPAD_UNAVAILABLE, "no raw HID: %s",
                              library_error.message);
        return false;
        /* coverage-on */
    }
    if (tb_library_bind(*library, bindings, count, &library_error) != TB_LIBRARY_OK)
    {
        /* coverage-off: every Windows since 8 exports these */
        (void)tb_gamepad_fail(error, TB_GAMEPAD_UNAVAILABLE, "no raw HID: %s",
                              library_error.message);
        return false;
        /* coverage-on */
    }
    return true;
}

tb_gamepad_result tb_hid_gamepads_open(void **native, tb_gamepad_error *error)
{
    tb_library *cfgmgr = NULL;
    tb_library *hid_library = NULL;
    tb_library *winusb = NULL;
    tb_library_function cfgmgr_functions[5] = {0};
    tb_library_function hid_functions[5] = {0};
    tb_library_function winusb_functions[7] = {0};
    const tb_library_binding cfgmgr_bindings[5] = {
        {"CM_Get_Device_Interface_List_SizeW", &cfgmgr_functions[0], true},
        {"CM_Get_Device_Interface_ListW", &cfgmgr_functions[1], true},
        {"CM_Get_Device_Interface_PropertyW", &cfgmgr_functions[2], true},
        {"CM_Locate_DevNodeW", &cfgmgr_functions[3], true},
        {"CM_Get_DevNode_PropertyW", &cfgmgr_functions[4], true},
    };
    const tb_library_binding hid_bindings[5] = {
        {"HidD_GetAttributes", &hid_functions[0], true},
        {"HidD_GetPreparsedData", &hid_functions[1], true},
        {"HidD_FreePreparsedData", &hid_functions[2], true},
        {"HidP_GetCaps", &hid_functions[3], true},
        {"HidD_GetFeature", &hid_functions[4], true},
    };
    const tb_library_binding winusb_bindings[7] = {
        {"WinUsb_Initialize", &winusb_functions[0], true},
        {"WinUsb_Free", &winusb_functions[1], true},
        {"WinUsb_QueryInterfaceSettings", &winusb_functions[2], true},
        {"WinUsb_QueryPipe", &winusb_functions[3], true},
        {"WinUsb_SetPipePolicy", &winusb_functions[4], true},
        {"WinUsb_WritePipe", &winusb_functions[5], true},
        {"WinUsb_ReadPipe", &winusb_functions[6], true},
    };
    tb_hid_calls calls = {0};
    tb_hid_gamepads *made = NULL;

    if (!tb_hid_load("cfgmgr32.dll", cfgmgr_bindings, 5, &cfgmgr, error) ||
        !tb_hid_load("hid.dll", hid_bindings, 5, &hid_library, error) ||
        !tb_hid_load("winusb.dll", winusb_bindings, 7, &winusb, error))
    {
        /* coverage-off: every Windows since 8 has the three */
        tb_library_close(winusb);
        tb_library_close(hid_library);
        tb_library_close(cfgmgr);
        return TB_GAMEPAD_UNAVAILABLE;
        /* coverage-on */
    }
    calls = (tb_hid_calls){
        .list_size = (CONFIGRET(WINAPI *)(PULONG, LPGUID, DEVINSTID_W, ULONG))cfgmgr_functions[0],
        .list =
            (CONFIGRET(WINAPI *)(LPGUID, DEVINSTID_W, PZZWSTR, ULONG, ULONG))cfgmgr_functions[1],
        .interface_property = (CONFIGRET(WINAPI *)(LPCWSTR, const DEVPROPKEY *, DEVPROPTYPE *,
                                                   PBYTE, PULONG, ULONG))cfgmgr_functions[2],
        .locate = (CONFIGRET(WINAPI *)(PDEVINST, DEVINSTID_W, ULONG))cfgmgr_functions[3],
        .node_property = (CONFIGRET(WINAPI *)(DEVINST, const DEVPROPKEY *, DEVPROPTYPE *, PBYTE,
                                              PULONG, ULONG))cfgmgr_functions[4],
        .attributes = (BOOLEAN(WINAPI *)(HANDLE, PHIDD_ATTRIBUTES))hid_functions[0],
        .preparsed = (BOOLEAN(WINAPI *)(HANDLE, PHIDP_PREPARSED_DATA *))hid_functions[1],
        .free_preparsed = (BOOLEAN(WINAPI *)(PHIDP_PREPARSED_DATA))hid_functions[2],
        .caps = (NTSTATUS(WINAPI *)(PHIDP_PREPARSED_DATA, PHIDP_CAPS))hid_functions[3],
        .get_feature = (BOOLEAN(WINAPI *)(HANDLE, PVOID, ULONG))hid_functions[4],
        .usb_initialize = (BOOL(WINAPI *)(HANDLE, PWINUSB_INTERFACE_HANDLE))winusb_functions[0],
        .usb_free = (BOOL(WINAPI *)(WINUSB_INTERFACE_HANDLE))winusb_functions[1],
        .usb_query_interface = (BOOL(WINAPI *)(WINUSB_INTERFACE_HANDLE, UCHAR,
                                               PUSB_INTERFACE_DESCRIPTOR))winusb_functions[2],
        .usb_query_pipe = (BOOL(WINAPI *)(WINUSB_INTERFACE_HANDLE, UCHAR, UCHAR,
                                          PWINUSB_PIPE_INFORMATION))winusb_functions[3],
        .usb_set_pipe_policy = (BOOL(WINAPI *)(WINUSB_INTERFACE_HANDLE, UCHAR, ULONG, ULONG,
                                               PVOID))winusb_functions[4],
        .usb_write = (BOOL(WINAPI *)(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG,
                                     LPOVERLAPPED))winusb_functions[5],
        .usb_read = (BOOL(WINAPI *)(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG,
                                    LPOVERLAPPED))winusb_functions[6],
        .create_file = CreateFileW,
        .close_handle = CloseHandle,
        .create_event = CreateEventW,
        .read_file = ReadFile,
        .write_file = WriteFile,
        .overlapped_result = GetOverlappedResultEx,
        .cancel_io = CancelIoEx,
    };
    made = tb_hid_gamepads_make(&calls, getenv(TB_HID_IGNORE_VARIABLE),
                                tb_platform_clock_nanoseconds, true);
    made->cfgmgr = cfgmgr;
    made->hid = hid_library;
    made->winusb = winusb;
    *native = made;
    return TB_GAMEPAD_OK;
}
