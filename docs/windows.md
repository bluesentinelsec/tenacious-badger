# Windows and displays

`window.h` is the platform interface for the window a game draws into, the
displays it may fill, and the events the system sends it. Every desktop
platform implements it; any other platform's build reports it as not
implemented.

| Platform | Windowing | How it is reached |
| --- | --- | --- |
| Windows | Win32, per-monitor DPI aware | user32 and gdi32, which the toolchain links by default; shcore loaded at run time |
| Linux and Steam Deck | Wayland where a compositor runs, else X11 | `libwayland-client.so.0`, `libX11.so.6` and `libXrandr.so.2`, loaded at run time; `libwayland-cursor.so.0`, `libxkbcommon.so.0` and `libXi.so.6` too, where they are, for the mouse and text |
| macOS | Cocoa | `libobjc.A.dylib`, AppKit and CoreGraphics, loaded at run time |

A machine without the libraries, or without a display server, gets
`TB_WINDOW_UNAVAILABLE` from `tb_window_system_open()`: a capability that is
missing, never a crash. A program can ask for a backend: X11 under a Wayland
desktop runs through Xwayland, for instance.

## Modes

| Mode | Windows | Linux | macOS |
| --- | --- | --- | --- |
| Windowed | A framed window, centered on its display | Framed; centered on X11, placed by the compositor on Wayland | Framed, centered |
| Borderless | A frameless window covering its display | Fullscreen as the compositor or window manager does it | A frameless window above the menu bar, with the Dock and menu bar hidden |
| Fullscreen | The display in the game's own resolution and refresh rate, given back while the player is in another program | Borderless | Borderless |

Exclusive fullscreen of a game's own resolution is a Windows capability: on
the other platforms a window asked for it is borderless, and its state says
so. Modes, displays and sizes change at any time with `tb_window_configure()`.

Known gaps, each a Wayland limitation or work for later:

- A Wayland client cannot place its windows, so a new window is wherever the
  compositor puts it.
- Wayland windows have decorations only where the compositor offers
  xdg-decoration; GNOME does not, so a windowed Burrow there has no frame.
- Wayland scale is the integer scale of the outputs a window is on;
  fractional scales arrive with `wp_fractional_scale_v1`.
- Wayland reports no minimizing.
- X11 reports no refresh rate.
- Linux has no system dialog to ask a question with, so `tb_window_confirm()`
  is unavailable there until the engine's interface draws its own.

## Sizes and scale

A window's size is in logical units; its pixels are that size times its
display's content scale. A renderer renders at the pixels, and an interface
lays itself out in logical units, so text is as large on a 4K monitor at 200%
as on a 1080p one at 100%. Moving a window to a display of another scale
sends `TB_WINDOW_EVENT_SCALE_CHANGED` with the new pixels.

Each display reports its physical size where the system knows it. The Steam
Deck reports a scale of 1 for a 7-inch 1280x800 screen, so an interface that
uses only the scale would be small there; the physical size lets it be
readable at arm's length.

A window given no size is the largest 16:9 size up to 1280 by 720 that fits in
85% of its display's usable area. A display index that no display has, as a
settings file may name a monitor since unplugged, is the primary display.

## Events

`tb_window_system_poll()` reads what the system sent and returns it as events:
close requests, which leave the window open for the game to decide; resizes
and scale changes; focus; minimizing; display changes; keys; the mouse; and
text. Keys are named by their place on the keyboard, whatever its layout,
with the system's own code alongside for keys `tb_key` does not name, and
the modifiers held; the full key set arrives with the input module.

`tb_window_system_wait()` blocks until a message comes, and another thread
ends the wait with `tb_window_system_wake()`.

## The mouse and text

A window reports the mouse's buttons (left, right, middle, and the two side
buttons), its motion, its wheel, and its entering and leaving. Each position
is from the client area's top-left corner, in logical units and in pixels,
whichever the system reads it in: Windows and X11 read pixels, Wayland and
macOS logical units. While a button is down the window holds the mouse, so a
drag that leaves it still reports. The wheel's turn is in notches, with
fractions from a wheel or touchpad of finer resolution; positive `delta_y`
scrolls up.

Typed text comes as `TB_WINDOW_EVENT_TEXT_INPUT`, apart from keys, in UTF-8
that the system composed: a dead key and the letter after it are one
character. Control characters (Backspace, Enter, Tab, Escape) are keys and
never text.

| | Windows | X11 | Wayland | macOS |
| --- | --- | --- | --- | --- |
| Mouse | Window messages | Core events | `wl_pointer` | The content view's mouse events |
| Wheel | `WM_MOUSEWHEEL`, in 120ths of a notch | Buttons 4 to 7, whole notches | `axis_value120`, else the axis distance | Scrolling deltas; a touchpad's in points |
| Relative motion | Raw Input, unaccelerated | XInput2 raw motion, unaccelerated | `zwp_relative_pointer_v1`, unaccelerated | The events' deltas, accelerated by the system |
| Holding the mouse | `ClipCursor` while focused | A pointer grab while focused | `zwp_locked_pointer_v1` | `CGAssociateMouseAndMouseCursorPosition` while key |
| Cursor shapes | The system's cursors | The cursor font, which a theme draws | `wp_cursor_shape_v1`, else the cursor theme | `NSCursor` |
| Text | `WM_CHAR`, surrogate pairs joined | The X input method (`Xutf8LookupString`) | xkbcommon's keymap and compose sequences | The input context, through `NSTextInputClient` |

Relative mouse mode, for a first-person camera, is turned on with
`tb_window_set_relative_mouse()`: the cursor is hidden and held in the
window, and `TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION` replaces
`TB_WINDOW_EVENT_MOUSE_MOVED`. The system lets the cursor go while the player
is in another window. A Wayland compositor without the relative-pointer and
pointer-constraints protocols, or a seat without a mouse, has no such mode:
the call says it is unavailable. `tb_window_set_cursor()` and
`tb_window_show_cursor()` set the cursor over the client area.

Known gaps:

- Input method editors' compositions (Chinese, Japanese, Korean) are not
  reported as they are composed; their committed text arrives as text.
- Wayland compositors do not repeat keys; a held key types its text once.
- X11's wheel is in whole notches; XInput2's smooth scrolling is not read.
- Wayland's theme cursors, without `wp_cursor_shape_v1`, are drawn at 24
  units in the default theme, not scaled to the display.

## Threading

Every function but `tb_window_system_wake()` belongs to the thread that opened
the window system, which on macOS must be the main thread.

## Testing

The window tests run every case on each windowing that opens. CI's Linux
coverage job starts an X server (Xvfb) and a Wayland compositor (Weston,
headless) so that both backends are run; the Windows and macOS runners have
displays of their own. On Windows the message handling is also tested by
sending a window the messages Windows would; on X11 by sending the window
events through the X server; and on macOS by giving the content view events
made as AppKit makes them. Raw Input, which Windows sends only to the window
in front, and the Wayland seat, which the headless compositor lacks, are
tested through the functions that translate them.
