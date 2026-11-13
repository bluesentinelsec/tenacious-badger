include_guard(GLOBAL)

#[[
tb_add_test_executable(<target>
    SOURCES <file>...       Sources that define cases with TB_TEST(suite, name).
    [LIBRARIES <target>...] Libraries under test, linked privately.
    [PREFIX <text>]         Prepended to each CTest name, such as "core.".
    [SUFFIX <text>]         Appended to each CTest name, such as ".sanitized".
    [MAIN <file>]           Custom entry point that calls tb_test_main().
    [FRAMEWORK <target>]    Framework library to link; defaults to tb_test.
    [NO_CTEST])             Build the executable without registering its cases.

Builds a test executable and registers every case as its own CTest test named
<PREFIX><suite>.<name><SUFFIX>. Each CTest test carries the source location of
its case, which is what lets IDE test explorers list, run, debug, and jump to
individual cases.

The cases are found by scanning SOURCES when the target builds. Nothing runs
the executable to discover them, so registration also works for binaries the
build machine cannot execute.

Add sources that define no cases, such as the code under test, with
target_sources() after this call. Apply project compile settings to the target
afterwards as well.
]]
function(tb_add_test_executable target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "NO_CTEST" "PREFIX;SUFFIX;MAIN;FRAMEWORK"
        "SOURCES;LIBRARIES")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "tb_add_test_executable(${target}): unknown arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "tb_add_test_executable(${target}): SOURCES is required")
    endif()
    if(NOT arg_MAIN)
        set(arg_MAIN "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/main.c")
    endif()
    if(NOT arg_FRAMEWORK)
        set(arg_FRAMEWORK tb_test)
    endif()

    set(scanned_sources "")
    foreach(source IN LISTS arg_SOURCES)
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE
            OUTPUT_VARIABLE absolute_source)
        list(APPEND scanned_sources "${absolute_source}")
    endforeach()

    set(scan_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/TbTestScan.cmake")
    set(output_base "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    set(manifest "${output_base}_scan.cmake")
    set(registry "${output_base}_registry.c")
    set(ctest_cases "${output_base}_cases.cmake")

    # The scan reads its inputs from a manifest instead of the command line, so
    # every generator reruns it when the sources or names of the target change.
    string(CONCAT manifest_content
        "set(TB_TEST_TARGET [==[${target}]==])\n"
        "set(TB_TEST_SOURCES [==[${scanned_sources}]==])\n"
        "set(TB_TEST_NAME_PREFIX [==[${arg_PREFIX}]==])\n"
        "set(TB_TEST_NAME_SUFFIX [==[${arg_SUFFIX}]==])\n"
        "set(TB_TEST_REGISTRY [==[${registry}]==])\n"
        "set(TB_TEST_CTEST_CASES [==[${ctest_cases}]==])\n")
    file(CONFIGURE OUTPUT "${manifest}" CONTENT "${manifest_content}" @ONLY)

    add_custom_command(
        OUTPUT "${registry}" "${ctest_cases}"
        COMMAND "${CMAKE_COMMAND}" "-DTB_TEST_MANIFEST=${manifest}" -P "${scan_script}"
        DEPENDS ${scanned_sources} "${manifest}" "${scan_script}"
        COMMENT "Collecting TB_TEST cases of ${target}"
        VERBATIM)

    add_executable(${target} ${scanned_sources} "${registry}" "${arg_MAIN}")
    target_link_libraries(${target} PRIVATE ${arg_FRAMEWORK} ${arg_LIBRARIES})

    if(arg_NO_CTEST)
        return()
    endif()

    # CTest reads this file whenever it loads the directory's tests. Until the
    # target is built there are no cases to run; a placeholder that cannot
    # start makes that a visible failure rather than an empty, green run.
    set(not_built "${arg_PREFIX}${target}.NOT_BUILT${arg_SUFFIX}")
    string(CONCAT include_content
        "set(TB_TEST_EXECUTABLE [==[$<TARGET_FILE:${target}>]==])\n"
        "if(EXISTS \"\${TB_TEST_EXECUTABLE}\" AND EXISTS [==[${ctest_cases}]==])\n"
        "    include([==[${ctest_cases}]==])\n"
        "else()\n"
        "    add_test([==[${not_built}]==] [==[${not_built}]==])\n"
        "endif()\n")
    set(ctest_include "${output_base}_ctest.cmake")
    get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(is_multi_config)
        # One file per configuration, chosen by the configuration CTest runs.
        file(GENERATE OUTPUT "${output_base}_ctest-$<CONFIG>.cmake" CONTENT "${include_content}")
        string(CONCAT dispatch_content
            "set(tb_test_configured"
            " \"${output_base}_ctest-\${CTEST_CONFIGURATION_TYPE}.cmake\")\n"
            "if(EXISTS \"\${tb_test_configured}\")\n"
            "    include(\"\${tb_test_configured}\")\n"
            "else()\n"
            "    add_test([==[${not_built}]==] [==[${not_built}]==])\n"
            "endif()\n")
        file(CONFIGURE OUTPUT "${ctest_include}" CONTENT "${dispatch_content}" @ONLY)
    else()
        file(GENERATE OUTPUT "${ctest_include}" CONTENT "${include_content}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${ctest_include}")
endfunction()
