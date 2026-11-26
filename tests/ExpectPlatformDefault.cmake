# Checks the default platform adapter: every function of every platform
# interface reports "not implemented" on stderr and ends the process with a
# failure status. The functions are read from the public headers, so one that
# is added to an interface without a default, or without a probe case, fails
# here.
#
#   TB_PROBE        The probe executable, built from the default adapter.
#   TB_HEADERS      The public headers of the platform interfaces, separated
#                   by commas.
#   TB_SYSTEM_NAME  The system the build was configured for, which the report
#                   names.
#   TB_ENGINE_SOURCES  Sources of the engine, separated by commas, that define
#                   functions of the interfaces' headers once for every
#                   platform: names, and arithmetic on values. Those need
#                   nothing from the platform, so no adapter defines them.

# The one function the default adapter implements: it ends the process, which
# the report of every other function relies on.
set(implemented_function tb_platform_abort)

# Exit status of the framework when no case has the name it was given.
set(no_such_case_status 2)

# The functions defined at the start of a line of an engine source.
string(REPLACE "," ";" engine_sources "${TB_ENGINE_SOURCES}")
set(engine_functions "")
foreach(source IN LISTS engine_sources)
    file(STRINGS "${source}" definitions REGEX "^[a-z].*[ *]tb_[a-z0-9_]+\\(")
    foreach(definition IN LISTS definitions)
        if(definition MATCHES "[ *](tb_[a-z0-9_]+)\\(")
            list(APPEND engine_functions "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()

string(REPLACE "," ";" headers "${TB_HEADERS}")
set(functions "")
foreach(header IN LISTS headers)
    file(STRINGS "${header}" declarations REGEX "^TB_API ")
    if(NOT declarations)
        message(FATAL_ERROR "${header} declares no TB_API function; is it a platform interface?")
    endif()
    foreach(declaration IN LISTS declarations)
        if(NOT declaration MATCHES "[ *](tb_[a-z0-9_]+)\\(")
            message(FATAL_ERROR
                "${header}: no function name on the line of its TB_API: ${declaration}")
        endif()
        # A function that only names a result needs nothing from the
        # platform: the engine implements it once, and no adapter does. The
        # name is kept first: the next match replaces CMAKE_MATCH_1.
        set(name "${CMAKE_MATCH_1}")
        list(FIND engine_functions "${name}" engine_index)
        if(NOT name MATCHES "_result_name$" AND engine_index EQUAL -1)
            list(APPEND functions "${name}")
        endif()
    endforeach()
endforeach()

foreach(function IN LISTS functions)
    execute_process(COMMAND "${TB_PROBE}" "platform_default.${function}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic
        TIMEOUT 60)
    set(report "${function}: status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")
    if(result STREQUAL "${no_such_case_status}")
        message(FATAL_ERROR
            "${function} has no case in tests/platform/default_probe.c. Add one, and the "
            "function to the default adapter in src/platform/default/: ${report}")
    endif()
    if(NOT result STREQUAL "1")
        message(FATAL_ERROR
            "The default adapter must end the process with a failure status: ${report}")
    endif()
    set(expected "^platform\\.[a-z]+: ${function} on ${TB_SYSTEM_NAME}: not implemented; add a platform adapter for ${TB_SYSTEM_NAME} under src/platform/\n$")
    if(function STREQUAL implemented_function)
        if(NOT diagnostic STREQUAL "")
            message(FATAL_ERROR "${function} must end the process and report nothing: ${report}")
        endif()
    elseif(NOT diagnostic MATCHES "${expected}")
        message(FATAL_ERROR "Unexpected report, wanted ${expected}: ${report}")
    endif()
endforeach()

list(LENGTH functions function_count)
if(function_count EQUAL 0)
    message(FATAL_ERROR "No function of a platform interface was found, so nothing was checked.")
endif()
message(STATUS "Checked ${function_count} functions of the default platform adapter")
