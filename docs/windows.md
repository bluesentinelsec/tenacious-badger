# Windows and displays

`window.h` is the platform interface for the window a game draws into, the
displays it may fill, and the events the system sends it. Every desktop
platform implements it; any other platform's build reports it as not
implemented.

| Platform | Windowing | How it is reached |
| --- | --- | --- |
| Windows | Win32, per-monitor DPI aware | user32 and gdi32, which the toolchain links by default; shcore loaded at run time |
| Linux and Steam Deck | Wayland where a compositor runs, else X11 | `libwayland-client.so.0`, `libX11.so.6` and `libXrandr.so.2`, loaded at run time |
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
and scale changes; focus; minimizing; display changes; and keys. Keys are
named by their place on the keyboard, whatever its layout, with the system's
own code alongside for keys `tb_key` does not name; text input and the full
key set arrive with the input module.

`tb_window_system_wait()` blocks until a message comes, and another thread
ends the wait with `tb_window_system_wake()`.

## Threading

Every function but `tb_window_system_wake()` belongs to the thread that opened
the window system, which on macOS must be the main thread.

## Testing

The window tests run every case on each windowing that opens. CI's Linux
coverage job starts an X server (Xvfb) and a Wayland compositor (Weston,
headless) so that both backends are run; the Windows and macOS runners have
displays of their own. On Windows the message handling is also tested by
sending a window the messages Windows would.
