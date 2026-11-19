# Checks the keys that tb_add_package_key compiles in (cmake/BadgerPackageKey.cmake):
# the probe prints the key of a key file and of a password, which must be the
# file's key and the SHA-256 of the label and the password; and neither key
# may appear in the probe's bytes, forwards or backwards.
#
#   TB_PROBE          The probe executable, tests/tools/package_key_probe.c.
#   TB_KEY_FILE       The key file it was built with.
#   TB_PASSWORD_FILE  The password file it was built with.
#   TB_MODULE         cmake/BadgerPackageKey.cmake, for the label.

include("${TB_MODULE}")
execute_process(COMMAND "${TB_PROBE}" OUTPUT_VARIABLE output RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "The probe failed: ${status}")
endif()
string(REPLACE "\n" ";" lines "${output}")
list(GET lines 0 file_key)
list(GET lines 1 password_key)

file(STRINGS "${TB_KEY_FILE}" expected_file_key LIMIT_COUNT 1)
file(STRINGS "${TB_PASSWORD_FILE}" password LIMIT_COUNT 1)
string(SHA256 expected_password_key "${TB_PACKAGE_KEY_LABEL}${password}")
if(NOT file_key STREQUAL expected_file_key)
    message(FATAL_ERROR "The key of the key file came back as ${file_key}, not ${expected_file_key}")
endif()
if(NOT password_key STREQUAL expected_password_key)
    message(FATAL_ERROR "The key of the password came back as ${password_key}, not ${expected_password_key}")
endif()

# The executable's bytes, in hexadecimal: a key would be its 64 digits at an
# even offset. Eight bytes of a key are enough to find it.
file(READ "${TB_PROBE}" binary HEX)
foreach(key IN ITEMS "${file_key}" "${password_key}")
    foreach(start RANGE 0 48 16)
        string(SUBSTRING "${key}" ${start} 16 piece)
        string(FIND "${binary}" "${piece}" at)
        while(NOT at EQUAL -1)
            math(EXPR odd "${at} % 2")
            if(odd EQUAL 0)
                message(FATAL_ERROR "Bytes ${start} to ${start}+8 of a key are in the executable at ${at}")
            endif()
            math(EXPR next "${at} + 1")
            string(SUBSTRING "${binary}" ${next} -1 rest)
            string(FIND "${rest}" "${piece}" found)
            if(found EQUAL -1)
                set(at -1)
            else()
                math(EXPR at "${next} + ${found}")
            endif()
        endwhile()
    endforeach()
endforeach()
message(STATUS "Both keys came back, and neither is in the executable.")
