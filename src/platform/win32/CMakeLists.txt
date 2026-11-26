set(TB_PLATFORM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/virtual_memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/fs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/abort.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/crash.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/error_stream.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/library.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/process.c"
    # A loaded library's record, the checks of its name, and its messages.
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/library.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stack.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/atomic.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/random.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/system.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/thread.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/time.c"
    # Shared by every adapter that names threads.
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/name.c"
    # The line that says what a crash was, without the C library.
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/crash_text.c"
    # A semaphore is built from the adapter's mutex and condition variable.
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/semaphore.c"
    # Waiting for a moment is built from the adapter's sleep and clock.
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/wait.c"
    CACHE INTERNAL "Selected platform adapter sources")
target_sources(badger PRIVATE ${TB_PLATFORM_SOURCES})
target_sources(badger_static PRIVATE ${TB_PLATFORM_SOURCES})

# Windows and displays: the shared front, then Win32 behind it. Windowing is
# built on the foundation modules, which the adapter beneath them is not, so
# it is not part of the adapter's own sources and tests.
set(TB_WINDOW_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/window.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/window_size.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/window.c"
    CACHE INTERNAL "Selected windowing sources")
target_sources(badger PRIVATE ${TB_WINDOW_SOURCES})
target_sources(badger_static PRIVATE ${TB_WINDOW_SOURCES})

# Gamepads: the shared front, then XInput and raw HID behind it, their
# libraries loaded at run time, with the HID pads' protocol drivers. They
# are built on the foundation modules, as windowing is.
set(TB_GAMEPAD_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/gamepad.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/gamepad_mux.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/gamepad_hid.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/gamepad_switch2.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gamepad.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gamepad_rawhid.c"
    CACHE INTERNAL "Selected gamepad sources")
target_sources(badger PRIVATE ${TB_GAMEPAD_SOURCES})
target_sources(badger_static PRIVATE ${TB_GAMEPAD_SOURCES})

# The shader compilers: DXC, loaded at run time and called through COM, and
# D3DCompiler, part of Windows, loaded from the system directory. They are
# built on the foundation modules, as windowing is, so they are not part of
# the adapter's own sources and tests.
set(TB_DXC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/dxc.c" "${CMAKE_CURRENT_SOURCE_DIR}/d3dcompiler.c"
    CACHE INTERNAL "Selected shader compiler sources")
target_sources(badger PRIVATE ${TB_DXC_SOURCES})
target_sources(badger_static PRIVATE ${TB_DXC_SOURCES})
configure_file(api.h.in "${PROJECT_BINARY_DIR}/generated/badger/api.h" @ONLY)
configure_file(thread_storage.h.in
    "${PROJECT_BINARY_DIR}/generated/badger/thread_storage.h" COPYONLY)
