# The key that a game's packages are obfuscated with, compiled into the game
# so that no tool such as `strings`, and no search for the key's bytes, finds
# it in the executable. It needs nothing but CMake.
#
#   tb_add_package_key(<target> FUNCTION <name> KEY_FILE <path>)
#   tb_add_package_key(<target> FUNCTION <name> PASSWORD_FILE <path>)
#
# Either form compiles into <target> a C function
#
#   void <name>(uint8_t key[32]);
#
# declared in <name>.h, which the target can include, that writes the key
# into `key` when it is called. The packaging tools are given the same key
# file or password, so they and the game agree.
#
# KEY_FILE names a file of 64 hexadecimal digits: the key. If the file does
# not exist, a new key is made and written there, and kept for every later
# configure. Keep it where it lasts, and out of version control unless the
# project means to share it; anyone with it can read the packages.
#
# PASSWORD_FILE names a file whose first line is a password. The key is the
# SHA-256 of a fixed label and the password, so the password itself is never
# compiled in.
#
# How the key is hidden: it is split into three parts whose XOR is the key,
# each of 32 bytes in an array of its own and in an order of its own, and the
# function puts them together when it runs. The parts are read through a
# volatile pointer, so no compiler folds them back into the key. This defeats
# `strings` and a search of the executable for the key; it does not defeat a
# debugger, which can watch the function run. Obfuscation is not security.
#
# A key made here comes from /dev/urandom on Linux and macOS. On Windows CMake
# has no such source, and the key is the SHA-256 of CMake's own random string,
# whose seed holds 32 bits from the system, with the time to the microsecond.
# That is ample for obfuscation, which a debugger defeats anyway; a project
# that wants more makes the key file elsewhere.

# The label hashed with a password, so a key made from one cannot be mistaken
# for the plain SHA-256 of the password.
set(TB_PACKAGE_KEY_LABEL "Tenacious Badger package key v1:")

# Bytes of the hexadecimal digits `hex` as a CMake list of numbers.
function(_tb_package_key_bytes hex out)
    set(bytes "")
    foreach(index RANGE 0 62 2)
        string(SUBSTRING "${hex}" ${index} 2 pair)
        math(EXPR value "0x${pair}")
        list(APPEND bytes ${value})
    endforeach()
    set(${out} "${bytes}" PARENT_SCOPE)
endfunction()

# A new key, as 64 hexadecimal digits.
function(_tb_package_key_generate out)
    set(entropy "")
    if(EXISTS "/dev/urandom")
        file(READ "/dev/urandom" urandom LIMIT 32 HEX)
        string(APPEND entropy "${urandom}")
    endif()
    string(RANDOM LENGTH 64 ALPHABET "0123456789abcdef" random)
    string(TIMESTAMP now "%Y%m%d%H%M%S%f" UTC)
    string(SHA256 key "${entropy}:${random}:${now}")
    set(${out} "${key}" PARENT_SCOPE)
endfunction()

# The key, as 64 lowercase hexadecimal digits, from a key file (made if it is
# missing) or a password file. Exactly one of the two is given.
function(_tb_package_key_resolve out key_file password_file)
    if(key_file AND password_file)
        message(FATAL_ERROR "tb_add_package_key: give KEY_FILE or PASSWORD_FILE, not both")
    endif()
    if(password_file)
        if(NOT EXISTS "${password_file}")
            message(FATAL_ERROR "tb_add_package_key: no password file at ${password_file}")
        endif()
        file(STRINGS "${password_file}" lines LIMIT_COUNT 1)
        list(GET lines 0 password)
        if(password STREQUAL "")
            message(FATAL_ERROR "tb_add_package_key: ${password_file} has an empty password")
        endif()
        string(SHA256 key "${TB_PACKAGE_KEY_LABEL}${password}")
    elseif(key_file)
        if(NOT EXISTS "${key_file}")
            _tb_package_key_generate(key)
            file(WRITE "${key_file}" "${key}\n")
            message(STATUS "Made a new package key: ${key_file}")
        endif()
        file(STRINGS "${key_file}" lines LIMIT_COUNT 1)
        list(GET lines 0 key)
        string(STRIP "${key}" key)
        string(TOLOWER "${key}" key)
        if(NOT key MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR "tb_add_package_key: ${key_file} must hold 64 hexadecimal digits")
        endif()
        string(LENGTH "${key}" length)
        if(NOT length EQUAL 64)
            message(FATAL_ERROR "tb_add_package_key: ${key_file} must hold 64 hexadecimal digits")
        endif()
    else()
        message(FATAL_ERROR "tb_add_package_key: give KEY_FILE or PASSWORD_FILE")
    endif()
    set(${out} "${key}" PARENT_SCOPE)
endfunction()

# A C array initializer of a list of numbers.
function(_tb_package_key_array values out)
    list(JOIN values ", " joined)
    set(${out} "{${joined}}" PARENT_SCOPE)
endfunction()

# Write <directory>/<function>.c and .h, which put `key` back together.
# The parts and their order are derived from the key, so the same key makes
# the same source, and builds stay reproducible.
function(_tb_package_key_write_source key function directory)
    if(NOT function MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "tb_add_package_key: FUNCTION must be a C identifier, not ${function}")
    endif()
    string(SHA256 mask_hex_1 "mask 1:${key}")
    string(SHA256 mask_hex_2 "mask 2:${key}")
    string(SHA256 order_hex "order:${key}")
    _tb_package_key_bytes("${key}" key_bytes)
    _tb_package_key_bytes("${mask_hex_1}" mask_1)
    _tb_package_key_bytes("${mask_hex_2}" mask_2)
    _tb_package_key_bytes("${order_hex}" order_bytes)
    # A permutation of 0 to 31, shuffled by the bytes of `order`.
    set(order "")
    foreach(index RANGE 0 31)
        list(APPEND order ${index})
    endforeach()
    foreach(index RANGE 31 1 -1)
        list(GET order_bytes ${index} random)
        math(EXPR other "${random} % (${index} + 1)")
        list(GET order ${index} first)
        list(GET order ${other} second)
        list(REMOVE_AT order ${index})
        list(INSERT order ${index} ${second})
        list(REMOVE_AT order ${other})
        list(INSERT order ${other} ${first})
    endforeach()
    # Part i of the key is stored at position order[i] of each array.
    set(part_a "")
    set(part_b "")
    set(part_c "")
    foreach(slot RANGE 0 31)
        list(APPEND part_a 0)
        list(APPEND part_b 0)
        list(APPEND part_c 0)
    endforeach()
    foreach(index RANGE 0 31)
        list(GET order ${index} slot)
        list(GET key_bytes ${index} k)
        list(GET mask_1 ${index} m1)
        list(GET mask_2 ${index} m2)
        math(EXPR a "${k} ^ ${m1} ^ ${m2}")
        list(REMOVE_AT part_a ${slot})
        list(INSERT part_a ${slot} ${a})
        list(REMOVE_AT part_b ${slot})
        list(INSERT part_b ${slot} ${m1})
        list(REMOVE_AT part_c ${slot})
        list(INSERT part_c ${slot} ${m2})
    endforeach()
    _tb_package_key_array("${part_a}" array_a)
    _tb_package_key_array("${part_b}" array_b)
    _tb_package_key_array("${part_c}" array_c)
    _tb_package_key_array("${order}" array_order)
    string(TOUPPER "${function}" guard)
    file(CONFIGURE OUTPUT "${directory}/${function}.h" CONTENT
"/* Generated by BadgerPackageKey.cmake. Do not edit, and do not commit: it
 * holds the package key, hidden. */
#ifndef TB_PACKAGE_KEY_${guard}_H
#define TB_PACKAGE_KEY_${guard}_H

#include <stdint.h>

/* Write the package key into key[0] to key[31]. Thread-safe. */
void ${function}(uint8_t key[32]);

#endif
")
    file(CONFIGURE OUTPUT "${directory}/${function}.c" CONTENT
"/* Generated by BadgerPackageKey.cmake. Do not edit, and do not commit: it
 * holds the package key, as three parts whose XOR is the key. */
#include \"${function}.h\"

#include <stddef.h>

/* Read through volatile, so that no compiler folds the parts into the key. */
static const volatile uint8_t ${function}_a[32] = ${array_a};
static const volatile uint8_t ${function}_b[32] = ${array_b};
static const volatile uint8_t ${function}_c[32] = ${array_c};
static const uint8_t ${function}_order[32] = ${array_order};

void ${function}(uint8_t key[32])
{
    for (size_t index = 0; index < 32; index++)
    {
        const size_t slot = ${function}_order[index];
        key[index] = (uint8_t)(${function}_a[slot] ^ ${function}_b[slot] ^ ${function}_c[slot]);
    }
}
")
endfunction()

function(tb_add_package_key target)
    cmake_parse_arguments(PARSE_ARGV 1 key "" "FUNCTION;KEY_FILE;PASSWORD_FILE" "")
    if(key_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "tb_add_package_key: unknown arguments ${key_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT key_FUNCTION)
        message(FATAL_ERROR "tb_add_package_key: give FUNCTION, the name of the C function")
    endif()
    _tb_package_key_resolve(key "${key_KEY_FILE}" "${key_PASSWORD_FILE}")
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/package_keys/${key_FUNCTION}")
    _tb_package_key_write_source("${key}" "${key_FUNCTION}" "${directory}")
    target_sources(${target} PRIVATE "${directory}/${key_FUNCTION}.c")
    target_include_directories(${target} PRIVATE "${directory}")
    # A new key or password in the file makes CMake configure again.
    foreach(file IN ITEMS "${key_KEY_FILE}" "${key_PASSWORD_FILE}")
        if(file)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
        endif()
    endforeach()
endfunction()
