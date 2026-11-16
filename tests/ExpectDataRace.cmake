# Runs the race probe under ThreadSanitizer and checks that the sanitizer
# reports the one deliberate data race, and reports nothing where the engine's
# mutex, atomic values, or thread functions order the same accesses.
#
#   TB_PROBE  The probe executable, built with ThreadSanitizer.

# Runs one probe case. Sets result, output, diagnostic, and report.
macro(run_probe_case case_name)
    execute_process(COMMAND "${TB_PROBE}" "${case_name}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic
        TIMEOUT 120)
    set(report
        "${case_name}: status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")
endmacro()

run_probe_case("race_probe.unsynchronized_writes")
if(result STREQUAL "0" OR result MATCHES "timeout")
    message(FATAL_ERROR "A data race must fail the run: ${report}")
endif()
if(NOT diagnostic MATCHES "ThreadSanitizer: data race")
    message(FATAL_ERROR "The sanitizer did not report the data race: ${report}")
endif()

foreach(case_name
        writes_under_a_mutex
        atomic_updates
        writes_before_a_thread_starts_and_after_it_ends
        an_object_published_through_an_atomic_pointer)
    run_probe_case("race_probe.${case_name}")
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Synchronized access must pass: ${report}")
    endif()
    if(diagnostic MATCHES "ThreadSanitizer")
        message(FATAL_ERROR "The sanitizer reported synchronized access: ${report}")
    endif()
endforeach()
