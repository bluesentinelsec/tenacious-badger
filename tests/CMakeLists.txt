add_subdirectory(framework)

set(TB_ENGINE_TEST_SUPPORT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/support")

# Adds a test executable for engine code. Its entry point routes failed engine
# assertions to the test framework; see support/engine_test.h. Accepts the
# arguments of tb_add_test_executable except MAIN.
function(tb_add_engine_tests target)
    tb_add_test_executable(${target}
        MAIN "${TB_ENGINE_TEST_SUPPORT_DIRECTORY}/engine_main.c" ${ARGN})
    target_include_directories(${target} PRIVATE "${TB_ENGINE_TEST_SUPPORT_DIRECTORY}")
    tb_configure_target(${target})
endfunction()

set(core_test_sources core/assert_test.c core/sum_test.c)
tb_add_engine_tests(tb_core_tests
    PREFIX "core."
    SOURCES ${core_test_sources}
    LIBRARIES TenaciousBadger::badger)

# The default assertion handler ends the process, so a child process probes it.
tb_add_test_executable(tb_assert_probe NO_CTEST
    SOURCES core/assert_probe.c
    LIBRARIES TenaciousBadger::badger)
tb_configure_target(tb_assert_probe)
add_test(NAME core.assert.default_handler_reports_and_aborts
    COMMAND "${CMAKE_COMMAND}" "-DTB_PROBE=$<TARGET_FILE:tb_assert_probe>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectAssertAbort.cmake")

if(TARGET burrow)
    add_test(NAME burrow.smoke COMMAND burrow)
    set_tests_properties(burrow.smoke PROPERTIES PASS_REGULAR_EXPRESSION "2 \\+ 3 = 5")
endif()

add_executable(tb_static_tests consumers/static/main.c)
target_link_libraries(tb_static_tests PRIVATE TenaciousBadger::badger_static)
tb_configure_target(tb_static_tests)
add_test(NAME consumers.static COMMAND tb_static_tests)

function(tb_add_platform_test target suffix)
    if(WIN32)
        add_test(NAME platform.time${suffix} COMMAND ${target})
    else()
        add_test(NAME platform.fatal_stub${suffix}
            COMMAND "${CMAKE_COMMAND}"
                "-DTB_PROBE=$<TARGET_FILE:${target}>"
                "-DTB_PLATFORM=${TB_STUB_PLATFORM}"
                "-DTB_NATIVE_API=${TB_STUB_API}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectPlatformStub.cmake")
    endif()
endfunction()

add_executable(tb_platform_probe platform/time_probe.c)
target_link_libraries(tb_platform_probe PRIVATE TenaciousBadger::badger)
tb_configure_target(tb_platform_probe)
tb_add_platform_test(tb_platform_probe "")

if(TB_ENABLE_SANITIZERS)
    # Compile engine sources into standalone test executables. The shipping
    # shared library and burrow never acquire sanitizer dependencies.
    function(tb_make_standalone_sanitized target)
        target_include_directories(${target} PRIVATE
            "${PROJECT_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/generated")
        target_compile_definitions(${target} PRIVATE TB_STATIC)
        tb_enable_sanitizers(${target})
    endfunction()

    tb_add_engine_tests(tb_core_sanitized
        PREFIX "core." SUFFIX ".sanitized"
        FRAMEWORK tb_test_sanitized
        SOURCES ${core_test_sources})
    target_sources(tb_core_sanitized PRIVATE ${TB_CORE_SOURCES})
    tb_make_standalone_sanitized(tb_core_sanitized)

    add_executable(tb_platform_sanitized platform/time_probe.c "${TB_PLATFORM_TIME_SOURCE}")
    tb_configure_target(tb_platform_sanitized)
    tb_make_standalone_sanitized(tb_platform_sanitized)
    tb_add_platform_test(tb_platform_sanitized ".sanitized")
endif()
