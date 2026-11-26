/* Private to the Windows gamepad backend and its tests: the raw HID
 * backend, and the system's functions it calls, which a test replaces with
 * devices of its own. */
#ifndef TB_WIN32_GAMEPAD_RAWHID_H
#define TB_WIN32_GAMEPAD_RAWHID_H

#include "../shared/gamepad.h"
#include "../shared/gamepad_hid.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cfgmgr32.h>
#include <hidsdi.h>
#include <winusb.h>

/* How often the device interfaces are listed for pads that came: listing
 * the HID interfaces takes about a tenth of a millisecond. */
#define TB_HID_SCAN_NANOSECONDS 500000000u

/* How long a command on a pad's vendor channel, and a rumble report, may
 * take before the backend gives up on it. */
#define TB_HID_COMMAND_TIMEOUT_MILLISECONDS 500u
#define TB_HID_WRITE_TIMEOUT_MILLISECONDS 50u

/* The most input reports read from a pad in one poll; a pad reports a few
 * hundred times a second. */
#define TB_HID_READS_PER_POLL 256u

/* The longest report, input, output or feature, of a device the backend
 * opens: a DualSense's descriptor gives some of 547 bytes over Bluetooth. */
#define TB_HID_REPORT_LIMIT 4096u

/* The functions the backend calls, each required: cfgmgr32.dll's, which
 * list the device interfaces and read their properties; hid.dll's, which
 * give a device's IDs and its reports' lengths and read its feature
 * reports; winusb.dll's, for a pad's vendor channel; and kernel32.dll's
 * file functions. */
typedef struct tb_hid_calls
{
    CONFIGRET(WINAPI *list_size)(PULONG size, LPGUID interface_class, DEVINSTID_W device,
                                 ULONG flags);
    CONFIGRET(WINAPI *list)(LPGUID interface_class, DEVINSTID_W device, PZZWSTR buffer,
                            ULONG length, ULONG flags);
    CONFIGRET(WINAPI *interface_property)(LPCWSTR path, const DEVPROPKEY *key, DEVPROPTYPE *type,
                                          PBYTE buffer, PULONG size, ULONG flags);
    CONFIGRET(WINAPI *locate)(PDEVINST node, DEVINSTID_W device, ULONG flags);
    CONFIGRET(WINAPI *node_property)(DEVINST node, const DEVPROPKEY *key, DEVPROPTYPE *type,
                                     PBYTE buffer, PULONG size, ULONG flags);
    BOOLEAN(WINAPI *attributes)(HANDLE file, PHIDD_ATTRIBUTES attributes);
    BOOLEAN(WINAPI *preparsed)(HANDLE file, PHIDP_PREPARSED_DATA *data);
    BOOLEAN(WINAPI *free_preparsed)(PHIDP_PREPARSED_DATA data);
    NTSTATUS(WINAPI *caps)(PHIDP_PREPARSED_DATA data, PHIDP_CAPS caps);
    BOOLEAN(WINAPI *get_feature)(HANDLE file, PVOID report, ULONG length);
    BOOL(WINAPI *usb_initialize)(HANDLE file, PWINUSB_INTERFACE_HANDLE usb);
    BOOL(WINAPI *usb_free)(WINUSB_INTERFACE_HANDLE usb);
    BOOL(WINAPI *usb_query_interface)(WINUSB_INTERFACE_HANDLE usb, UCHAR alternate,
                                      PUSB_INTERFACE_DESCRIPTOR descriptor);
    BOOL(WINAPI *usb_query_pipe)(WINUSB_INTERFACE_HANDLE usb, UCHAR alternate, UCHAR index,
                                 PWINUSB_PIPE_INFORMATION pipe);
    BOOL(WINAPI *usb_set_pipe_policy)(WINUSB_INTERFACE_HANDLE usb, UCHAR pipe, ULONG policy,
                                      ULONG length, PVOID value);
    BOOL(WINAPI *usb_write)(WINUSB_INTERFACE_HANDLE usb, UCHAR pipe, PUCHAR buffer, ULONG length,
                            PULONG written, LPOVERLAPPED overlapped);
    BOOL(WINAPI *usb_read)(WINUSB_INTERFACE_HANDLE usb, UCHAR pipe, PUCHAR buffer, ULONG length,
                           PULONG read, LPOVERLAPPED overlapped);
    HANDLE(WINAPI *create_file)(LPCWSTR path, DWORD access, DWORD share,
                                LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags,
                                HANDLE template_file);
    BOOL(WINAPI *close_handle)(HANDLE handle);
    HANDLE(WINAPI *create_event)(LPSECURITY_ATTRIBUTES security, BOOL manual, BOOL initial,
                                 LPCWSTR name);
    BOOL(WINAPI *read_file)(HANDLE file, LPVOID buffer, DWORD length, LPDWORD read,
                            LPOVERLAPPED overlapped);
    BOOL(WINAPI *write_file)(HANDLE file, LPCVOID buffer, DWORD length, LPDWORD written,
                             LPOVERLAPPED overlapped);
    BOOL(WINAPI *overlapped_result)(HANDLE file, LPOVERLAPPED overlapped, LPDWORD transferred,
                                    DWORD milliseconds, BOOL alertable);
    BOOL(WINAPI *cancel_io)(HANDLE file, LPOVERLAPPED overlapped);
} tb_hid_calls;

/* The backend's table. */
extern const tb_gamepad_ops tb_hid_gamepad_ops;

/* A backend that finds pads through `calls`. `ignored` is a list as
 * SDL_GAMECONTROLLER_IGNORE_DEVICES holds, or NULL; it is copied. `clock`
 * times rumble. With `rumble_thread`, a thread sends the rumble reports of
 * pads whose driver needs them sent again and again; without, only
 * tb_hid_gamepads_refresh() does, as a test calls it. For
 * tb_gamepad_system_make() with tb_hid_gamepad_ops; it owns no library. */
void *tb_hid_gamepads_make(const tb_hid_calls *calls, const char *ignored, tb_gamepad_clock *clock,
                           bool rumble_thread);

/* Send the rumble reports that are due at `now`, and stop the rumble whose
 * time is up, as the rumble thread does. */
void tb_hid_gamepads_refresh(void *native, uint64_t now);

/* Open the raw HID backend over the system's libraries, loaded from the
 * system directory. TB_GAMEPAD_UNAVAILABLE without them. */
tb_gamepad_result tb_hid_gamepads_open(void **native, tb_gamepad_error *error);

#endif
