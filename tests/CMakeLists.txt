add_subdirectory(framework)

# Embeds the files of a directory of tests/data that match a glob, as the array
# NAME of tb_test_file (see support/test_data.h), in the C source that OUTPUT
# names, to add to a test executable's sources. Several executables share the
# source, so one target makes it, and each of them waits for that target: with
# Makefiles, executables that each ran the command would race to write it.
function(tb_embed_test_data output name directory pattern)
    set(source_directory "${CMAKE_CURRENT_SOURCE_DIR}/data/${directory}")
    file(GLOB files CONFIGURE_DEPENDS "${source_directory}/${pattern}")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/test_data/${name}.c")
    add_custom_command(OUTPUT "${source}"
        COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${source}" "-DNAME=${name}"
            "-DDIRECTORY=${source_directory}" "-DPATTERN=${pattern}"
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedTestData.cmake"
        DEPENDS ${files} "${PROJECT_SOURCE_DIR}/cmake/EmbedTestData.cmake"
        COMMENT "Embedding test data ${directory}/${pattern}"
        VERBATIM)
    add_custom_target(${name}_data DEPENDS "${source}")
    set_property(GLOBAL APPEND PROPERTY TB_TEST_DATA_TARGETS ${name}_data)
    set(${output} "${source}" PARENT_SCOPE)
endfunction()

# JSONTestSuite's parsing corpus (MIT; see data/jsontestsuite/README.md).
tb_embed_test_data(TB_JSON_SUITE_SOURCE tb_json_suite_files jsontestsuite/test_parsing "*.json")

# Archives that GNU tar, bsdtar, and Python wrote (tools/make_tar_test_data.py).
tb_embed_test_data(TB_TAR_DATA_SOURCE tb_tar_files tar "*.tar*")

# Archives that Python, bsdtar, and .NET wrote (tools/make_zip_test_data.py).
tb_embed_test_data(TB_ZIP_DATA_SOURCE tb_zip_files zip "*.zip")

# Fonts of the author's font pack, under the licenses beside them (see data/fonts/README.md).
tb_embed_test_data(TB_FONT_DATA_SOURCE tb_font_files fonts "*.ttf")

# Shaders, and what DXC reports for them (tools/shader_reflection_goldens.py).
tb_embed_test_data(TB_SHADER_REFLECTION_SOURCE tb_shader_reflection_files shader_reflection "*")

# The cases of the embedded fonts expect what TB_EMBED_FONTS chose.
set(tb_embed_fonts_modes all defaults none)
list(FIND tb_embed_fonts_modes "${TB_EMBED_FONTS}" tb_embed_fonts_mode)
set_source_files_properties(font/font_catalog_test.c PROPERTIES
    COMPILE_DEFINITIONS "TB_TEST_EMBED_FONTS=${tb_embed_fonts_mode}")

# Every test case of the project, in one executable: tb_tests. Add the source
# of a new test here. Suites are named <module>_<feature>, so one pattern
# selects a module (tb_tests "core_*") or a feature (tb_tests "core_sum.*").
set(TB_TEST_CASE_SOURCES
    codec/base64_test.c
    codec/chacha20_test.c
    codec/checksum_test.c
    codec/deflate_test.c
    codec/inflate_test.c
    codec/package_test.c
    codec/sha256_test.c
    codec/tar_test.c
    ${TB_TAR_DATA_SOURCE}
    codec/zip_test.c
    ${TB_ZIP_DATA_SOURCE}
    core/allocator_test.c
    core/ap_test.c
    core/arena_test.c
    core/reserved_arena_test.c
    core/array_test.c
    core/assert_test.c
    core/atom_test.c
    core/bit_test.c
    core/cli_test.c
    core/cli_benchmark_test.c
    core/decimal_test.c
    core/exclusive_test.c
    core/fatal_test.c
    core/fmt_test.c
    core/frame_memory_test.c
    core/fs_test.c
    core/geometry_test.c
    core/hash_test.c
    core/list_test.c
    core/log_test.c
    core/math_test.c
    core/memory_test.c
    core/memory_debug_test.c
    core/path_test.c
    core/pool_test.c
    core/result_test.c
    core/ring_test.c
    core/rng_test.c
    core/set_test.c
    core/size_test.c
    core/sort_test.c
    core/str_test.c
    core/stream_test.c
    core/sum_test.c
    core/table_test.c
    core/text_test.c
    core/thread_safety_test.c
    core/utf8_test.c
    core/xp_test.c
    font/font_builder.c
    font/font_catalog_test.c
    font/font_test.c
    font/font_damage_test.c
    font/font_raster_test.c
    font/font_atlas_test.c
    font/font_layout_test.c
    ${TB_FONT_DATA_SOURCE}
    jobs/jobs_test.c
    media/bitmap_bmp_test.c
    media/bitmap_test.c
    media/sound_edit_test.c
    media/sound_test.c
    media/wav_builder.c
    media/wav_malformed_test.c
    media/wav_test.c
    media/wav_write_test.c
    serialize/json_test.c
    serialize/json_suite_test.c
    ${TB_JSON_SUITE_SOURCE}
    serialize/json_writer_test.c
    game/game_loop_test.c
    gui/gui_choice_test.c
    gui/gui_combobox_test.c
    gui/gui_deferred_test.c
    gui/gui_dialog_test.c
    gui/gui_draw_test.c
    gui/gui_entry_test.c
    gui/gui_event_test.c
    gui/gui_file_kinds_test.c
    gui/gui_file_test.c
    gui/gui_fixture.c
    gui/gui_grid_test.c
    gui/gui_listbox_test.c
    gui/gui_menu_test.c
    gui/gui_names_test.c
    gui/gui_navigation_test.c
    gui/gui_notebook_test.c
    gui/gui_popup_test.c
    gui/gui_range_test.c
    gui/gui_scale_test.c
    gui/gui_scroll_test.c
    gui/gui_style_test.c
    gui/gui_theme_file_test.c
    gui/gui_tree_test.c
    gui/gui_treeview_test.c
    gui/gui_var_test.c
    gui/gui_watch_test.c
    input/input_file_test.c
    input/input_test.c
    input/input_rules_test.c
    jobs/loop_test.c
    anim/anim_test.c
    platform/window_test.c
    settings/engine_settings_test.c
    settings/settings_test.c
    shader/shader_cache_test.c
    shader/shader_compile_test.c
    ${TB_SHADER_REFLECTION_SOURCE}
    render/canvas_slice_test.c
    render/canvas_test.c
    render/light_test.c
    render/material_test.c
    render/quality_test.c
    render/text_test.c
    render/viewport_test.c
    render/render_graph_test.c
    render/renderer_test.c
    render/skinning_test.c
    render/sky_test.c
    rhi/rhi_test.c
    ui/ui_draw_test.c
    ui/ui_fixture.c
    ui/ui_test.c
    ui/ui_text_field_test.c
    shader/shader_dxil_test.c
    shader/shader_library_test.c
    shader/shader_manifest_test.c
    shader/shader_preprocess_test.c
    vfs/vfs_test.c
    framework/selftest/checks_test.c
    framework/selftest/options_test.c
    framework/selftest/pattern_test.c
    framework/selftest/random_test.c)

# Builds an executable of engine test cases. Its entry point turns the engine's
# fatal conditions into test results and checks every case for leaks; see
# support/engine_test.h. The arguments go to tb_add_test_executable.
function(tb_add_tests target)
    tb_add_test_executable(${target}
        MAIN "${CMAKE_CURRENT_SOURCE_DIR}/support/engine_main.c"
        ${ARGN})
    target_sources(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/support/engine_memory.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/support/test_allocator.c")
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/support"
        # The framework's own cases test its internal interfaces.
        "${CMAKE_CURRENT_SOURCE_DIR}/framework/src")
    tb_configure_target(${target})
    get_property(data_targets GLOBAL PROPERTY TB_TEST_DATA_TARGETS)
    if(data_targets)
        add_dependencies(${target} ${data_targets})
    endif()
endfunction()

# Windowing is above the platform adapters, so its cases of one platform
# are cases of the engine's tests.
if(WIN32)
    list(APPEND TB_TEST_CASE_SOURCES platform/win32/window_messages_test.c)
    # DXIL that tools/make_rhi_test_shaders.py compiled from tests/data/rhi.
    tb_embed_test_data(TB_RHI_SHADER_SOURCE tb_rhi_shader_files rhi "*.dxil")
    # Frames the renderer drew on WARP, looked at by eye (see docs/testing.md).
    tb_embed_test_data(TB_GOLDEN_SOURCE tb_golden_files golden "*.bmp")
    list(APPEND TB_TEST_CASE_SOURCES rhi/rhi_d3d12_test.c ${TB_RHI_SHADER_SOURCE}
        render/canvas_d3d12_test.c render/golden.c render/lighting_d3d12_test.c ${TB_GOLDEN_SOURCE}
        render/material_d3d12_test.c
        render/renderer_d3d12_test.c render/resize_d3d12_test.c render/skinning_d3d12_test.c
        render/sky_d3d12_test.c
        render/text_d3d12_test.c render/viewport_d3d12_test.c shader/shader_library_d3d12_test.c
        gui/gui_d3d12_test.c
        gui/gui_look_d3d12_test.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TB_TEST_CASE_SOURCES platform/linux/window_x11_test.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TB_TEST_CASE_SOURCES platform/macos/window_events_test.c)
endif()

tb_add_tests(tb_tests SOURCES ${TB_TEST_CASE_SOURCES} LIBRARIES TenaciousBadger::badger)
# The engine's shaders: the manifest, and the libraries the build compiles
# from it beside the executables (tools/shaderc), which the cases on D3D12
# compare with what they compile at run time.
set_source_files_properties(shader/shader_library_d3d12_test.c PROPERTIES COMPILE_DEFINITIONS
    "TB_TEST_SHADER_MANIFEST=\"${PROJECT_SOURCE_DIR}/shaders/Engine/Shaders.manifest\";TB_TEST_SHADER_DIRECTORY=\"${PROJECT_BINARY_DIR}/bin\"")
if(TB_BUILD_SHADERS)
    add_dependencies(tb_tests badger_shaders)
endif()

# Repeat the allocation contracts with the checking functions installed.
tb_add_tests(tb_memory_checked_tests SUFFIX ".checked"
    SOURCES core/memory_test.c LIBRARIES TenaciousBadger::badger)
target_compile_definitions(tb_memory_checked_tests PRIVATE TB_TEST_CHECK_MEMORY)

# What a module keeps to itself is not in the engine library's interface, so
# tb_tests cannot reach it. The cases that test it are built from the engine's
# sources in place of linking the library: tb_internal_tests. A source of the
# engine is named here as it is named within the engine, such as
# "jobs/deque.h".
set(TB_INTERNAL_TEST_CASE_SOURCES
    core/decimal_fast_test.c
    core/decimal_rounding_test.c
    core/exclusive_list_test.c
    core/fs_transaction_test.c
    core/math_kernels_test.c
    core/reserved_arena_failure_test.c
    codec/crc32_table_test.c
    font/font_embedded_cache_test.c
    game/game_loop_refresh_test.c
    jobs/deque_test.c
    render/canvas_batch_test.c
    # A window's scale changed as its adapter changes it, with Roboto of the test data.
    render/canvas_scale_test.c
    ${TB_FONT_DATA_SOURCE}
    render/light_pack_test.c
    render/temporal_test.c
    rhi/rhi_choice_test.c
    rhi/rhi_device_test.c
    shader/shader_check_test.c
    shader/shader_dxc_test.c
    shader/shader_form_test.c
    shader/shader_parse_test.c
    platform/window_input_test.c
    platform/window_size_test.c)
# Input that a platform sends only to the window in front is handed to its
# backend's handling directly.
if(WIN32)
    list(APPEND TB_INTERNAL_TEST_CASE_SOURCES platform/win32/window_raw_test.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TB_INTERNAL_TEST_CASE_SOURCES platform/linux/window_input_test.c)
endif()
set(TB_ENGINE_SOURCES ${TB_CORE_SOURCES} ${TB_PLATFORM_SOURCES} ${TB_JOBS_SOURCES}
    ${TB_CODEC_SOURCES} ${TB_MEDIA_SOURCES} ${TB_SERIALIZE_SOURCES} ${TB_FONT_SOURCES} ${TB_FONT_DATA_OBJECTS}
    ${TB_VFS_SOURCES}
    ${TB_SETTINGS_SOURCES} ${TB_WINDOW_SOURCES} ${TB_DXC_SOURCES} ${TB_GAME_SOURCES}
    ${TB_SHADER_SOURCES} ${TB_RHI_SOURCES} ${TB_RENDER_SOURCES} ${TB_ANIM_SOURCES}
    ${TB_UI_SOURCES} ${TB_GUI_SOURCES} ${TB_INPUT_SOURCES})

# The arguments go to tb_add_tests.
function(tb_add_internal_tests target)
    tb_add_tests(${target} SOURCES ${TB_INTERNAL_TEST_CASE_SOURCES} ${ARGN})
    target_sources(${target} PRIVATE ${TB_ENGINE_SOURCES})
    target_link_libraries(${target} PRIVATE ${TB_MATH_LIBRARIES})
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
endfunction()

# Some behaviors cannot be cases of tb_tests because they end the process or
# must fail: the framework's own failure reports (framework/selftest/fixture),
# the engine's default handling of fatal conditions, and the test support's
# own checks. tb_probe holds those cases; a script per topic runs them in a
# child process and checks the outcome.
tb_add_tests(tb_probe NO_CTEST
    SOURCES core/assert_probe.c core/cli_probe.c core/crash_probe.c core/fatal_probe.c
        core/fmt_probe.c core/memory_probe.c
        core/memory_debug_probe.c core/fmt_failure_probe.c core/log_probe.c platform/virtual_memory_probe.c support/support_probe.c
    LIBRARIES TenaciousBadger::badger)
function(tb_add_probe_test name script)
    add_test(NAME ${name}
        COMMAND "${CMAKE_COMMAND}" "-DTB_PROBE=$<TARGET_FILE:tb_probe>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/${script}")
endfunction()
tb_add_probe_test(core_assert.default_handler_reports_and_aborts ExpectAssertAbort.cmake)
tb_add_probe_test(core_fatal.default_handler_reports_and_aborts ExpectFatalAbort.cmake)
tb_add_probe_test(core_crash.reports_crashes_with_their_stacks ExpectCrashReport.cmake)
tb_add_probe_test(core_fmt.prints_to_the_standard_streams ExpectFormattedOutput.cmake)
tb_add_probe_test(core_cli.finishing_writes_to_the_standard_streams ExpectCommandLine.cmake)
tb_add_probe_test(core_memory.default_handler_reports_and_aborts ExpectOutOfMemoryAbort.cmake)
tb_add_probe_test(test_support.leaks_and_failure_injection ExpectTestSupport.cmake)
tb_add_probe_test(core_memory_debug.reports_and_aborts ExpectMemoryDebug.cmake)

# Checks of the source text. They run with the tests so that a violation fails
# locally, before it fails in CI.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME core_log.stderr_output_and_failure
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/log_sink_test.py"
        "$<TARGET_FILE:tb_probe>")
add_test(NAME platform_memory.inaccessible_pages_fault
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/virtual_memory_fault_test.py"
        "$<TARGET_FILE:tb_probe>")
add_test(NAME core_fmt.failed_standard_streams_are_reported
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/stream_failure_test.py"
        "$<TARGET_FILE:tb_probe>")
add_test(NAME packaging.child_python_uses_the_running_interpreter
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/package_sdk_test.py")
add_test(NAME source_checks.foundation
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_foundation.py")
add_test(NAME source_checks.foundation_self_test
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_foundation.py" --self-test)
add_test(NAME source_checks.analyzer_driver_self_test
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/analyze_with_clang.py" --self-test)
add_test(NAME source_checks.contracts
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_contracts.py")
add_test(NAME source_checks.contracts_self_test
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_contracts.py" --self-test)
add_test(NAME source_checks.benchmark_comparison_self_test
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/compare_benchmarks.py" --self-test)
add_test(NAME source_checks.coverage_self_test
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_coverage.py" --self-test)
add_test(NAME source_checks.text
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_text.py")
add_test(NAME source_checks.text_self_test
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_text.py" --self-test)

if(TARGET burrow)
    add_test(NAME burrow.smoke COMMAND burrow check)
    add_test(NAME burrow.fails_when_its_output_cannot_be_written
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/burrow_output_test.py"
            "$<TARGET_FILE:burrow>")
    set_tests_properties(burrow.smoke PROPERTIES PASS_REGULAR_EXPRESSION "2 \\+ 3 = 5")
    add_test(NAME burrow.settings_load_in_layers_from_the_command_line
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/burrow_settings_test.py"
            "$<TARGET_FILE:burrow>")
    # Its command line: help, the version, and a wrong option, each with the
    # status it must exit with, which a pass expression would not check.
    function(tb_add_burrow_test name argument status expected)
        add_test(NAME burrow.${name}
            COMMAND "${CMAKE_COMMAND}" "-DTB_PROGRAM=$<TARGET_FILE:burrow>"
                "-DTB_ARGUMENT=${argument}" "-DTB_STATUS=${status}" "-DTB_EXPECTED=${expected}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectProgramOutput.cmake")
    endfunction()
    tb_add_burrow_test(help_is_written --help 0 "^usage: burrow \\[options\\]")
    tb_add_burrow_test(the_version_is_written --version 0 "^burrow ${PROJECT_VERSION}")
    tb_add_burrow_test(a_wrong_option_is_refused --nope 2
        "burrow: error: unknown option '--nope'")
    # The game's room, world grid, player, HUD and menus, built from its
    # sources; the menus from the layout files Burrow ships.
    tb_add_tests(tb_burrow_tests
        SOURCES burrow/burrow_grid_test.c burrow/burrow_hud_test.c burrow/burrow_menu_test.c
            burrow/burrow_menus_test.c burrow/burrow_player_test.c burrow/burrow_room_test.c
        LIBRARIES TenaciousBadger::badger)
    target_sources(tb_burrow_tests PRIVATE ${TB_BURROW_SOURCES})
    target_include_directories(tb_burrow_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(tb_burrow_tests PRIVATE
        BURROW_TEST_CONTENT="${TB_BURROW_CONTENT}")
    # The game runs for a few frames, drawing the room in a window where
    # there is a display, and skips where there is none.
    add_test(NAME burrow.plays_a_number_of_frames
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/burrow_play_test.py"
            "$<TARGET_FILE:burrow>")
    set_tests_properties(burrow.plays_a_number_of_frames PROPERTIES SKIP_RETURN_CODE 77)
    # Its menus, driven by navigation alone, in a window with the null
    # renderer; skipped where there is no display.
    add_test(NAME burrow.menus_are_driven_by_navigation
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/burrow_menus_test.py"
            "$<TARGET_FILE:burrow>")
    set_tests_properties(burrow.menus_are_driven_by_navigation PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(tb_static_tests consumers/static/main.c)
target_link_libraries(tb_static_tests PRIVATE TenaciousBadger::badger_static)
tb_configure_target(tb_static_tests)
add_test(NAME consumers.static COMMAND tb_static_tests)

# Compiles engine sources into a test executable in place of linking the
# engine library, so that the tests reach what the library does not export.
function(tb_make_standalone target)
    target_include_directories(${target} PRIVATE
        "${PROJECT_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/generated")
    target_compile_definitions(${target} PRIVATE TB_STATIC)
endfunction()

tb_add_internal_tests(tb_internal_tests)
tb_make_standalone(tb_internal_tests)

# The platform adapters are the layer beneath the foundation modules. Their
# tests are built from the selected adapter's sources and nothing else of the
# engine, which keeps the adapters free of the modules above them and lets a
# test call a function that is private to an adapter.
set(TB_PLATFORM_TEST_SOURCES
    platform/fs_test.c platform/virtual_memory_test.c
    platform/atomic_test.c platform/contract_test.c platform/crash_text_test.c
    platform/library_test.c platform/name_test.c platform/process_test.c
    platform/random_test.c platform/semaphore_test.c platform/stack_test.c platform/system_test.c
    platform/thread_local_test.c
    platform/thread_options_test.c platform/thread_test.c platform/time_test.c)
# An adapter reports a broken contract through tb_assert_fail(), which the
# core defines. Executables built from an adapter alone get it from here, as
# a trap of the test framework.
set(TB_PLATFORM_TEST_SUPPORT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/support/platform_assert.c")
set(TB_PLATFORM_PRIVATE_DIRECTORY "")
if(WIN32)
    list(APPEND TB_PLATFORM_TEST_SOURCES
        platform/win32/affinity_test.c platform/win32/counter_test.c
        platform/win32/crash_dump_test.c platform/win32/library_test.c
        platform/win32/process_test.c
        platform/win32/processors_test.c platform/win32/thread_priority_test.c
        platform/win32/thread_wait_test.c)
    set(TB_PLATFORM_PRIVATE_DIRECTORY "${PROJECT_SOURCE_DIR}/src/platform/win32")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TB_PLATFORM_TEST_SOURCES
        platform/linux/affinity_test.c platform/linux/library_test.c
        platform/linux/processors_test.c
        platform/linux/thread_priority_test.c platform/posix/crash_dump_test.c)
    set(TB_PLATFORM_PRIVATE_DIRECTORY "${PROJECT_SOURCE_DIR}/src/platform/linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TB_PLATFORM_TEST_SOURCES platform/macos/library_test.c
        platform/macos/thread_priority_test.c
        platform/posix/crash_dump_test.c)
endif()

# A shared library for the platform tests to load at run time and never link.
# It is built into the directory of the test executables, where a library
# that ships with a program is found.
add_library(tb_library_fixture MODULE platform/library_fixture.c)
set_target_properties(tb_library_fixture PROPERTIES PREFIX "" WINDOWS_EXPORT_ALL_SYMBOLS ON)
tb_configure_target(tb_library_fixture)

# The arguments go to tb_add_test_executable.
function(tb_add_platform_tests target)
    tb_add_test_executable(${target} SOURCES ${TB_PLATFORM_TEST_SOURCES} ${ARGN})
    target_sources(${target} PRIVATE ${TB_PLATFORM_SOURCES} ${TB_PLATFORM_TEST_SUPPORT_SOURCES})
    # What the adapters share is tested on every platform.
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/platform/shared")
    if(TB_PLATFORM_PRIVATE_DIRECTORY)
        target_include_directories(${target} PRIVATE "${TB_PLATFORM_PRIVATE_DIRECTORY}")
    endif()
    tb_configure_target(${target})
    tb_make_standalone(${target})
    add_dependencies(${target} tb_library_fixture)
    target_compile_definitions(${target} PRIVATE
        "TB_LIBRARY_FIXTURE_FILE=\"$<TARGET_FILE_NAME:tb_library_fixture>\"")
endfunction()

tb_add_platform_tests(tb_platform_tests)

# A platform without an adapter of its own is built with the default one,
# whose every function reports "not implemented" and ends the process. It is
# standard C, so it is built and probed here on every platform: a function
# cannot join a platform interface without its default.
tb_add_test_executable(tb_platform_default_probe NO_CTEST SOURCES platform/default_probe.c)
target_sources(tb_platform_default_probe PRIVATE ${TB_PLATFORM_DEFAULT_SOURCES})
target_include_directories(tb_platform_default_probe PRIVATE
    "${TB_PLATFORM_DEFAULT_INCLUDE_DIRECTORY}")
tb_configure_target(tb_platform_default_probe)
tb_make_standalone(tb_platform_default_probe)
set(TB_PLATFORM_INTERFACE_HEADERS
    "${PROJECT_SOURCE_DIR}/include/badger/crash.h"
    "${PROJECT_SOURCE_DIR}/include/badger/dxc.h"
    "${PROJECT_SOURCE_DIR}/include/badger/virtual_memory.h"
    "${PROJECT_SOURCE_DIR}/include/badger/file.h"
    "${PROJECT_SOURCE_DIR}/include/badger/library.h"
    "${PROJECT_SOURCE_DIR}/include/badger/platform.h"
    "${PROJECT_SOURCE_DIR}/include/badger/process.h"
    "${PROJECT_SOURCE_DIR}/include/badger/random.h"
    "${PROJECT_SOURCE_DIR}/include/badger/stack.h"
    "${PROJECT_SOURCE_DIR}/include/badger/system.h"
    "${PROJECT_SOURCE_DIR}/include/badger/thread.h"
    "${PROJECT_SOURCE_DIR}/include/badger/window.h")
list(JOIN TB_PLATFORM_INTERFACE_HEADERS "," tb_platform_interface_headers)
add_test(NAME platform_default.every_function_reports_not_implemented
    COMMAND "${CMAKE_COMMAND}" "-DTB_PROBE=$<TARGET_FILE:tb_platform_default_probe>"
        "-DTB_HEADERS=${tb_platform_interface_headers}"
        "-DTB_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectPlatformDefault.cmake")
# The package key: compiled in from a key file and from a password, it comes
# back whole and is nowhere in the executable; and keys are made and kept.
add_executable(tb_package_key_probe tools/package_key_probe.c)
target_link_libraries(tb_package_key_probe PRIVATE TenaciousBadger::badger)
tb_configure_target(tb_package_key_probe)
tb_add_package_key(tb_package_key_probe FUNCTION tb_probe_file_key
    KEY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/data/keys/test.key")
tb_add_package_key(tb_package_key_probe FUNCTION tb_probe_password_key
    PASSWORD_FILE "${CMAKE_CURRENT_SOURCE_DIR}/data/keys/test.password")
add_test(NAME package_key.comes_back_whole_and_is_nowhere_in_the_executable
    COMMAND "${CMAKE_COMMAND}" "-DTB_PROBE=$<TARGET_FILE:tb_package_key_probe>"
        "-DTB_KEY_FILE=${CMAKE_CURRENT_SOURCE_DIR}/data/keys/test.key"
        "-DTB_PASSWORD_FILE=${CMAKE_CURRENT_SOURCE_DIR}/data/keys/test.password"
        "-DTB_MODULE=${PROJECT_SOURCE_DIR}/cmake/BadgerPackageKey.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectPackageKey.cmake")
add_test(NAME package_key.keys_are_made_kept_and_refused
    COMMAND "${CMAKE_COMMAND}" "-DTB_MODULE=${PROJECT_SOURCE_DIR}/cmake/BadgerPackageKey.cmake"
        "-DTB_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/package_key_generation"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectPackageKeyGeneration.cmake")

add_test(NAME platform_selection.an_unsupported_platform_stops_the_build
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectPlatformSelection.cmake")

if(TB_SANITIZER)
    # Compile engine sources into standalone test executables. The shipping
    # shared library and burrow never acquire sanitizer dependencies. The
    # build has one kind of sanitizer, address or thread (cmake/Sanitizers.cmake),
    # and the same executables are built for either.
    # Each of these is built from the engine's sources.
    function(tb_make_standalone_sanitized target)
        tb_make_standalone(${target})
        tb_enable_sanitizers(${target})
        target_link_libraries(${target} PRIVATE ${TB_MATH_LIBRARIES})
    endfunction()

    tb_add_tests(tb_tests_sanitized
        SUFFIX ".sanitized" FRAMEWORK tb_test_sanitized
        SOURCES ${TB_TEST_CASE_SOURCES})
    target_sources(tb_tests_sanitized PRIVATE ${TB_ENGINE_SOURCES})
    tb_make_standalone_sanitized(tb_tests_sanitized)

    tb_add_internal_tests(tb_internal_tests_sanitized
        SUFFIX ".sanitized" FRAMEWORK tb_test_sanitized)
    tb_make_standalone_sanitized(tb_internal_tests_sanitized)

    if(TB_SANITIZER STREQUAL "address")
        # The same buffer overruns are detected independently by ASan.
        tb_add_tests(tb_memory_raw_probe_sanitized NO_CTEST FRAMEWORK tb_test_sanitized
            SOURCES core/memory_debug_probe.c)
        target_sources(tb_memory_raw_probe_sanitized PRIVATE ${TB_ENGINE_SOURCES})
        target_compile_definitions(tb_memory_raw_probe_sanitized PRIVATE TB_MEMORY_PROBE_RAW)
        tb_make_standalone_sanitized(tb_memory_raw_probe_sanitized)
        add_test(NAME core_memory_debug.sanitizer_agrees_on_overruns.sanitized
            COMMAND "${CMAKE_COMMAND}" "-DTB_PROBE=$<TARGET_FILE:tb_memory_raw_probe_sanitized>"
                -DTB_RAW=ON -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectMemoryDebug.cmake")

        # Touching memory that an arena has released must end with a report.
        tb_add_tests(tb_probe_sanitized NO_CTEST FRAMEWORK tb_test_sanitized
            SOURCES core/arena_probe.c)
        target_sources(tb_probe_sanitized PRIVATE ${TB_ENGINE_SOURCES})
        tb_make_standalone_sanitized(tb_probe_sanitized)
        add_test(NAME core_arena.released_memory_is_reported.sanitized
            COMMAND "${CMAKE_COMMAND}" "-DTB_PROBE=$<TARGET_FILE:tb_probe_sanitized>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectArenaPoison.cmake")
    endif()

    tb_add_platform_tests(tb_platform_tests_sanitized
        SUFFIX ".sanitized" FRAMEWORK tb_test_sanitized)
    tb_enable_sanitizers(tb_platform_tests_sanitized)

    if(TB_SANITIZER STREQUAL "thread")
        # A run that reports no data race proves nothing unless the sanitizer
        # can see one. This probe races on purpose, and repeats the race behind
        # each thing the engine offers to prevent it; a script checks that the
        # first is reported and the others are not.
        tb_add_test_executable(tb_race_probe_sanitized NO_CTEST FRAMEWORK tb_test_sanitized
            SOURCES platform/race_probe.c)
        target_sources(tb_race_probe_sanitized PRIVATE
            ${TB_PLATFORM_SOURCES} ${TB_PLATFORM_TEST_SUPPORT_SOURCES})
        tb_configure_target(tb_race_probe_sanitized)
        tb_make_standalone(tb_race_probe_sanitized)
        tb_enable_sanitizers(tb_race_probe_sanitized)
        add_test(NAME platform_race.a_data_race_is_reported_and_synchronized_access_is_not.sanitized
            COMMAND "${CMAKE_COMMAND}" "-DTB_PROBE=$<TARGET_FILE:tb_race_probe_sanitized>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectDataRace.cmake")
    endif()
endif()
