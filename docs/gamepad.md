# Gamepads

`gamepad.h` is the platform interface for the controllers a player holds. It
reads every gamepad the platform reports, in one standard layout, and rumbles
the pads that have motors. Every desktop platform implements it; any other
platform's build reports it as not implemented.

| Platform | Gamepads | How it is reached |
| --- | --- | --- |
| Windows | XInput: Xbox pads, and what Steam Input exposes, under Proton too | `xinput1_4.dll`, or `xinput9_1_0.dll` before Windows 8, loaded at run time from the system directory |
| Windows | Raw HID: the pads a protocol driver of the engine's knows, which XInput does not read ([raw HID pads](#raw-hid-pads)), over USB and Bluetooth | `cfgmgr32.dll`, `hid.dll` and `winusb.dll`, loaded at run time from the system directory |
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
| DualSense (`054c:0ce6`), USB and Bluetooth | Raw HID, with rumble and lights | hid-playstation, from Linux 5.12, with rumble | GameController, from macOS 11.3 |
| DualSense Edge (`054c:0df2`), USB and Bluetooth | Raw HID, with its back buttons, rumble and lights | hid-playstation, from Linux 6.2; its back buttons from Linux 7.2 | GameController, without its back buttons |
| DualShock 4 | Through Steam Input (#383) | hid-playstation | GameController |
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
"Switch 2 Pro Controller" or "DualSense Wireless Controller", for a raw HID
pad; the driver's name on Linux, as
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

### PlayStation's pads

PlayStation pads already have their face buttons where Xbox pads have
theirs. Their middle is laid out differently: one button each side of the
touchpad, and the PS and mute buttons below it.

| Place | Xbox | DualSense | Engine |
| --- | --- | --- | --- |
| Bottom, right, left, top | A, B, X, Y | Cross, Circle, Square, Triangle | `south`, `east`, `west`, `north` |
| Shoulders | LB, RB | L1, R1 | `left_shoulder`, `right_shoulder` |
| Triggers | LT, RT | L2, R2, analog | `left_trigger`, `right_trigger` |
| Sticks, pressed | LS, RS | L3, R3 | `left_stick`, `right_stick` |
| Middle, left and right | View, Menu | Create, Options | `back`, `start` |
| Back buttons | Elite's P3, P1 | Edge's left and right back buttons | `left_paddle_1`, `right_paddle_1` |
| System | Guide, Share | PS, mute | Never read |
| No place in the layout | | The touchpad's click and touches, the Edge's Fn buttons, motion | Not read (#384) |

- **Create is `back`.** It is the one button left of the middle, where
  every other pad has Back, View or Select, and PC games read it so: Linux
  gives it as `BTN_SELECT`, the GameController framework as `buttonOptions`,
  SDL as Back. Xbox's Share button, which `back` leaves to the system, is a
  button of its own beside View; a DualSense has none, and a game would
  have no `back` without Create. On a PlayStation console Create takes
  screenshots; on Windows and Linux no system listens for it, and Steam's
  screenshot is a chord with the PS button, which stays the system's. On
  macOS the framework gives Create as `buttonOptions`, which the backend
  has read as `back` since it was written.
- **PS is the guide button**, and never read.
- **Mute is the system's.** It mutes the pad's microphone in its own
  firmware and lights its LED, and Linux keeps it as hid-playstation's own;
  it sits below PS where Xbox's Share does. Never read.
- **The touchpad's click** has no place in the layout, which has no button
  a game could count on there: not read, with the touchpad's touches
  (#384). The **Edge's Fn buttons** switch the pad's own profiles, held
  with another button: not read.
- **L2 and R2** are read from their analog values, from 0 to 1. Their
  digital bits, which the pad sets partway down, are not: the analog value
  carries them, and `tb_gamepad_virtual_buttons()` presses the trigger as a
  button at a threshold of the engine's own.

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
  `ABS_Z` and `ABS_RZ` (xpad, hid-playstation, which gives `BTN_TL2` and
  `BTN_TR2` beside them, not read), `ABS_HAT2Y` and `ABS_HAT2X`
  (hid-steam, the Steam Deck), `ABS_BRAKE` and `ABS_GAS`, or the digital
  `BTN_TL2` and `BTN_TR2`, at 0 or 1 (hid-nintendo).
- **The d-pad.** `ABS_HAT0X` and `ABS_HAT0Y`, the `BTN_DPAD_*` buttons, or
  xpad's first four `BTN_TRIGGER_HAPPY` buttons.
- **The paddles.** The Steam Deck's L4, R4, L5, R5 (`BTN_GRIPL`, `BTN_GRIPR`,
  `BTN_GRIPL2`, `BTN_GRIPR2`), the Xbox Elite pad's P1 to P4
  (`BTN_TRIGGER_HAPPY5` to `8`), and the DualSense Edge's back buttons,
  which hid-playstation gives from Linux 7.2 as `BTN_TRIGGER_HAPPY3` and
  `4`, the upper paddles; its Fn buttons, `BTN_TRIGGER_HAPPY1` and `2`, are
  not read.
- **The DualSense** (`054c:0ce6`, and the Edge, `0df2`) is three devices on
  hid-playstation: the pad, read by place as every Sony pad, with Create as
  `BTN_SELECT` and PS as `BTN_MODE`, not read; its touchpad, whose only key
  is `BTN_LEFT`, and its motion sensors, which have no keys, are not
  gamepads. The mute button is the driver's own and is no event.
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
pad is a new driver, and the backend does not change. Two families are
written: the Switch 2 Pro Controller's (`gamepad_switch2.c`) and the
DualSense's (`gamepad_dualsense.c`). Every driver is tested on every
platform, with reports captured from a real pad.

- **Finding pads.** The HID device interfaces are listed every half second
  with `CM_Get_Device_Interface_ListW`, which takes about a tenth of a
  millisecond, rather than through a notification that would call back on
  another thread. Only interfaces whose path names the vendor and product of
  a pad a driver knows are opened: over USB as `VID_054C&PID_0CE6`; over
  Bluetooth, whose HID interfaces are under the HID service's class
  `{00001124-0000-1000-8000-00805F9B34FB}`, as `VID&0002054C_PID&0CE6`.
  Windows' own HID driver reads Bluetooth pads, so the backend reaches them
  through `hid.dll` as it does USB pads, and tells the driver which it is.
- **Not twice.** XInput's own HID collections, whose paths have `IG_` in them,
  are never opened: XInput reads those. Nor are the pads Steam names in
  `SDL_GAMECONTROLLER_IGNORE_DEVICES`.
- **One pad, two ways.** A pad connected over a cable and Bluetooth at once
  has a HID interface for each. A driver that knows which physical pad it
  has says so once the pad is started (the DualSense's gives its Bluetooth
  address, which it reports over both). An interface whose pad is open
  already through another is closed again, and passed over at later
  listings while that one stays open; when it goes, the other is opened at
  the next listing. USB interfaces are opened before Bluetooth ones of the
  same listing, so a pad found both ways at once is read over its cable.
- **Reading.** Each pad has one overlapped read outstanding, of the length
  the device's descriptor gives its longest input report
  (`HidP_GetCaps`). A poll takes every report that came since the last
  without waiting, and the last is the pad's state. Output reports are
  written at the descriptor's length too, a shorter one followed by zeros,
  as Windows requires, and feature reports read with `HidD_GetFeature`.
- **Lights.** When a pad comes into a slot, the system tells its backend the
  slot, and a driver whose pad has lights shows it.
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

### The DualSense

The DualSense (`054c:0ce6`) and the DualSense Edge (`054c:0df2`) speak one
protocol, over USB and Bluetooth; the Edge adds its back buttons and Fn
buttons to a byte the DualSense leaves empty. Nothing sets a pad up before
it reports, and nothing needs a vendor channel.

| | USB | Bluetooth |
| --- | --- | --- |
| Input | Report `0x01`, 64 bytes, from the start | The simple report `0x01` (sticks, buttons, triggers) until the calibration feature report `0x05` is read; then the full report `0x31`, 78 bytes: USB's data two bytes on, and a CRC-32 |
| Output | Report `0x02`, 48 bytes | Report `0x31`, 78 bytes: a number from 0 to 15, a tag, USB's data, and a CRC-32 |
| Feature | `0x05` calibration, `0x09` the pad's Bluetooth address, `0x20` its firmware | The same, each ending in a CRC-32 |

Over Bluetooth every report ends in the CRC-32 of a seed byte and the rest
of the report: `0xa1` for input, `0xa2` for output, `0xa3` for feature
reports. A full input report whose CRC is wrong is dropped, and a feature
report whose CRC is wrong is not believed.

When a pad starts, the driver reads its calibration over Bluetooth, which
turns on the full reports; its Bluetooth address, which is its identity on
both transports; and its firmware's update version (byte 44 of `0x20`).
Then it ends the firmware's own fade of the light bar, which would hide
the player's color.

- **Rumble.** The pad has two voice-coil actuators, which its firmware
  drives as an older pad's two motors when an output report asks it to: the
  left grip's is the low-frequency motor and the right's the high, each a
  byte from 0 to 255. Firmware from update 2.21, and every Edge, takes the
  newer flag for it (`0x04` in the report's third byte of flags), older
  firmware the first (`0x01`); both select the motors over audio haptics
  (`0x02`). The motors run until the next report, so nothing is sent again
  while they run; the system's stop rules send the report that stops them.
- **Lights.** A pad that comes into a slot shows its player as PlayStation
  consoles do: the five player lights below the touchpad (the middle one
  for the first player, then two, three, four and all five), and the light
  bar dimly in blue, red, green or pink for the first four, then orange,
  teal, yellow and white. Players past the fifth light as the first again,
  each with a color of their own. A game cannot set them yet (#385).
- **Steam.** While Steam runs, with its PlayStation support on, it opens the
  pad too and may set its lights; Windows gives each program the pad's
  reports, so both read it. A game Steam starts is given the pad's IDs in
  `SDL_GAMECONTROLLER_IGNORE_DEVICES` and reads Steam's XInput pad instead,
  so the pad is seen once.

The facts of the protocol are from the drivers that use it, Linux's
`hid-playstation` and SDL's, and from reports read from a real pad; the
code is the engine's own. The tests (`tests/platform/gamepad_dualsense_test.c`)
read a report captured from a DualSense over USB, and the same state as
Bluetooth's full report carries it.

## Families

Every pad has the same layout, but not the same labels: south is A, Cross
or B. A pad's info gives its **family**, `tb_gamepad_info`'s `family`, so
that a game shows the labels printed on the pad in hand
([prompts](input.md#prompts)):

| Family | `tb_gamepad_family` | Which pads |
| --- | --- | --- |
| Xbox | `TB_GAMEPAD_FAMILY_XBOX` | Microsoft's (`045e`), every XInput pad, and pads the GameController framework calls Xbox |
| PlayStation | `TB_GAMEPAD_FAMILY_PLAYSTATION` | Sony's (`054c`), and the framework's DualShock and DualSense |
| Nintendo | `TB_GAMEPAD_FAMILY_NINTENDO` | Nintendo's (`057e`), and the framework's Switch and Joy-Con pads |
| Steam Deck | `TB_GAMEPAD_FAMILY_STEAM_DECK` | The Deck's own controls (`28de:1205`), and on a Deck, the pad Steam stands in for |
| Generic | `TB_GAMEPAD_FAMILY_GENERIC` | Any other, and Steam's virtual pad (`28de:11ff`) off a Deck |

The family comes from what the system says of a pad: XInput's pads are
Xbox pads, or made to be read as one; raw HID pads and Linux's have their
USB IDs (`tb_gamepad_family_from_usb()`); the GameController framework
names a category, as "DualSense" or "Switch Pro Controller", read by its
words.

**The Steam Deck.** A game Steam starts on a Deck reads the Deck's controls
through Steam's virtual pad: an XInput pad under Proton, `28de:11ff` on
Linux. Steam sets `SteamDeck=1` in such a game's environment, and the
system reads it when it opens: on a Deck, a pad Steam stands in for, which
has Steam's virtual IDs or none, is in the Steam Deck family, since a game
on a Deck is played with its own controls. A pad of its own IDs, as a
DualSense read through raw HID, keeps its family. Off a Deck, Steam's
virtual pad may stand for any pad, and is generic; Steam says which in a
file named by `SteamVirtualGamepadInfo`, which the engine does not yet read
(see [known gaps](#known-gaps)).

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
| Windows, DualSense and DualSense Edge | Its two actuators, one in each grip, as the firmware's two-motor emulation drives them, over USB and Bluetooth | Unavailable: the adaptive triggers are motors of another kind (#386) |
| Linux | An `FF_RUMBLE` effect, uploaded with `EVIOCSFF` and played by writing it, on pads whose driver reports `FF_RUMBLE` and whose node can be written: hid-playstation does for the DualSense from Linux 5.12, with the same emulation | Unavailable |
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
  reads none of the system's buttons: the Switch 2's Home, Capture and C,
  the DualSense's PS and mute. Raw HID pads are opened shared, so Steam
  and the system read them too.
- **Linux** reads devices without `EVIOCGRAB`, so the desktop and Steam see
  every pad too, and never reports `BTN_MODE` or the share button's
  `KEY_RECORD`.
- **macOS** never reads `buttonHome` or the share button, and leaves their
  `preferredSystemGestureState` at its default, enabled. On a DualSense
  the framework gives PS as `buttonHome`, which is not read, Options as
  `buttonMenu` and Create as `buttonOptions`, read as `start` and `back`.

The DualSense's Create button is read as `back`, not left to the system as
Xbox's Share is: see [PlayStation's pads](#playstations-pads).

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
- Raw HID drivers exist for the Switch 2 Pro Controller over USB, and the
  DualSense and DualSense Edge over USB and Bluetooth. The Joy-Con 2 pair
  and the GameCube controller for Switch 2 speak the same protocol with
  reports and rumble of their own (#378); the Switch 2 pads over Bluetooth,
  the DualShock 4 (#383), and Switch Pro pads without Steam need drivers of
  their own.
- The DualSense's touchpad, motion sensors and microphone mute state are
  not read (#384); its lights show the player and cannot be set by a game
  (#385); its adaptive triggers are not driven (#386). Bluetooth reports
  were tested against reports made from a USB capture: no Bluetooth
  capture of a real pad is in the tests yet.
- macOS reads the DualSense Edge's back buttons only if the framework gives
  them as an Xbox Elite pad's paddles, which it does not.
- Linux before 7.4 has no driver for the Switch 2 controllers, and the
  engine does not set them up itself through hidraw and usbfs (#380).
- macOS's GameController framework does not know the Switch 2 controllers;
  an IOKit HID backend with the same drivers would read them (#379).
- Steam's virtual pad off a Steam Deck is generic: the pad it stands for is
  in the file `SteamVirtualGamepadInfo` names, which is not read yet; nor
  does a Deck's family tell its controls from another pad plugged into it,
  which Steam also stands in for.
- Gyros, touchpads and lights are not read; a Switch 2 pad's player light is
  the first, and a DualSense's lights show its slot.
