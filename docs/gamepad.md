# Gamepads

`gamepad.h` is the platform interface for the controllers a player holds. It
reads every gamepad the platform reports, in one standard layout, and rumbles
the pads that have motors. Every desktop platform implements it; any other
platform's build reports it as not implemented.

| Platform | Gamepads | How it is reached |
| --- | --- | --- |
| Windows | XInput: Xbox pads, and what Steam Input exposes, under Proton too | `xinput1_4.dll`, or `xinput9_1_0.dll` before Windows 8, loaded at run time from the system directory |
| Windows | Raw HID: the pads a protocol driver of the engine's knows, which XInput does not read ([raw HID pads](#raw-hid-pads)) | `cfgmgr32.dll`, `hid.dll` and `winusb.dll`, loaded at run time from the system directory |
| Linux and Steam Deck | evdev: the event devices of `/dev/input`, read without grabbing | the kernel's system calls; nothing loaded |
| macOS | The GameController framework's extended gamepads | `libobjc.A.dylib` and `GameController.framework`, loaded at run time |

A system without the library gets `TB_GAMEPAD_UNAVAILABLE` from
`tb_gamepad_system_open()`: a capability that is missing, and a game plays on
without gamepads. On Windows, XInput and raw HID are one system: either is
enough, and their pads share the slots.

## Supported controllers

| Controller | Windows | Linux and Steam Deck | macOS |
| --- | --- | --- | --- |
| Xbox pads and XInput pads | XInput | xpad | GameController |
| PlayStation pads | Through Steam Input | hid-playstation | GameController |
| Switch Pro Controller | Through Steam Input | hid-nintendo | GameController |
| Switch 2 Pro Controller, USB (`057e:2069`) | Raw HID, with rumble | hid-nintendo, from Linux 7.4 | Not yet: the framework does not know it (#379) |
| Joy-Con 2, GameCube controller for Switch 2 | Not yet (#378) | hid-nintendo, from Linux 7.4 | Not yet (#379) |
| Steam Deck | | hid-steam | |
| Any pad, under Steam | Steam Input's virtual XInput pad | Steam Input's virtual pad | |

A pad that Steam stands in for is seen once: through Steam's virtual pad, as
Steam names the physical ones in `SDL_GAMECONTROLLER_IGNORE_DEVICES` (see
[Steam](#mapping-evdev-devices) below), which the raw HID backend honors too.

```c
tb_gamepad_system *gamepads = NULL;
tb_gamepad_error error = TB_GAMEPAD_ERROR_INIT;
if (tb_gamepad_system_open(&gamepads, &error) != TB_GAMEPAD_OK)
{
    tb_fmt_print_error("no gamepads: %s\n", error.message);
}

/* Once a frame, on the main thread: */
tb_gamepad_system_set_vibration(gamepads, (float)tb_setting_number(engine->controls.gamepad_vibration));
tb_gamepad_system_poll(gamepads);
tb_gamepad_event event = {0};
while (tb_gamepad_system_next_event(gamepads, &event))
{
    /* TB_GAMEPAD_EVENT_CONNECTED or _DISCONNECTED, of event.slot */
}
for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
{
    tb_gamepad_state state = {0};
    tb_gamepad_get_state(gamepads, slot, &state);
    tb_input_feed_gamepad(input, TB_INPUT_DEVICE_GAMEPAD(slot), &state);
}
```

A game reads pads through the [input module](input.md) as actions, which
`tb_input_feed_gamepad()` feeds, or reads their states directly. The module
is not yet driven by the [game loop](game-loop.md), the [GUI](gui.md) or
Burrow. The sandbox's `gamepad` scene ([sandbox](sandbox.md)) shows every
connected pad live, and rumbles them on R.

## Slots and hot-plugging

A system has `TB_GAMEPAD_SLOT_COUNT` (8) slots. A pad keeps its slot from the
poll that finds it to the poll that finds it gone; a pad plugged in later
takes the lowest free slot. A pad that comes or goes is an event, in the
order the polls saw them, and a pad unplugged and another plugged in between
two polls are both. An empty slot's state and info are all zero.

| Platform | Slots | A pad that comes | A pad that goes |
| --- | --- | --- | --- |
| Windows | The lowest free, XInput's pads and raw HID pads alike | XInput's empty users are asked, and the HID device interfaces listed, every half second | At the next poll: XInput says so, and a raw HID pad's read fails |
| Linux | The lowest free | inotify on `/dev/input`: a node made, or its permissions set by udev a moment later | Its node removed, or a read that fails as an unplugged device's does |
| macOS | The lowest free | `+[GCController controllers]`, listed every poll | At the next poll |

`tb_gamepad_get_info()` gives a pad's name as the system gives it ("Xbox
controller" from XInput, which names only a kind; the protocol driver's, as
"Switch 2 Pro Controller", for a raw HID pad; the driver's name on Linux, as
"Microsoft X-Box One S pad"; the vendor's on macOS), its USB vendor and
product where the system says (raw HID pads and Linux), and its capabilities.

## The standard layout

Buttons and axes are named by their place on an Xbox-style pad, whatever
their labels say: south is A on an Xbox pad, Cross on a PlayStation pad, and
B on a Nintendo pad.

### Nintendo's pads by place

Nintendo puts A on the right and B at the bottom, the other way round from
Xbox, and X at the top and Y on the left. The engine reads them by place, on
every platform, so that south is always the button under the player's thumb:

| Place | Xbox | Switch and Switch 2 | Engine |
| --- | --- | --- | --- |
| Bottom | A | B | `south` |
| Right | B | A | `east` |
| Left | X | Y | `west` |
| Top | Y | X | `north` |
| Shoulders | LB, RB | L, R | `left_shoulder`, `right_shoulder` |
| Triggers | LT, RT | ZL, ZR, digital: 0 or 1 | `left_trigger`, `right_trigger` |
| Middle | View, Menu | -, + | `back`, `start` |
| Back buttons | Elite's P3, P1 | Switch 2's GL, GR | `left_paddle_1`, `right_paddle_1` |
| System | Guide, Share | Home, Capture, and the Switch 2's C | Never read |

A game that shows prompts names the button by its label for the pad in
hand; the engine gives the place. The C button opens the Switch 2's chat,
so it is the system's too, as Capture is.

| Buttons | Axes |
| --- | --- |
| `south`, `east`, `west`, `north`; `left_shoulder`, `right_shoulder`; `left_stick`, `right_stick` (pressed); `dpad_up`, `_down`, `_left`, `_right`; `start`, `back`; the back paddles `left_paddle_1`, `_2`, `right_paddle_1`, `_2` | `left_x`, `left_y`, `right_x`, `right_y` from -1 to 1, right and up positive; `left_trigger`, `right_trigger` from 0 to 1 |

Values are the pad's own, scaled to those ranges and nothing more. A stick's
range scales each half to its own end, so XInput's -32768 to 32767 reaches
-1 and 1 and rests at 0, and a range of 0 to 255 rests at 128. **Dead zones
are not applied here**: the input module applies them, once, in its settings
modifier, from `controls.gamepad.left_dead_zone` and `right_dead_zone`
([input](input.md#settings)). A game that reads states directly applies its
own.

### Mapping evdev devices

XInput and the GameController framework give the standard layout already.
evdev gives each driver's codes, which `src/platform/linux/gamepad_evdev.c`
maps itself, with no database of controllers:

- **What a gamepad is.** A device with the south button (`BTN_SOUTH`) and a
  stick's `ABS_X` and `ABS_Y`. Keyboards, mice, joysticks and touchpads are
  passed over.
- **The face buttons.** The kernel's documentation names them by place
  (`BTN_NORTH` the top, `BTN_WEST` the left), and the Sony and Nintendo
  drivers follow it. xpad, hid-steam and Steam's virtual pads report Xbox
  labels instead: `BTN_X`, the same code as `BTN_NORTH`, for the left button.
  A device of Sony's or Nintendo's USB vendor is read by place, any other by
  label.
- **The right stick and the triggers.** The right stick is `ABS_RX` and
  `ABS_RY`, or `ABS_Z` and `ABS_RZ` on generic HID pads. The triggers are
  `ABS_Z` and `ABS_RZ` (xpad, hid-playstation), `ABS_HAT2Y` and `ABS_HAT2X`
  (hid-steam, the Steam Deck), `ABS_BRAKE` and `ABS_GAS`, or the digital
  `BTN_TL2` and `BTN_TR2`, at 0 or 1 (hid-nintendo).
- **The d-pad.** `ABS_HAT0X` and `ABS_HAT0Y`, the `BTN_DPAD_*` buttons, or
  xpad's first four `BTN_TRIGGER_HAPPY` buttons.
- **The paddles.** The Steam Deck's L4, R4, L5, R5 (`BTN_GRIPL`, `BTN_GRIPR`,
  `BTN_GRIPL2`, `BTN_GRIPR2`) and the Xbox Elite pad's P1 to P4
  (`BTN_TRIGGER_HAPPY5` to `8`).
- **Dropped events.** After `SYN_DROPPED` the events up to the next report are
  incomplete, so the device's whole state is read again then, with
  `EVIOCGKEY` and `EVIOCGABS`.

- **The Switch 2 controllers** (`057e:2066`, `2067`, `2069`, `2073`), which
  `hid-nintendo` drives from Linux 7.4, name A and B by their labels'
  codes: A, on the right, is `BTN_SOUTH`, and B, at the bottom, `BTN_EAST`.
  Their south and east are swapped back, so they read by place as every
  other Nintendo pad does. GL and GR are `BTN_GRIPL` and `BTN_GRIPR`, the
  upper paddles; Home (`BTN_MODE`), Capture (`KEY_RECORD`) and C (`BTN_C`)
  are not read. Older kernels have no driver for them, and the pad stays
  silent ([known gaps](#known-gaps)).

Each axis's range comes from `EVIOCGABS`. The tests feed these mappings the
kernel's events for each driver, on every platform
(`tests/platform/gamepad_evdev_test.c`).

**Steam.** Under Steam each physical pad also appears as a virtual xpad pad
that Steam Input makes, and Steam names the physical ones in
`SDL_GAMECONTROLLER_IGNORE_DEVICES` (entries of `0xVVVV/0xPPPP`) for the games
it starts. The backend leaves those alone, so that each pad is seen once,
through Steam's mapping of it.

**Permissions.** A desktop gives the logged-in player access to gamepads'
nodes. A node that can be read but not written is a pad without rumble.

## Raw HID pads

On Windows, pads that XInput does not read are read through their HID
interfaces, by **protocol drivers** in standard C under `src/platform/shared/`
(`gamepad_hid.h`). A driver knows one family of pads by USB vendor and
product: the commands that set a pad up, its input reports, and its rumble
report. The Windows backend (`src/platform/win32/gamepad_rawhid.c`) finds the
pads, opens them, reads their reports, and sends what the driver makes; a new
pad is a new driver, and the backend does not change. One driver is written:
the Switch 2 Pro Controller's (`gamepad_switch2.c`). Every driver is tested
on every platform, with reports captured from a real pad.

- **Finding pads.** The HID device interfaces are listed every half second
  with `CM_Get_Device_Interface_ListW`, which takes about a tenth of a
  millisecond, rather than through a notification that would call back on
  another thread. Only interfaces whose path names the vendor and product of
  a pad a driver knows are opened.
- **Not twice.** XInput's own HID collections, whose paths have `IG_` in them,
  are never opened: XInput reads those. Nor are the pads Steam names in
  `SDL_GAMECONTROLLER_IGNORE_DEVICES`.
- **Reading.** Each pad has one overlapped read outstanding. A poll takes every
  report that came since the last without waiting, and the last is the pad's
  state.
- **A pad that goes** is closed when its read fails, at the next poll.

### The Switch 2 Pro Controller

Over USB the pad is a composite device: a HID interface, which carries its
input reports and, as output reports, its rumble; a vendor interface, bound to
WinUSB by the pad's own Microsoft OS descriptors, so no driver is installed;
and an audio interface. **The pad sends no input reports until a host has set
it up** through the vendor interface:

1. Read the sticks' calibration from its flash: the factory's, and the
   player's own where a console wrote one (`0x130a8`, `0x130e8`, `0x1fc040`,
   `0x1fc080`).
2. Choose the features its reports carry (buttons, sticks and rumble, not
   motion) and turn them on.
3. Set up rumble, choose the Pro Controller's own report (`0x09`), and light
   the first player light.
4. Start the reports.

The backend opens the vendor interface, found in the same device container as
the HID interface, while the driver sets the pad up, and closes it after, so
that other programs can have it. **WinUSB lets one program have it at a
time.** While Steam runs, Steam has it and has set the pad up already: the pad
is read as Steam set it up, in the report every Switch 2 controller has
(`0x05`), which the driver reads too, and the sticks use their default
calibration, a center of 2048 and a reach of 1500 either way, since the flash
cannot be read. Either way the pad's sticks rest near 0 and reach 1.

The facts of the protocol are from the community's research of these pads
([ndeadly's switch2_controller_research](https://github.com/ndeadly/switch2_controller_research))
and the drivers that use it, Linux's `hid-nintendo` and SDL's; the code is
the engine's own.

## Sticks and triggers as buttons

A menu wants a stick's directions as buttons, and a game may want a trigger
as one. `tb_gamepad_analog_button()` turns a value into a button with two
thresholds: down at `press` and above, up again only below `release`. A
value that hovers at the threshold, as a stick at rest on its edge or a
trigger held half down does, keeps the button as it was instead of pressing
it again and again. `tb_gamepad_virtual_buttons()` does it for every
direction of both sticks and both triggers, from a state and the bits it
returned last time:

```c
static uint32_t held[TB_GAMEPAD_SLOT_COUNT];
held[slot] = tb_gamepad_virtual_buttons(&state, held[slot], NULL);
if (held[slot] & (1u << TB_GAMEPAD_VIRTUAL_LEFT_STICK_UP)) { ... }
```

| | Press | Release | Why |
| --- | --- | --- | --- |
| A stick's direction | 0.5 | below 0.4 | Unreal presses `Gamepad_LeftStick_Up` and the like at XInput's dead zones (about 0.24 and 0.27), without hysteresis, which presses the side of a stick pushed some 75 degrees from it. At 0.5, Enhanced Input's default actuation, a direction is down when an action on the axis would be actuated, and a stick at full tilt presses the directions within 60 degrees of it: both of a diagonal, one of a push along an axis. 0.1 of hysteresis is well above a stick's noise. |
| A trigger | 0.2 | below 0.1 | Unreal's `Gamepad_LeftTrigger` is down past XInput's `XINPUT_GAMEPAD_TRIGGER_THRESHOLD`, 30 of 255 (0.12), without hysteresis. A trigger is squeezed deliberately, so it presses early, as there; a little later, so that a worn trigger at rest never presses, and released below XInput's threshold. |

A direction is of one axis alone, as Unreal's: a diagonal presses two. The
thresholds are applied to the pad's values, before any dead zone, and
`tb_gamepad_virtual_thresholds` gives others. The input module makes the same
buttons as keys (`gamepad_left_stick_up`, `gamepad_right_trigger_button`), with
these defaults ([input](input.md#keys)).

## Rumble

`tb_gamepad_rumble()` runs a pad's two motors for a time, in place of what
they ran: the low-frequency one, the large motor in the left grip, and the
high-frequency one, the small motor in the right. Each is from 0 to 1,
multiplied by the system's **vibration**, which a game sets from its
`controls.gamepad.vibration` setting with `tb_gamepad_system_set_vibration()`
([settings](settings.md#controls)); a change rescales what runs, for the time
it has left. A rumble lasts at most an hour.

Rumble stops:

- when its time is up, at the first poll after;
- when its pad is unplugged; the next pad in the slot does not inherit it;
- when the game loses the focus: `tb_gamepad_system_set_focus(system,
  false)`, from the window's `TB_WINDOW_EVENT_FOCUS_LOST`, stops every pad and
  starts none until the focus is back;
- when the system is closed.

| Platform | Low- and high-frequency motors | Trigger motors |
| --- | --- | --- |
| Windows | `XInputSetState`: the left and right motors | Unavailable: the Xbox One pad's impulse triggers need Windows.Gaming.Input, a WinRT interface the engine does not use yet |
| Windows, Switch 2 Pro Controller | Its two linear actuators, one in each grip, each driven at a low and a high frequency for the two motors | Unavailable |
| Linux | An `FF_RUMBLE` effect, uploaded with `EVIOCSFF` and played by writing it, on pads whose driver reports `FF_RUMBLE` and whose node can be written | Unavailable |
| macOS | Unavailable for now: the framework's haptics need Core Haptics, which the engine does not drive yet | Unavailable |

**The Switch 2 Pro Controller** plays a rumble report for a few milliseconds
and then stops, so the backend's rumble thread sends it again every 8
milliseconds while the motors run, and stops sending when the rumble's time is
up, even when the game stops polling. Its full strength is held to 450 of the
actuators' 1023, as Linux and SDL hold it: the drivers warn that more is
uncomfortable and may harm the pad.

Where a pad cannot rumble, its info's capabilities leave out
`TB_GAMEPAD_CAPABILITY_RUMBLE`, and `tb_gamepad_rumble()` returns false: an
unavailable capability, never a fault. No pad reports
`TB_GAMEPAD_CAPABILITY_TRIGGER_RUMBLE` yet.

## The system's buttons

The guide button (Xbox, PS, Steam) and the share or capture button belong to
the operating system and the launcher: Game Bar, the Steam overlay,
screenshots and recording, and macOS's gestures for them. The engine never
claims them, so they keep working while a game runs:

- **Windows** reads with `XInputGetState()`, which leaves the guide button
  out. It never calls the undocumented `XInputGetStateEx()` (ordinal 100),
  which would report it. XInput has no share button. A raw HID pad's driver
  reads none of the system's buttons: the Switch 2's Home, Capture and C.
- **Linux** reads devices without `EVIOCGRAB`, so the desktop and Steam see
  every pad too, and never reports `BTN_MODE` or the share button's
  `KEY_RECORD`.
- **macOS** never reads `buttonHome` or the share button, and leaves their
  `preferredSystemGestureState` at its default, enabled.

No state has a guide button, and the input module's `gamepad_guide` key is
never given by a gamepad of the engine.

## Threading

A gamepad system is **stage-owned**: it is opened, polled, read, rumbled and
closed on one thread, the main thread, which on macOS the GameController
framework requires. The functions that take no system
(`tb_gamepad_button_down()`, `tb_gamepad_analog_button()`,
`tb_gamepad_virtual_buttons()` and the names) are thread-safe.

## Known gaps

- Trigger motors on Windows need Windows.Gaming.Input (#364).
- Rumble on macOS needs Core Haptics through `GCController.haptics` (#363).
- XInput reports at most four pads, gives no names beyond a kind, and no
  USB vendor or product; PlayStation and Switch pads reach it only through
  Steam Input.
- Raw HID drivers exist for the Switch 2 Pro Controller over USB only. The
  Joy-Con 2 pair and the GameCube controller for Switch 2 speak the same
  protocol with reports and rumble of their own (#378); the Switch 2 pads
  over Bluetooth, and DualSense and Switch Pro pads without Steam, need
  drivers of their own.
- Linux before 7.4 has no driver for the Switch 2 controllers, and the
  engine does not set them up itself through hidraw and usbfs (#380).
- macOS's GameController framework does not know the Switch 2 controllers;
  an IOKit HID backend with the same drivers would read them (#379).
- Gyros, touchpads and lights are not read; a Switch 2 pad's player light is
  the first.
