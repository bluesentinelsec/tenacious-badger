/* What the Windows raw HID backend does, with the system's functions
 * replaced by devices of the test's own: pads found by their interfaces
 * and set up through their vendor channel, XInput's and Steam's pads left
 * alone, reports read without waiting, pads that go and come, and rumble
 * sent again while it runs and stopped when its time is up. A last test
 * reads a real Switch 2 Pro Controller, when one is plugged in. */
#include "platform/win32/gamepad_rawhid.h"

#include "platform/shared/gamepad_switch2.h"

#include "badger/platform.h"
#include "badger/str.h"

#include "tb_test.h"

#include <string.h>
#include <wchar.h>

#define FAKE_DEVICE_COUNT 3u
#define FAKE_QUEUE 8u
#define FAKE_LIST_CAPACITY 1024u
#define FAKE_MILLISECONDS 1000000u

/* The kinds of handle the fake makes, in the low byte of its value; the
 * device is in the bytes above. */
enum
{
    FAKE_HID_FILE = 1,
    FAKE_USB_FILE = 2,
    FAKE_EVENT = 3,
    FAKE_USB = 4
};

/* NTSTATUS values in an OVERLAPPED's Internal. */
#define FAKE_DONE 0u
#define FAKE_PENDING 0x103u
#define FAKE_CANCELLED 0xc0000120u

typedef struct fake_device
{
    const wchar_t *hid_path;
    const wchar_t *usb_path; /* NULL for none. */
    GUID container;
    uint16_t vendor;
    uint16_t product;
    bool present;
    bool unplugged;   /* Reads fail. */
    bool usb_busy;    /* Another program has the vendor channel. */
    bool read_only;   /* Opening for writing is refused. */
    bool writes_hang; /* A write never ends until it is cancelled. */
    uint8_t queue[FAKE_QUEUE][TB_HID_REPORT_CAPACITY];
    size_t queued;
    size_t taken;
    OVERLAPPED *pending_read;
    uint8_t *pending_buffer;
    OVERLAPPED *pending_write;
    size_t commands;
    uint8_t reply[TB_HID_REPORT_CAPACITY];
    size_t reply_length;
    size_t writes;
    uint8_t last_write[TB_HID_REPORT_CAPACITY];
} fake_device;

typedef struct fake_system
{
    fake_device devices[FAKE_DEVICE_COUNT];
    const wchar_t *extra_paths[4]; /* Other HID interfaces in the list. */
    size_t list_calls;
    bool list_grows_once;
    bool list_fails;
    bool attributes_lie;
    bool events_fail;
    long handles; /* Open handles, made less closed. */
    size_t opens_of_extras;
    size_t usb_frees;
    size_t cancels;
} fake_system;

static fake_system fake = {0};
static uint64_t fake_now = 0;

static uint64_t fake_clock(void)
{
    return fake_now;
}

static HANDLE fake_handle(size_t device, int kind)
{
    return (HANDLE)(uintptr_t)(((device + 1u) << 8) | (unsigned)kind);
}

/* The device a handle is of. The rumble thread calls the fake's
 * functions too, so they check nothing with the test's macros. */
static fake_device *fake_owner(HANDLE handle)
{
    const uintptr_t value = (uintptr_t)handle;
    const size_t device = (size_t)(value >> 8) - 1u;

    return &fake.devices[device % FAKE_DEVICE_COUNT];
}

static const GUID fake_hid_class = {
    0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

/* Write a list of paths, NUL after each and after the list; its length in
 * characters. */
static ULONG fake_build_list(const GUID *interface_class, wchar_t *out)
{
    size_t at = 0;
    const bool hid = memcmp(interface_class, &fake_hid_class, sizeof(GUID)) == 0;

    for (size_t index = 0; index < FAKE_DEVICE_COUNT + TB_TEST_COUNT_OF(fake.extra_paths); index++)
    {
        const wchar_t *path = NULL;
        size_t length = 0;
        if (index < FAKE_DEVICE_COUNT)
        {
            const fake_device *const device = &fake.devices[index];
            if (device->present)
            {
                path = hid ? device->hid_path : device->usb_path;
            }
        }
        else if (hid)
        {
            path = fake.extra_paths[index - FAKE_DEVICE_COUNT];
        }
        if (path == NULL)
        {
            continue;
        }
        length = wcslen(path) + 1;
        if (out != NULL)
        {
            memcpy(out + at, path, length * sizeof(wchar_t));
        }
        at += length;
    }
    if (out != NULL)
    {
        out[at] = L'\0';
    }
    return (ULONG)(at + 1);
}

static CONFIGRET WINAPI fake_list_size(PULONG size, LPGUID interface_class, DEVINSTID_W device,
                                       ULONG flags)
{
    (void)device;
    TB_TEST_EXPECT_HEX(flags, ==, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (fake.list_fails)
    {
        return CR_FAILURE;
    }
    *size = fake_build_list(interface_class, NULL);
    return CR_SUCCESS;
}

static CONFIGRET WINAPI fake_list(LPGUID interface_class, DEVINSTID_W device, PZZWSTR buffer,
                                  ULONG length, ULONG flags)
{
    wchar_t list[FAKE_LIST_CAPACITY] = {0};
    const ULONG needed = fake_build_list(interface_class, list);

    (void)device;
    (void)flags;
    fake.list_calls++;
    if (fake.list_grows_once || needed > length)
    {
        fake.list_grows_once = false;
        return CR_BUFFER_SMALL;
    }
    memcpy(buffer, list, needed * sizeof(wchar_t));
    return CR_SUCCESS;
}

/* A device's instance ID is its interface's path, in the fake. */
static CONFIGRET WINAPI fake_interface_property(LPCWSTR path, const DEVPROPKEY *key,
                                                DEVPROPTYPE *type, PBYTE buffer, PULONG size,
                                                ULONG flags)
{
    const ULONG bytes = (ULONG)((wcslen(path) + 1) * sizeof(wchar_t));

    (void)key;
    (void)flags;
    if (bytes > *size)
    {
        return CR_BUFFER_SMALL;
    }
    memcpy(buffer, path, bytes);
    *type = DEVPROP_TYPE_STRING;
    *size = bytes;
    return CR_SUCCESS;
}

static CONFIGRET WINAPI fake_locate(PDEVINST node, DEVINSTID_W id, ULONG flags)
{
    (void)flags;
    for (size_t index = 0; index < FAKE_DEVICE_COUNT; index++)
    {
        const fake_device *const device = &fake.devices[index];
        if ((device->hid_path != NULL && wcscmp(device->hid_path, id) == 0) ||
            (device->usb_path != NULL && wcscmp(device->usb_path, id) == 0))
        {
            *node = (DEVINST)(index + 1);
            return CR_SUCCESS;
        }
    }
    return CR_NO_SUCH_DEVNODE;
}

static CONFIGRET WINAPI fake_node_property(DEVINST node, const DEVPROPKEY *key, DEVPROPTYPE *type,
                                           PBYTE buffer, PULONG size, ULONG flags)
{
    (void)key;
    (void)flags;
    TB_TEST_REQUIRE_UINT(*size, ==, sizeof(GUID));
    memcpy(buffer, &fake.devices[node - 1].container, sizeof(GUID));
    *type = DEVPROP_TYPE_GUID;
    return CR_SUCCESS;
}

static BOOLEAN WINAPI fake_attributes(HANDLE file, PHIDD_ATTRIBUTES attributes)
{
    const fake_device *const device = fake_owner(file);

    attributes->VendorID = device->vendor;
    attributes->ProductID = fake.attributes_lie ? 0x2009 : device->product;
    return TRUE;
}

static BOOL WINAPI fake_usb_initialize(HANDLE file, PWINUSB_INTERFACE_HANDLE usb)
{
    const uintptr_t device = ((uintptr_t)file >> 8) - 1u;

    *usb = (WINUSB_INTERFACE_HANDLE)fake_handle((size_t)device, FAKE_USB);
    return TRUE;
}

static BOOL WINAPI fake_usb_free(WINUSB_INTERFACE_HANDLE usb)
{
    (void)usb;
    fake.usb_frees++;
    return TRUE;
}

static BOOL WINAPI fake_usb_query_interface(WINUSB_INTERFACE_HANDLE usb, UCHAR alternate,
                                            PUSB_INTERFACE_DESCRIPTOR descriptor)
{
    (void)usb;
    (void)alternate;
    descriptor->bNumEndpoints = 3;
    return TRUE;
}

/* An interrupt pipe, which is passed over, then the bulk IN and OUT. */
static BOOL WINAPI fake_usb_query_pipe(WINUSB_INTERFACE_HANDLE usb, UCHAR alternate, UCHAR index,
                                       PWINUSB_PIPE_INFORMATION pipe)
{
    static const WINUSB_PIPE_INFORMATION pipes[3] = {
        {UsbdPipeTypeInterrupt, 0x83, 64, 4},
        {UsbdPipeTypeBulk, 0x82, 64, 0},
        {UsbdPipeTypeBulk, 0x02, 64, 0},
    };

    (void)usb;
    (void)alternate;
    *pipe = pipes[index];
    return TRUE;
}

static BOOL WINAPI fake_usb_set_pipe_policy(WINUSB_INTERFACE_HANDLE usb, UCHAR pipe, ULONG policy,
                                            ULONG length, PVOID value)
{
    (void)usb;
    (void)length;
    (void)value;
    TB_TEST_EXPECT_HEX(policy, ==, PIPE_TRANSFER_TIMEOUT);
    TB_TEST_EXPECT(pipe == 0x82 || pipe == 0x02);
    return TRUE;
}

/* The pad answers every command as done; a flash read with flash that was
 * never written. */
static BOOL WINAPI fake_usb_write(WINUSB_INTERFACE_HANDLE usb, UCHAR pipe, PUCHAR buffer,
                                  ULONG length, PULONG written, LPOVERLAPPED overlapped)
{
    fake_device *const device = fake_owner((HANDLE)usb);

    TB_TEST_EXPECT_HEX(pipe, ==, 0x02);
    TB_TEST_EXPECT_POINTER(overlapped, ==, NULL);
    device->commands++;
    memset(device->reply, 0, sizeof(device->reply));
    device->reply[0] = buffer[0];
    device->reply[1] = 0x01;
    device->reply[3] = buffer[3];
    device->reply_length = 12;
    if (buffer[0] == TB_SWITCH2_COMMAND_FLASH)
    {
        device->reply[8] = buffer[8];
        memcpy(device->reply + 12, buffer + 12, 4);
        memset(device->reply + 16, 0xff, buffer[8]);
        device->reply_length = 16u + buffer[8];
    }
    *written = length;
    return TRUE;
}

static BOOL WINAPI fake_usb_read(WINUSB_INTERFACE_HANDLE usb, UCHAR pipe, PUCHAR buffer,
                                 ULONG length, PULONG read, LPOVERLAPPED overlapped)
{
    const fake_device *const device = fake_owner((HANDLE)usb);

    (void)overlapped;
    TB_TEST_EXPECT_HEX(pipe, ==, 0x82);
    TB_TEST_REQUIRE_UINT(length, >=, device->reply_length);
    memcpy(buffer, device->reply, device->reply_length);
    *read = (ULONG)device->reply_length;
    return TRUE;
}

static HANDLE WINAPI fake_create_file(LPCWSTR path, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                      DWORD flags, HANDLE template_file)
{
    (void)security;
    (void)template_file;
    TB_TEST_EXPECT_HEX(share, ==, FILE_SHARE_READ | FILE_SHARE_WRITE);
    TB_TEST_EXPECT_HEX(disposition, ==, OPEN_EXISTING);
    TB_TEST_EXPECT((flags & FILE_FLAG_OVERLAPPED) != 0);
    for (size_t index = 0; index < FAKE_DEVICE_COUNT; index++)
    {
        const fake_device *const device = &fake.devices[index];
        if (!device->present)
        {
            continue;
        }
        if (wcscmp(device->hid_path, path) == 0)
        {
            if (device->read_only && (access & GENERIC_WRITE) != 0)
            {
                SetLastError(ERROR_ACCESS_DENIED);
                return INVALID_HANDLE_VALUE;
            }
            fake.handles++;
            return fake_handle(index, FAKE_HID_FILE);
        }
        if (device->usb_path != NULL && wcscmp(device->usb_path, path) == 0)
        {
            if (device->usb_busy)
            {
                SetLastError(ERROR_ACCESS_DENIED);
                return INVALID_HANDLE_VALUE;
            }
            fake.handles++;
            return fake_handle(index, FAKE_USB_FILE);
        }
    }
    fake.opens_of_extras++;
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
}

static BOOL WINAPI fake_close_handle(HANDLE handle)
{
    TB_TEST_EXPECT_POINTER(handle, !=, NULL);
    fake.handles--;
    return TRUE;
}

static HANDLE WINAPI fake_create_event(LPSECURITY_ATTRIBUTES security, BOOL manual, BOOL initial,
                                       LPCWSTR name)
{
    (void)security;
    (void)initial;
    (void)name;
    TB_TEST_EXPECT(manual);
    if (fake.events_fail)
    {
        return NULL;
    }
    fake.handles++;
    return fake_handle(0, FAKE_EVENT);
}

/* Take the next queued report into a read's buffer. */
static bool fake_take(fake_device *device, uint8_t *buffer, DWORD *received)
{
    if (device->taken == device->queued)
    {
        return false;
    }
    memcpy(buffer, device->queue[device->taken % FAKE_QUEUE], TB_HID_REPORT_CAPACITY);
    device->taken++;
    *received = TB_HID_REPORT_CAPACITY;
    return true;
}

static BOOL WINAPI fake_read_file(HANDLE file, LPVOID buffer, DWORD length, LPDWORD read,
                                  LPOVERLAPPED overlapped)
{
    fake_device *const device = fake_owner(file);
    DWORD received = 0;

    TB_TEST_EXPECT_POINTER(read, ==, NULL);
    TB_TEST_REQUIRE_UINT(length, ==, TB_SWITCH2_REPORT_SIZE);
    if (device->unplugged)
    {
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return FALSE;
    }
    if (fake_take(device, buffer, &received))
    {
        overlapped->Internal = FAKE_DONE;
        overlapped->InternalHigh = received;
        return TRUE;
    }
    overlapped->Internal = FAKE_PENDING;
    device->pending_read = overlapped;
    device->pending_buffer = buffer;
    SetLastError(ERROR_IO_PENDING);
    return FALSE;
}

static BOOL WINAPI fake_write_file(HANDLE file, LPCVOID buffer, DWORD length, LPDWORD written,
                                   LPOVERLAPPED overlapped)
{
    fake_device *const device = fake_owner(file);

    if (written != NULL || length > TB_HID_REPORT_CAPACITY)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    device->writes++;
    memcpy(device->last_write, buffer, length);
    if (device->writes_hang)
    {
        overlapped->Internal = FAKE_PENDING;
        device->pending_write = overlapped;
        SetLastError(ERROR_IO_PENDING);
        return FALSE;
    }
    overlapped->Internal = FAKE_DONE;
    overlapped->InternalHigh = length;
    return TRUE;
}

static BOOL WINAPI fake_overlapped_result(HANDLE file, LPOVERLAPPED overlapped, LPDWORD transferred,
                                          DWORD milliseconds, BOOL alertable)
{
    fake_device *const device = fake_owner(file);

    (void)alertable;
    if (overlapped->Internal == FAKE_PENDING && overlapped == device->pending_read &&
        fake_take(device, device->pending_buffer, transferred))
    {
        overlapped->Internal = FAKE_DONE;
        return TRUE;
    }
    if (overlapped->Internal == FAKE_DONE)
    {
        *transferred = (DWORD)overlapped->InternalHigh;
        return TRUE;
    }
    if (overlapped->Internal == FAKE_CANCELLED)
    {
        SetLastError(ERROR_OPERATION_ABORTED);
        return FALSE;
    }
    if (device->unplugged)
    {
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return FALSE;
    }
    SetLastError(milliseconds == 0 ? ERROR_IO_INCOMPLETE : WAIT_TIMEOUT);
    return FALSE;
}

static BOOL WINAPI fake_cancel_io(HANDLE file, LPOVERLAPPED overlapped)
{
    (void)file;
    fake.cancels++;
    overlapped->Internal = FAKE_CANCELLED;
    return TRUE;
}

static const tb_hid_calls fake_calls = {
    .list_size = fake_list_size,
    .list = fake_list,
    .interface_property = fake_interface_property,
    .locate = fake_locate,
    .node_property = fake_node_property,
    .attributes = fake_attributes,
    .usb_initialize = fake_usb_initialize,
    .usb_free = fake_usb_free,
    .usb_query_interface = fake_usb_query_interface,
    .usb_query_pipe = fake_usb_query_pipe,
    .usb_set_pipe_policy = fake_usb_set_pipe_policy,
    .usb_write = fake_usb_write,
    .usb_read = fake_usb_read,
    .create_file = fake_create_file,
    .close_handle = fake_close_handle,
    .create_event = fake_create_event,
    .read_file = fake_read_file,
    .write_file = fake_write_file,
    .overlapped_result = fake_overlapped_result,
    .cancel_io = fake_cancel_io,
};

/* A Switch 2 Pro Controller as Windows shows it: its HID interface and
 * its WinUSB vendor interface, in one device container. */
static void fake_switch2(size_t index, const wchar_t *hid_path, const wchar_t *usb_path)
{
    fake.devices[index] = (fake_device){
        .hid_path = hid_path,
        .usb_path = usb_path,
        .container = {0x380e34d4u + (unsigned long)index, 0x8c8d, 0x5f46, {0x9c, 0xbc}},
        .vendor = TB_SWITCH2_VENDOR,
        .product = TB_SWITCH2_PRODUCT_PRO,
        .present = true,
    };
}

static const wchar_t first_hid[] =
    L"\\\\?\\HID#VID_057E&PID_2069&MI_00#7&763804&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
static const wchar_t first_usb[] =
    L"\\\\?\\usb#vid_057e&pid_2069&mi_01#6&2356d3fd&0&0001#{6f13725e-ef0e-4fd3-ae5f-b2de989ec825}";
static const wchar_t second_hid[] =
    L"\\\\?\\hid#vid_057e&pid_2069&mi_00#7&99&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
static const wchar_t second_usb[] =
    L"\\\\?\\usb#vid_057e&pid_2069&mi_01#6&88&0&0001#{6f13725e-ef0e-4fd3-ae5f-b2de989ec825}";

static tb_gamepad_system *fake_open(const char *ignored, bool thread)
{
    fake = (fake_system){0};
    fake_now = 0;
    fake_switch2(0, first_hid, first_usb);
    return tb_gamepad_system_make(&tb_hid_gamepad_ops,
                                  tb_hid_gamepads_make(&fake_calls, ignored, fake_clock, thread),
                                  fake_clock);
}

static void queue_report(fake_device *device, uint8_t byte, uint8_t mask)
{
    uint8_t *const report = device->queue[device->queued % FAKE_QUEUE];

    memset(report, 0, TB_HID_REPORT_CAPACITY);
    report[0] = TB_SWITCH2_REPORT_PRO;
    report[byte] = mask;
    /* Both sticks at their centers. */
    report[6] = 0x00;
    report[7] = 0x08;
    report[8] = 0x80;
    report[9] = 0x00;
    report[10] = 0x08;
    report[11] = 0x80;
    device->queued++;
}

static tb_gamepad_state state_at(const tb_gamepad_system *system, size_t slot)
{
    tb_gamepad_state state = {0};

    tb_gamepad_get_state(system, slot, &state);
    return state;
}

/* A Switch 2 Pro Controller is found at the first poll, set up through its
 * vendor channel, which is closed again, and named. */
TB_TEST(platform_win32_gamepad_rawhid, a_switch2_pad_is_found_and_set_up)
{
    tb_gamepad_system *const system = fake_open(NULL, false);
    tb_gamepad_info info = {0};
    tb_gamepad_event event = {0};

    tb_gamepad_system_poll(system);
    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    TB_TEST_EXPECT_INT(event.type, ==, TB_GAMEPAD_EVENT_CONNECTED);
    TB_TEST_EXPECT_UINT(event.slot, ==, 0u);
    tb_gamepad_get_info(system, 0, &info);
    TB_TEST_EXPECT_STRING(info.name, ==, "Switch 2 Pro Controller");
    TB_TEST_EXPECT_HEX(info.vendor, ==, 0x057e);
    TB_TEST_EXPECT_HEX(info.product, ==, 0x2069);
    TB_TEST_EXPECT_HEX(info.capabilities, ==, TB_GAMEPAD_CAPABILITY_RUMBLE);
    /* The four flash reads and the six setup commands. */
    TB_TEST_EXPECT_UINT(fake.devices[0].commands, ==, 10u);
    TB_TEST_EXPECT_UINT(fake.usb_frees, ==, 1u);
    /* The HID interface and its two events stay open, and nothing else. */
    TB_TEST_EXPECT_INT(fake.handles, ==, 3);
    TB_TEST_EXPECT(state_at(system, 0).connected);
    tb_gamepad_system_close(system);
    TB_TEST_EXPECT_INT(fake.handles, ==, 0);
}

/* Every report that came since the last poll is read, without waiting,
 * and the last is the pad's state; a read still outstanding at a poll is
 * finished at a later one. */
TB_TEST(platform_win32_gamepad_rawhid, reports_are_read_without_waiting)
{
    tb_gamepad_system *const system = fake_open(NULL, false);
    fake_device *const device = &fake.devices[0];
    tb_gamepad_state state = {0};

    tb_gamepad_system_poll(system);
    queue_report(device, 3, 0x01); /* B */
    queue_report(device, 3, 0x02); /* A */
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_HEX(state_at(system, 0).buttons, ==, 1u << TB_GAMEPAD_BUTTON_EAST);
    TB_TEST_EXPECT_UINT(device->taken, ==, 2u);
    /* Nothing new: the state stays. */
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_HEX(state_at(system, 0).buttons, ==, 1u << TB_GAMEPAD_BUTTON_EAST);
    queue_report(device, 3, 0x08); /* X */
    tb_gamepad_system_poll(system);
    state = state_at(system, 0);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 1u << TB_GAMEPAD_BUTTON_NORTH);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_LEFT_X], 0.0f, 0.001f);
    tb_gamepad_system_close(system);
    /* The read outstanding at the close was cancelled and waited for. */
    TB_TEST_EXPECT_UINT(fake.cancels, ==, 1u);
}

/* XInput's own HID collections, pads no driver knows, interfaces whose
 * paths name no USB IDs, and pads Steam names are never opened. */
TB_TEST(platform_win32_gamepad_rawhid, other_interfaces_are_left_alone)
{
    tb_gamepad_system *system = fake_open(NULL, false);

    fake.extra_paths[0] = L"\\\\?\\HID#VID_057E&PID_2069&IG_00#7&1&0&0000#{x}";
    fake.extra_paths[1] = L"\\\\?\\HID#VID_046D&PID_C542&Col01#6&8d&0&0000#{x}";
    fake.extra_paths[2] = L"\\\\?\\HID#ELAN0406&Col04#5&1d&0&0003#{x}";
    fake.extra_paths[3] = L"\\\\?\\HID#VID_057E&PID_20#7&1&0&0000#{x}";
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(state_at(system, 0).connected);
    TB_TEST_EXPECT(!state_at(system, 1).connected);
    TB_TEST_EXPECT_UINT(fake.opens_of_extras, ==, 0u);
    tb_gamepad_system_close(system);

    system = fake_open("0x28de/0x1205,0x057e/0x2069", false);
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(!state_at(system, 0).connected);
    TB_TEST_EXPECT_INT(fake.handles, ==, 0);
    tb_gamepad_system_close(system);
}

/* When another program has the vendor channel, as Steam does, the pad is
 * read as that program set it up, with no command sent. */
TB_TEST(platform_win32_gamepad_rawhid, a_busy_channel_is_gone_without)
{
    tb_gamepad_system *const system = fake_open(NULL, false);

    fake.devices[0].usb_busy = true;
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(state_at(system, 0).connected);
    TB_TEST_EXPECT_UINT(fake.devices[0].commands, ==, 0u);
    TB_TEST_EXPECT_UINT(fake.usb_frees, ==, 0u);
    tb_gamepad_system_close(system);
}

/* Each pad is set up through its own channel, found by its device
 * container, not through another pad's. */
TB_TEST(platform_win32_gamepad_rawhid, each_pad_has_its_own_channel)
{
    tb_gamepad_system *const system = fake_open(NULL, false);
    tb_gamepad_info info = {0};

    fake_switch2(1, second_hid, second_usb);
    /* A third device whose WinUSB interface is in no pad's container. */
    fake.devices[2] = (fake_device){.hid_path = L"\\\\?\\HID#VID_1234&PID_5678#1#{x}",
                                    .usb_path = L"\\\\?\\usb#vid_1234&pid_5678#2#{y}",
                                    .container = {7},
                                    .vendor = 0x1234,
                                    .product = 0x5678,
                                    .present = true};
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(state_at(system, 0).connected);
    TB_TEST_EXPECT(state_at(system, 1).connected);
    TB_TEST_EXPECT_UINT(fake.devices[0].commands, ==, 10u);
    TB_TEST_EXPECT_UINT(fake.devices[1].commands, ==, 10u);
    TB_TEST_EXPECT_UINT(fake.devices[2].commands, ==, 0u);
    tb_gamepad_get_info(system, 1, &info);
    TB_TEST_EXPECT_HEX(info.product, ==, 0x2069);
    tb_gamepad_system_close(system);
}

/* A pad that cannot be opened for writing is read, without rumble. */
TB_TEST(platform_win32_gamepad_rawhid, a_read_only_pad_cannot_rumble)
{
    tb_gamepad_system *const system = fake_open(NULL, false);
    tb_gamepad_info info = {0};

    fake.devices[0].read_only = true;
    tb_gamepad_system_poll(system);
    tb_gamepad_get_info(system, 0, &info);
    TB_TEST_EXPECT(state_at(system, 0).connected);
    TB_TEST_EXPECT_HEX(info.capabilities, ==, 0);
    TB_TEST_EXPECT(!tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){1.0f, 1.0f, 1.0}));
    TB_TEST_EXPECT_UINT(fake.devices[0].writes, ==, 0u);
    tb_gamepad_system_close(system);
}

/* A pad whose IDs are not what its path says, or whose events cannot be
 * made, is closed again and not reported. */
TB_TEST(platform_win32_gamepad_rawhid, a_pad_that_fails_to_open_is_closed)
{
    tb_gamepad_system *system = fake_open(NULL, false);

    fake.attributes_lie = true;
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(!state_at(system, 0).connected);
    TB_TEST_EXPECT_INT(fake.handles, ==, 0);
    tb_gamepad_system_close(system);

    system = fake_open(NULL, false);
    fake.events_fail = true;
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(!state_at(system, 0).connected);
    TB_TEST_EXPECT_INT(fake.handles, ==, 0);
    tb_gamepad_system_close(system);
}

/* A pad that goes is gone at the next poll, its read failing; one that
 * comes is found at the next listing, every half second, and a list that
 * grew while it was asked for is asked for again. */
TB_TEST(platform_win32_gamepad_rawhid, pads_go_and_come)
{
    tb_gamepad_system *const system = fake_open(NULL, false);
    tb_gamepad_event event = {0};
    size_t listed = 0;

    tb_gamepad_system_poll(system);
    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    /* A read outstanding that fails. */
    tb_gamepad_system_poll(system);
    TB_TEST_REQUIRE_POINTER(fake.devices[0].pending_read, !=, NULL);
    fake.devices[0].unplugged = true;
    fake.devices[0].present = false;
    tb_gamepad_system_poll(system);
    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    TB_TEST_EXPECT_INT(event.type, ==, TB_GAMEPAD_EVENT_DISCONNECTED);
    TB_TEST_EXPECT_INT(fake.handles, ==, 0);

    fake.devices[0].unplugged = false;
    fake.devices[0].present = true;
    listed = fake.list_calls;
    fake_now = TB_HID_SCAN_NANOSECONDS - 1u;
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_UINT(fake.list_calls, ==, listed);
    TB_TEST_EXPECT(!state_at(system, 0).connected);
    fake.list_grows_once = true;
    fake_now = TB_HID_SCAN_NANOSECONDS;
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(state_at(system, 0).connected);
    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    TB_TEST_EXPECT_INT(event.type, ==, TB_GAMEPAD_EVENT_CONNECTED);

    /* A read that fails when it is started is a pad that went, too. */
    fake.devices[0].unplugged = true;
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(!state_at(system, 0).connected);

    /* A listing the system refuses finds nothing. */
    fake.devices[0].unplugged = false;
    fake.list_fails = true;
    fake_now = 2u * TB_HID_SCAN_NANOSECONDS;
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(!state_at(system, 0).connected);
    tb_gamepad_system_close(system);
}

/* Rumble is written at once, written again every period while it runs,
 * and stopped with a report of no strength when its time is up, which the
 * backend times itself, so that the pad stops even when the game stops
 * polling. */
TB_TEST(platform_win32_gamepad_rawhid, rumble_is_sent_again_while_it_runs)
{
    tb_gamepad_system *const system = fake_open(NULL, false);
    fake_device *const device = &fake.devices[0];

    tb_gamepad_system_poll(system);
    TB_TEST_REQUIRE(tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){0.5f, 0.5f, 0.1}));
    TB_TEST_EXPECT_UINT(device->writes, ==, 1u);
    TB_TEST_EXPECT_HEX(device->last_write[0], ==, TB_SWITCH2_REPORT_RUMBLE);
    TB_TEST_EXPECT_HEX(device->last_write[6], !=, 0);
    tb_gamepad_system_close(system);
    /* Closing stopped it. */
    TB_TEST_EXPECT_UINT(device->writes, ==, 2u);
    TB_TEST_EXPECT_HEX(device->last_write[6], ==, 0);
}

/* The refresh, called as the rumble thread calls it. */
TB_TEST(platform_win32_gamepad_rawhid, refresh_sends_what_is_due)
{
    const tb_gamepad_ops *const ops = &tb_hid_gamepad_ops;
    fake_device *device = NULL;
    void *native = NULL;
    tb_gamepad_pad pads[TB_GAMEPAD_SLOT_COUNT] = {0};
    const uint64_t period = 8u * FAKE_MILLISECONDS;

    fake = (fake_system){0};
    fake_now = 0;
    fake_switch2(0, first_hid, first_usb);
    device = &fake.devices[0];
    native = tb_hid_gamepads_make(&fake_calls, NULL, fake_clock, false);
    ops->poll(native, 0, pads);
    TB_TEST_REQUIRE_UINT(pads[0].serial, !=, 0u);
    ops->rumble(native, 0, 0.5f, 0.25f, 0.02);
    TB_TEST_EXPECT_UINT(device->writes, ==, 1u);
    tb_hid_gamepads_refresh(native, period - 1u);
    TB_TEST_EXPECT_UINT(device->writes, ==, 1u);
    tb_hid_gamepads_refresh(native, period);
    TB_TEST_EXPECT_UINT(device->writes, ==, 2u);
    TB_TEST_EXPECT_HEX(device->last_write[6], !=, 0);
    tb_hid_gamepads_refresh(native, 2u * period);
    TB_TEST_EXPECT_UINT(device->writes, ==, 3u);
    /* At 20 milliseconds its time is up: a report of no strength, then
     * nothing more. */
    tb_hid_gamepads_refresh(native, 20u * FAKE_MILLISECONDS);
    TB_TEST_EXPECT_UINT(device->writes, ==, 4u);
    TB_TEST_EXPECT_HEX(device->last_write[5], ==, 0x11);
    TB_TEST_EXPECT_HEX(device->last_write[6], ==, 0x00);
    tb_hid_gamepads_refresh(native, 40u * FAKE_MILLISECONDS);
    TB_TEST_EXPECT_UINT(device->writes, ==, 4u);

    /* Stopped by the front: one report of no strength. */
    ops->rumble(native, 0, 1.0f, 1.0f, 1.0);
    ops->rumble(native, 0, 0.0f, 0.0f, 0.0);
    TB_TEST_EXPECT_UINT(device->writes, ==, 6u);
    tb_hid_gamepads_refresh(native, 60u * FAKE_MILLISECONDS);
    TB_TEST_EXPECT_UINT(device->writes, ==, 6u);
    /* A system over the backend closes it; nothing runs to stop. */
    tb_gamepad_system_close(tb_gamepad_system_make(ops, native, fake_clock));
    TB_TEST_EXPECT_UINT(device->writes, ==, 6u);
    TB_TEST_EXPECT_INT(fake.handles, ==, 0);
}

/* A write that does not end in time is cancelled, and waited for. */
TB_TEST(platform_win32_gamepad_rawhid, a_hung_write_is_cancelled)
{
    tb_gamepad_system *const system = fake_open(NULL, false);

    tb_gamepad_system_poll(system);
    fake.devices[0].writes_hang = true;
    (void)tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){0.5f, 0.5f, 0.1});
    TB_TEST_EXPECT_UINT(fake.devices[0].writes, ==, 1u);
    TB_TEST_EXPECT_UINT(fake.cancels, ==, 1u);
    fake.devices[0].writes_hang = false;
    tb_gamepad_system_close(system);
}

/* The rumble thread starts with the first rumble and ends with the
 * system. */
TB_TEST(platform_win32_gamepad_rawhid, the_rumble_thread_ends_with_the_system)
{
    tb_gamepad_system *const system = fake_open(NULL, true);

    tb_gamepad_system_poll(system);
    TB_TEST_REQUIRE(tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){0.5f, 0.5f, 0.1}));
    TB_TEST_REQUIRE(tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){0.4f, 0.4f, 0.1}));
    tb_gamepad_system_close(system);
    TB_TEST_EXPECT_UINT(fake.devices[0].writes, >=, 3u);
    TB_TEST_EXPECT_INT(fake.handles, ==, 0);
}

/* A real Switch 2 Pro Controller, through the system's own libraries:
 * found, named, and read in the layout's ranges. Skipped without one, as
 * on CI. Its buttons and sticks are whatever the player is doing, so they
 * are not checked; the sandbox's gamepad scene shows them. */
TB_TEST(platform_win32_gamepad_rawhid, a_real_switch2_pad_is_read)
{
    tb_gamepad_system *system = NULL;
    size_t found = TB_GAMEPAD_SLOT_COUNT;
    const uint64_t start = tb_platform_clock_nanoseconds();
    const uint64_t patience = 1500u * FAKE_MILLISECONDS;
    tb_gamepad_state state = {0};
    tb_gamepad_info info = {0};
    const uint32_t layout = (1u << TB_GAMEPAD_BUTTON_COUNT) - 1u;

    TB_TEST_REQUIRE_INT(tb_gamepad_system_open(&system, NULL), ==, TB_GAMEPAD_OK);
    while (tb_platform_clock_nanoseconds() - start < patience)
    {
        tb_gamepad_system_poll(system);
        for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT && found == TB_GAMEPAD_SLOT_COUNT;
             slot++)
        {
            tb_gamepad_info candidate = {0};
            tb_gamepad_get_info(system, slot, &candidate);
            if (candidate.vendor == TB_SWITCH2_VENDOR &&
                candidate.product == TB_SWITCH2_PRODUCT_PRO)
            {
                found = slot;
            }
        }
        tb_platform_sleep(20u * FAKE_MILLISECONDS);
    }
    if (found == TB_GAMEPAD_SLOT_COUNT)
    {
        tb_gamepad_system_close(system);
        TB_TEST_SKIP("no Switch 2 Pro Controller is plugged in");
    }
    tb_gamepad_get_state(system, found, &state);
    tb_gamepad_get_info(system, found, &info);
    tb_gamepad_system_close(system);
    TB_TEST_EXPECT(state.connected);
    TB_TEST_EXPECT_STRING(info.name, ==, "Switch 2 Pro Controller");
    TB_TEST_EXPECT_HEX(state.buttons & ~layout, ==, 0);
    for (size_t axis = 0; axis < TB_GAMEPAD_AXIS_COUNT; axis++)
    {
        TB_TEST_CONTEXT("axis %zu", axis);
        TB_TEST_EXPECT_FLOAT(state.axes[axis], >=, -1.0f);
        TB_TEST_EXPECT_FLOAT(state.axes[axis], <=, 1.0f);
    }
}
