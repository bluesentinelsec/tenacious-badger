set(TB_PLATFORM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/virtual_memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../posix/virtual_memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/fs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../posix/fs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fs_location.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/abort.c"
    # Catching crashes and writing to stderr are the same on Linux and macOS,
    # except where a signal's context says which instruction raised it.
    "${CMAKE_CURRENT_SOURCE_DIR}/../posix/crash.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/crash_context.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../posix/error_stream.c"
    # Loading a library is the same on Linux and macOS, except where the
    # system's own libraries are.
    "${CMAKE_CURRENT_SOURCE_DIR}/../posix/library.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/library_system.c"
    # The arguments are main's own on Linux and macOS.
    "${CMAKE_CURRENT_SOURCE_DIR}/../posix/process.c"
    # A loaded library's record, the checks of its name, and its messages.
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/library.c"
    # Capturing a stack and naming an address are the same on Linux and macOS.
    "${CMAKE_CURRENT_SOURCE_DIR}/../posix/stack.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/atomic.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/condition.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/processors.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/random.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/system.c"
    # Naming a thread and giving it a priority are not part of POSIX.
    "${CMAKE_CURRENT_SOURCE_DIR}/thread_native.c"
    # Shared by every adapter that names threads.
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/name.c"
    # The line that says what a crash was, without the C library.
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/crash_text.c"
    # A semaphore is built from the adapter's mutex and condition variable.
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/semaphore.c"
    # Waiting for a moment is built from the adapter's sleep and clock.
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/wait.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/time.c"
    # POSIX threads are the same on Linux and macOS, except where a condition
    # variable waits for a limited time: that part is condition.c.
    "${CMAKE_CURRENT_SOURCE_DIR}/../posix/thread.c"
    CACHE INTERNAL "Selected platform adapter sources")
target_sources(badger PRIVATE ${TB_PLATFORM_SOURCES})
target_sources(badger_static PRIVATE ${TB_PLATFORM_SOURCES})

# Windows and displays: the shared front, then Wayland and X11 behind it, each
# loaded at run time. Windowing is built on the foundation modules, which the
# adapter beneath them is not, so it is not part of the adapter's own sources
# and tests.
set(TB_WINDOW_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/window.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/window_size.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/window.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/window_wayland.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/window_x11.c"
    CACHE INTERNAL "Selected windowing sources")
target_sources(badger PRIVATE ${TB_WINDOW_SOURCES})
target_sources(badger_static PRIVATE ${TB_WINDOW_SOURCES})

# Gamepads: the shared front, then evdev behind it: the devices of
# /dev/input, watched with inotify. What a device's codes mean is standard
# C, in gamepad_evdev.c. They are built on the foundation modules, as
# windowing is.
set(TB_GAMEPAD_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/gamepad.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gamepad.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gamepad_evdev.c"
    CACHE INTERNAL "Selected gamepad sources")
target_sources(badger PRIVATE ${TB_GAMEPAD_SOURCES})
target_sources(badger_static PRIVATE ${TB_GAMEPAD_SOURCES})

# DXC, which is unavailable here: DXIL runs only on D3D12. It is built on the
# foundation modules, as windowing is.
set(TB_DXC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../posix/dxc.c"
    CACHE INTERNAL "Selected DXC sources")
target_sources(badger PRIVATE ${TB_DXC_SOURCES})
target_sources(badger_static PRIVATE ${TB_DXC_SOURCES})
