add_executable(tb_core_tests core/sum_test.c)
target_link_libraries(tb_core_tests PRIVATE TenaciousBadger::badger)
tb_configure_target(tb_core_tests)
add_test(NAME core.sum COMMAND tb_core_tests)
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
    add_executable(tb_core_sanitized core/sum_test.c "${PROJECT_SOURCE_DIR}/src/core/sum.c")
    add_executable(tb_platform_sanitized platform/time_probe.c "${TB_PLATFORM_TIME_SOURCE}")
    foreach(target tb_core_sanitized tb_platform_sanitized)
        target_include_directories(${target} PRIVATE
            "${PROJECT_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/generated")
        target_compile_definitions(${target} PRIVATE TB_STATIC)
        tb_configure_target(${target})
        tb_enable_sanitizers(${target})
    endforeach()
    add_test(NAME core.sum.sanitized COMMAND tb_core_sanitized)
    tb_add_platform_test(tb_platform_sanitized ".sanitized")
endif()
