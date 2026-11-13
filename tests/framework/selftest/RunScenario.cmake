#[[
Checks the test framework from the outside: each scenario runs the fixture
executable (cases that fail on purpose) or the source scanner, then verifies
the exit status and the report.

  cmake -DTB_SCENARIO=<name> -DTB_FIXTURE=<fixture executable>
        -DTB_FIXTURE_CASES=<fixture CTest case file> -DTB_SCAN_SCRIPT=<TbTestScan.cmake>
        -DTB_WORK_DIRECTORY=<scratch directory> -P RunScenario.cmake

Every `function(scenario_<name>)` below is registered as the CTest test
test_framework.scenario.<name> by the selftest CMakeLists.txt.
]]
cmake_minimum_required(VERSION 3.25)

set(default_seed "0x7462746573740001")

# --- helpers -----------------------------------------------------------------

function(fail_scenario message)
    message(FATAL_ERROR
        "${TB_SCENARIO}: ${message}\n"
        "--- status: ${status}\n--- stdout:\n${output}\n--- stderr:\n${diagnostics}")
endfunction()

# Runs the fixture with the given arguments. Sets status, output, diagnostics.
macro(run_fixture)
    execute_process(COMMAND "${TB_FIXTURE}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics
        TIMEOUT 60)
    # Windows text streams end lines with CR LF; compare against LF everywhere.
    string(REPLACE "\r\n" "\n" output "${output}")
    string(REPLACE "\r\n" "\n" diagnostics "${diagnostics}")
endmacro()

function(expect_status expected)
    if(NOT status STREQUAL expected)
        fail_scenario("expected exit status ${expected}")
    endif()
endfunction()

function(expect_text text)
    string(FIND "${output}" "${text}" position)
    if(position EQUAL -1)
        fail_scenario("stdout does not contain: ${text}")
    endif()
endfunction()

function(expect_no_text text)
    string(FIND "${output}" "${text}" position)
    if(NOT position EQUAL -1)
        fail_scenario("stdout must not contain: ${text}")
    endif()
endfunction()

function(expect_diagnostic text)
    string(FIND "${diagnostics}" "${text}" position)
    if(position EQUAL -1)
        fail_scenario("stderr does not contain: ${text}")
    endif()
endfunction()

function(expect_match expression)
    if(NOT output MATCHES "${expression}")
        fail_scenario("stdout does not match: ${expression}")
    endif()
endfunction()

# Counts the non-overlapping occurrences of a literal text in stdout.
function(count_text text result_variable)
    set(remaining "${output}")
    set(count 0)
    string(LENGTH "${text}" text_length)
    while(TRUE)
        string(FIND "${remaining}" "${text}" position)
        if(position EQUAL -1)
            break()
        endif()
        math(EXPR count "${count} + 1")
        math(EXPR next "${position} + ${text_length}")
        string(SUBSTRING "${remaining}" ${next} -1 remaining)
    endwhile()
    set(${result_variable} ${count} PARENT_SCOPE)
endfunction()

function(expect_count text expected)
    count_text("${text}" actual)
    if(NOT actual EQUAL expected)
        fail_scenario("expected ${expected} occurrences of \"${text}\", found ${actual}")
    endif()
endfunction()

# Prepares an empty scratch directory for the scenario. Sets work_directory.
macro(prepare_work_directory)
    set(work_directory "${TB_WORK_DIRECTORY}/${TB_SCENARIO}")
    file(REMOVE_RECURSE "${work_directory}")
    file(MAKE_DIRECTORY "${work_directory}")
endmacro()

# Runs the scanner over the given sources. Sets status, output, diagnostics,
# registry (path), and cases (path).
macro(run_scan)
    set(registry "${work_directory}/registry.c")
    set(cases "${work_directory}/cases.cmake")
    file(WRITE "${work_directory}/manifest.cmake"
        "set(TB_TEST_TARGET [==[scanned_target]==])\n"
        "set(TB_TEST_SOURCES [==[${ARGN}]==])\n"
        "set(TB_TEST_NAME_PREFIX [==[pre.]==])\n"
        "set(TB_TEST_NAME_SUFFIX [==[.post]==])\n"
        "set(TB_TEST_REGISTRY [==[${registry}]==])\n"
        "set(TB_TEST_CTEST_CASES [==[${cases}]==])\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DTB_TEST_MANIFEST=${work_directory}/manifest.cmake"
            -P "${TB_SCAN_SCRIPT}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics)
endmacro()

function(expect_file_text path text)
    file(READ "${path}" content)
    string(FIND "${content}" "${text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${TB_SCENARIO}: ${path} does not contain: ${text}\n---\n${content}")
    endif()
endfunction()

function(expect_no_file_text path text)
    file(READ "${path}" content)
    string(FIND "${content}" "${text}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "${TB_SCENARIO}: ${path} must not contain: ${text}\n---\n${content}")
    endif()
endfunction()

# --- running, passing, and exit statuses ---------------------------------------

function(scenario_passing_cases_exit_zero)
    run_fixture("pass.*")
    expect_status(0)
    expect_text("[ SUMMARY ] 2 cases: 2 passed, 0 failed, 0 skipped; 3 checks;")
    # Quiet by default: a passing case prints nothing of its own.
    expect_no_text("[ RUN     ]")
    expect_no_text("[ PASSED  ]")
endfunction()

function(scenario_verbose_reports_every_case)
    run_fixture(--verbose "pass.*")
    expect_status(0)
    expect_text("[ RUN     ] pass.no_checks")
    expect_text("[ PASSED  ] pass.no_checks (0 checks,")
    expect_text("[ PASSED  ] pass.several_checks (3 checks,")
endfunction()

function(scenario_expect_reports_and_continues)
    run_fixture("fail.expect_continues")
    expect_status(1)
    # Both checks ran: EXPECT records a failure without ending the case.
    expect_text("after-first-expect")
    expect_text("after-second-expect")
    expect_text("TB_TEST_EXPECT_INT(2 + 2, ==, 5) failed with 4 == 5")
    expect_text("[ FAILED  ] fail.expect_continues: 2 of 2 checks failed")
    expect_text("[ SUMMARY ] 1 case: 0 passed, 1 failed, 0 skipped; 2 checks;")
endfunction()

function(scenario_failure_location_is_a_compiler_style_diagnostic)
    run_fixture("fail.expect_continues")
    if(NOT output MATCHES "check-line=([0-9]+)")
        fail_scenario("the fixture did not print the line of its check")
    endif()
    set(line "${CMAKE_MATCH_1}")
    # `file(line): error:` on Windows and `file:line: error:` elsewhere. IDEs
    # link these forms and CMake Tools reads them as test failure locations.
    expect_match(
        "fixture_tests\\.c[(:]${line}[):]+ error: fail\\.expect_continues: TB_TEST_EXPECT\\(1 \\+ 1 == 3\\) failed")
endfunction()

function(scenario_require_ends_the_case)
    run_fixture("fail.require_stops" "fail.require_in_helper")
    expect_status(1)
    expect_text("TB_TEST_REQUIRE_INT(7, <, 3) failed with 7 < 3")
    expect_no_text("after-require")
    # A REQUIRE in a helper ends the whole case, not only the helper.
    expect_text("fail.require_in_helper: TB_TEST_REQUIRE(false) failed")
    expect_no_text("after-helper")
    expect_text("[ SUMMARY ] 2 cases: 0 passed, 2 failed, 0 skipped; 2 checks;")
endfunction()

function(scenario_failed_case_names_its_rerun_command)
    run_fixture("fail.require_stops")
    expect_text("rerun with: fail.require_stops --seed ${default_seed}")
endfunction()

function(scenario_typed_checks_report_both_values)
    run_fixture("fail.uint_values" "fail.hex_values" "fail.float_values" "fail.float_near"
        "fail.pointer_values")
    expect_status(1)
    expect_text("TB_TEST_EXPECT_UINT(used, <=, capacity) failed with 3 <= 2")
    expect_text("TB_TEST_EXPECT_HEX(0xFFu, ==, 0xF0u) failed with 0xFF == 0xF0")
    expect_text("TB_TEST_EXPECT_FLOAT(0.5, >, 1.5) failed with 0.5 > 1.5")
    expect_text("TB_TEST_EXPECT_FLOAT_NEAR(1.0, 2.0, 0.25) failed: 1 is not within 0.25 of 2")
    expect_text("TB_TEST_EXPECT_POINTER(&object, ==, NULL) failed with ")
endfunction()

function(scenario_string_checks_escape_and_locate_differences)
    run_fixture("fail.string_values" "fail.string_null")
    expect_status(1)
    expect_text("failed with \"badger\\n\" == \"badge\\tr\"")
    expect_text("    first difference at index 5")
    expect_text("TB_TEST_EXPECT_STRING(missing, ==, \"text\") failed with (null) == \"text\"")
endfunction()

function(scenario_byte_checks_show_the_first_difference)
    run_fixture("fail.bytes_values" "fail.bytes_null")
    expect_status(1)
    expect_text("failed: first difference at offset 20 of 32")
    expect_text("    actual:   .. 0C 0D 0E 0F 10 11 12 13 [14] 15 16 17 18 19 1A 1B 1C ..")
    expect_text("    expected: .. 0C 0D 0E 0F 10 11 12 13 [EE] 15 16 17 18 19 1A 1B 1C ..")
    expect_text("failed: a range of 4 bytes is NULL")
endfunction()

function(scenario_context_describes_the_failing_input)
    run_fixture("fail.context_is_reported")
    expect_status(1)
    expect_text("    context: row 3 of table")

    run_fixture("fail.context_is_cleared")
    expect_status(1)
    expect_no_text("context:")
endfunction()

function(scenario_explicit_failure_formats_its_message)
    run_fixture("fail.explicit_failure")
    expect_status(1)
    expect_text("error: fail.explicit_failure: TB_TEST_FAIL: unexpected tag 42")
endfunction()

function(scenario_traps_fail_unless_expected)
    run_fixture("trap.expected_trap_passes")
    expect_status(0)
    expect_text("after-expected-trap")

    run_fixture("fail.unexpected_trap")
    expect_status(1)
    # The report points at the trapping code, not at the test.
    expect_match("engine_source\\.c[(:]1234[):]+ error: fail\\.unexpected_trap: unexpected trap: fixture trap message")
    expect_text("[ FAILED  ] fail.unexpected_trap: 1 of 1 checks failed")

    run_fixture("fail.missing_trap" "fail.wrong_trap_message" "fail.require_trap_stops")
    expect_status(1)
    expect_text("TB_TEST_EXPECT_TRAP(return_without_trapping) failed: the body returned without a trap")
    expect_text("after-missing-trap")
    expect_text("failed: trap message \"alpha condition\" does not contain \"beta\"")
    expect_no_text("after-require-trap")
endfunction()

function(scenario_skips_are_visible_and_never_hide_failures)
    run_fixture("skip.with_reason")
    expect_status(77)
    expect_text("[ SKIPPED ] skip.with_reason: needs a GPU")
    expect_text("[ SUMMARY ] 1 case: 0 passed, 0 failed, 1 skipped; 0 checks;")

    # A skip next to a pass is a passing run.
    run_fixture("skip.with_reason" "pass.no_checks")
    expect_status(0)

    run_fixture("skip.after_failure")
    expect_status(1)
    expect_text("[ FAILED  ] skip.after_failure")
endfunction()

function(scenario_fail_fast_stops_at_the_first_failed_case)
    run_fixture("fail.*")
    expect_status(1)
    count_text("[ FAILED  ]" failed_without_fail_fast)
    if(failed_without_fail_fast LESS 2)
        fail_scenario("the fixture needs several failing cases for this scenario")
    endif()

    run_fixture(--fail-fast "fail.*")
    expect_status(1)
    expect_count("[ FAILED  ]" 1)
    expect_text("[ STOPPED ] --fail-fast: ")
    expect_text("[ SUMMARY ] 1 case: 0 passed, 1 failed, 0 skipped; 2 checks;")
endfunction()

function(scenario_hooks_run_around_every_case)
    # Without hooks installed, nothing extra runs and both hook cases pass.
    run_fixture("hooks.*" pass.several_checks)
    expect_status(0)
    expect_no_text("hook-")
    expect_text("failed-case-body-ran")
    expect_text("skipped-case-body-ran")

    run_fixture(--with-hooks pass.several_checks fail.require_stops skip.with_reason "hooks.*")
    expect_status(1)
    # Both hooks run for every case, with the context they were given.
    expect_count("hook-before " 5)
    expect_count("hook-after " 5)
    expect_text("hook-before pass.several_checks (fixture context)")
    expect_text("hook-after pass.several_checks failed=0")
    string(FIND "${output}" "hook-before pass.several_checks" before_position)
    string(FIND "${output}" "hook-after pass.several_checks" after_position)
    if(NOT before_position LESS after_position)
        fail_scenario("the before hook must run ahead of the after hook")
    endif()
    # The after hook runs even when the body was cut short, and can see that.
    expect_text("hook-after fail.require_stops failed=1")
    expect_text("hook-after skip.with_reason failed=0")
    # A hook can fail a case whose body passed.
    expect_text("failed-case-body-ran")
    expect_text("error: hooks.is_failed_by_the_after_hook: TB_TEST_FAIL: rejected by the after hook")
    expect_text("[ FAILED  ] hooks.is_failed_by_the_after_hook: 1 of 1 checks failed")
    # A before hook that ends the case keeps the body from running.
    expect_text("[ SKIPPED ] hooks.is_skipped_by_the_before_hook: skipped by the before hook")
    expect_no_text("skipped-case-body-ran")
    expect_text("hook-after hooks.is_skipped_by_the_before_hook failed=0")
    expect_text("[ SUMMARY ] 5 cases: 1 passed, 2 failed, 2 skipped;")
endfunction()

function(scenario_crash_ends_the_process_with_a_failure_status)
    # A debug C runtime must not park the run behind an error dialog.
    run_fixture("crash.aborts")
    if(status STREQUAL "0" OR status MATCHES "timeout")
        fail_scenario("an aborting case must end the process promptly with a failure status")
    endif()
endfunction()

# --- selection and command line -------------------------------------------------

function(scenario_patterns_select_cases)
    run_fixture(--list "order.*")
    expect_status(0)
    expect_count("order." 6)

    run_fixture(--list "order.*" --exclude order.a --exclude=order.b)
    expect_status(0)
    expect_count("order." 4)
    expect_no_text("order.a")
    expect_no_text("order.b")

    run_fixture(--list "*.no_checks" "order.?")
    expect_status(0)
    expect_text("pass.no_checks")
    expect_count("order." 6)

    run_fixture(--list "order")
    expect_status(2)
    expect_diagnostic("no test case matches the given patterns")
endfunction()

function(scenario_cases_run_in_command_line_order)
    # One executable holds every case of the project, so the command line must
    # be able to run any of them, in any order.
    run_fixture(--verbose order.e order.b order.d)
    expect_status(0)
    read_run_order(named_order)
    if(NOT named_order STREQUAL "e;b;d")
        fail_scenario("named cases must run in the order given, got ${named_order}")
    endif()

    # A pattern adds its matches in source order. A case that an earlier
    # argument already selected is not run a second time.
    run_fixture(--verbose order.f "order.*" order.a)
    expect_status(0)
    read_run_order(mixed_order)
    if(NOT mixed_order STREQUAL "f;a;b;c;d;e")
        fail_scenario("expected f first, then the rest in source order, got ${mixed_order}")
    endif()

    run_fixture(--verbose order.d "order.?" --exclude order.b)
    read_run_order(excluded_order)
    if(NOT excluded_order STREQUAL "d;a;c;e;f")
        fail_scenario("an exclusion must apply to every pattern, got ${excluded_order}")
    endif()
endfunction()

function(scenario_bad_command_lines_are_usage_errors)
    run_fixture(--bogus)
    expect_status(2)
    expect_diagnostic("unknown option: --bogus")
    expect_diagnostic("Usage:")

    run_fixture(--seed)
    expect_status(2)
    expect_diagnostic("missing value for option: --seed")

    run_fixture(--repeat 0)
    expect_status(2)
    expect_diagnostic("--repeat expects a count from 1 to 1000000: 0")

    run_fixture(--help)
    expect_status(0)
    expect_text("Usage:")
    expect_text("--fail-fast")
endfunction()

function(scenario_listed_locations_match_the_ctest_registration)
    # The scanner computes each case's line from the source text; the compiler
    # records it through __LINE__. IDE navigation needs the two to agree.
    run_fixture(--list)
    expect_status(0)
    file(READ "${TB_FIXTURE_CASES}" registered)
    string(REPLACE "\n" ";" listed "${output}")
    set(checked 0)
    foreach(entry IN LISTS listed)
        if(entry STREQUAL "")
            continue()
        endif()
        if(NOT entry MATCHES "^([A-Za-z0-9_]+\\.[A-Za-z0-9_]+)\t.*fixture_tests\\.c[(:]([0-9]+)\\)?$")
            fail_scenario("unexpected --list line: ${entry}")
        endif()
        set(key "${CMAKE_MATCH_1}")
        set(line "${CMAKE_MATCH_2}")
        string(FIND "${registered}" "add_test([==[${key}]==] " position)
        if(position EQUAL -1)
            fail_scenario("${key} is not registered in ${TB_FIXTURE_CASES}")
        endif()
        string(SUBSTRING "${registered}" ${position} -1 registration)
        if(NOT registration MATCHES "^[^\n]*\n[^\n]*\n[^\n]*\n[^\n]*fixture_tests\\.c:${line}\\]==\\]\\)")
            fail_scenario("${key} is listed at line ${line} but registered elsewhere")
        endif()
        math(EXPR checked "${checked} + 1")
    endforeach()
    string(REGEX MATCHALL "add_test\\(" registrations "${registered}")
    list(LENGTH registrations registration_count)
    if(NOT checked EQUAL registration_count)
        fail_scenario("${checked} cases listed but ${registration_count} registered")
    endif()
endfunction()

# --- seeds, order, and repetition -----------------------------------------------

# Extracts "random-values[<label>]=..." from stdout into result_variable.
function(read_random_values label result_variable)
    if(NOT output MATCHES "random-values\\[${label}\\]=([0-9a-f,]+)")
        fail_scenario("missing random values of the ${label} stream")
    endif()
    set(${result_variable} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(scenario_random_inputs_depend_only_on_seed_and_case)
    run_fixture("random.*")
    expect_status(0)
    expect_text("random-seed[first]=${default_seed}")
    read_random_values(first default_first)
    read_random_values(second default_second)
    if(default_first STREQUAL default_second)
        fail_scenario("two cases must not share a random stream")
    endif()

    # Same seed, one case alone, the other order: the stream must not change.
    run_fixture("random.first_stream")
    read_random_values(first alone_first)
    run_fixture("random.*" --shuffle --seed "${default_seed}")
    read_random_values(first shuffled_first)
    if(NOT default_first STREQUAL alone_first OR NOT default_first STREQUAL shuffled_first)
        fail_scenario("a case's random stream changed with the selection or order")
    endif()

    run_fixture("random.*" --seed 12345)
    expect_text("random-seed[first]=0x3039")
    read_random_values(first reseeded_first)
    if(default_first STREQUAL reseeded_first)
        fail_scenario("--seed did not change the random stream")
    endif()
    run_fixture("random.*" --seed=12345)
    read_random_values(first repeated_first)
    if(NOT reseeded_first STREQUAL repeated_first)
        fail_scenario("the same --seed must reproduce the same random stream")
    endif()
endfunction()

# Extracts the order of the "[ RUN     ] order.x" lines into result_variable.
function(read_run_order result_variable)
    string(REGEX MATCHALL "\\[ RUN     \\] order\\.[a-f]" lines "${output}")
    string(REPLACE "[ RUN     ] order." "" order "${lines}")
    set(${result_variable} "${order}" PARENT_SCOPE)
endfunction()

function(scenario_shuffle_is_reproducible_from_its_seed)
    run_fixture(--verbose "order.*")
    read_run_order(registry_order)
    if(NOT registry_order STREQUAL "a;b;c;d;e;f")
        fail_scenario("without --shuffle the cases must run in source order")
    endif()

    set(shuffled_differently FALSE)
    foreach(seed RANGE 1 4)
        run_fixture(--verbose --shuffle --seed ${seed} "order.*")
        expect_status(0)
        read_run_order(first_order)
        run_fixture(--verbose --shuffle --seed ${seed} "order.*")
        read_run_order(second_order)
        if(NOT first_order STREQUAL second_order)
            fail_scenario("--shuffle --seed ${seed} gave two different orders")
        endif()
        list(LENGTH first_order ran)
        if(NOT ran EQUAL 6)
            fail_scenario("--shuffle must still run every selected case once")
        endif()
        if(NOT first_order STREQUAL registry_order)
            set(shuffled_differently TRUE)
        endif()
    endforeach()
    if(NOT shuffled_differently)
        fail_scenario("--shuffle never changed the order")
    endif()
endfunction()

function(scenario_repeat_runs_rounds_with_advancing_seeds)
    run_fixture(--repeat 3 "pass.*")
    expect_status(0)
    expect_text("[ SUMMARY ] 6 cases: 6 passed, 0 failed, 0 skipped; 9 checks;")

    run_fixture(--repeat=2 --seed 100 "random.first_stream")
    expect_status(0)
    expect_text("random-seed[first]=0x64")
    expect_text("random-seed[first]=0x65")
endfunction()

# --- source scanner --------------------------------------------------------------

function(scenario_scan_registers_cases_with_their_lines)
    prepare_work_directory()
    set(unix_source "${work_directory}/unix_lines.c")
    set(windows_source "${work_directory}/windows_lines.c")
    file(WRITE "${unix_source}"
        "#include \"tb_test.h\"\n"
        "\n"
        "TB_TEST(alpha, first)\n"
        "{\n"
        "}\n"
        "\n"
        "    TB_TEST( alpha ,second_case2 )\n"
        "{\n"
        "}\n")
    file(WRITE "${windows_source}"
        "/* Carriage returns must not shift line numbers. */\r\n"
        "\r\n"
        "\r\n"
        "TB_TEST(beta, only)\r\n"
        "{\r\n"
        "}\r\n")
    run_scan("${unix_source}" "${windows_source}")
    expect_status(0)

    expect_file_text("${registry}" "extern const tb_test_case tb_test_case_alpha__first;")
    expect_file_text("${registry}" "    &tb_test_case_alpha__first,\n    &tb_test_case_alpha__second_case2,\n    &tb_test_case_beta__only,\n")
    expect_file_text("${registry}" "int tb_test_main(int argc, char **argv)")

    expect_file_text("${cases}"
        "add_test([==[pre.alpha.first.post]==] \"\${TB_TEST_EXECUTABLE}\" [==[alpha.first]==])")
    expect_file_text("${cases}" "SKIP_RETURN_CODE 77")
    expect_file_text("${cases}" "DEF_SOURCE_LINE [==[${unix_source}:3]==])")
    expect_file_text("${cases}" "DEF_SOURCE_LINE [==[${unix_source}:7]==])")
    expect_file_text("${cases}" "DEF_SOURCE_LINE [==[${windows_source}:4]==])")
endfunction()

function(scenario_scan_ignores_text_that_is_not_a_definition)
    prepare_work_directory()
    set(source "${work_directory}/lookalikes.c")
    file(WRITE "${source}"
        "// TB_TEST(commented, out)\n"
        "#define MY_TB_TEST(a, b) TB_TEST(a, b)\n"
        "MY_TB_TEST(wrapped, macro)\n"
        "TB_TEST(real, case_one)\n"
        "{\n"
        "    TB_TEST_EXPECT(true);\n"
        "    TB_TEST_REQUIRE(true); /* TB_TEST(inline, comment) */\n"
        "    int x = 1; TB_TEST(mid, line)\n"
        "}\n")
    run_scan("${source}")
    expect_status(0)
    expect_file_text("${registry}" "tb_test_case_real__case_one")
    expect_no_file_text("${registry}" "commented")
    expect_no_file_text("${registry}" "wrapped")
    expect_no_file_text("${registry}" "inline")
    expect_no_file_text("${registry}" "mid")
endfunction()

function(scenario_scan_rejects_duplicate_case_names)
    prepare_work_directory()
    set(first "${work_directory}/first.c")
    set(second "${work_directory}/second.c")
    file(WRITE "${first}" "TB_TEST(same, name)\n{\n}\n")
    file(WRITE "${second}" "\nTB_TEST(same, name)\n{\n}\n")
    run_scan("${first}" "${second}")
    if(status STREQUAL "0")
        fail_scenario("two cases with one name must fail the build")
    endif()
    expect_diagnostic("test case same.name is already defined at")
    expect_diagnostic("second.c:2")
    expect_diagnostic("first.c:1")
endfunction()

function(scenario_scan_rejects_sources_without_cases)
    prepare_work_directory()
    set(source "${work_directory}/empty.c")
    file(WRITE "${source}" "int unused;\n")
    run_scan("${source}")
    if(status STREQUAL "0")
        fail_scenario("an executable without cases must fail the build")
    endif()
    expect_diagnostic("No TB_TEST(suite, name) definition found for scanned_target")
endfunction()

# --- dispatch ---------------------------------------------------------------------

if(NOT COMMAND "scenario_${TB_SCENARIO}")
    message(FATAL_ERROR "Unknown scenario: ${TB_SCENARIO}")
endif()
cmake_language(CALL "scenario_${TB_SCENARIO}")
