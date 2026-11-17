# Standalone test executables can be built with one of two kinds of sanitizer.
# They cannot be combined in one executable, so a build tree has one kind:
#
#   TB_ENABLE_SANITIZERS        AddressSanitizer, with UndefinedBehaviorSanitizer
#                               on GCC and Clang: memory errors and undefined
#                               behavior.
#   TB_ENABLE_THREAD_SANITIZER  ThreadSanitizer, on GCC and Clang: data races
#                               and lock-order errors. MSVC has none.
#
# TB_SANITIZER is "address", "thread", or empty when neither is on.
if(TB_ENABLE_SANITIZERS AND TB_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR
        "TB_ENABLE_SANITIZERS and TB_ENABLE_THREAD_SANITIZER cannot both be ON: "
        "AddressSanitizer and ThreadSanitizer cannot instrument the same executable. "
        "Configure a build directory for each.")
endif()
set(TB_SANITIZER "")
if(TB_ENABLE_SANITIZERS)
    set(TB_SANITIZER "address")
elseif(TB_ENABLE_THREAD_SANITIZER)
    set(TB_SANITIZER "thread")
endif()
if(TB_SANITIZER AND NOT TB_BUILD_TESTS)
    message(FATAL_ERROR "The sanitizers instrument the tests; they require TB_BUILD_TESTS")
endif()
if(TB_SANITIZER STREQUAL "thread" AND NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    message(FATAL_ERROR
        "ThreadSanitizer is not available for ${CMAKE_C_COMPILER_ID}. "
        "It runs on Linux and macOS with GCC or Clang; CI runs it there on every pull request.")
endif()

if(TB_SANITIZER STREQUAL "address" AND MSVC)
    # MSVC's default runtime checks conflict with AddressSanitizer; ASan replaces
    # those checks in this development build. This does not suppress warnings.
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

function(tb_enable_sanitizers target)
    if(TB_SANITIZER STREQUAL "thread")
        # Tests read it to leave out what ThreadSanitizer cannot run.
        target_compile_definitions(${target} PRIVATE "$<$<CONFIG:Debug>:TB_SANITIZE_THREAD>")
        target_compile_options(${target} PRIVATE
            "$<$<CONFIG:Debug>:-fsanitize=thread>"
            "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>")
        target_link_options(${target} PRIVATE "$<$<CONFIG:Debug>:-fsanitize=thread>")
        return()
    endif()
    # Engine allocators that manage memory inside a block tell AddressSanitizer
    # which bytes are in use (src/core/sanitizer.h). Tests read it to know
    # that touching released memory ends the process.
    target_compile_definitions(${target} PRIVATE "$<$<CONFIG:Debug>:TB_SANITIZE_ADDRESS>")
    if(MSVC)
        target_compile_options(${target} PRIVATE "$<$<CONFIG:Debug>:/fsanitize=address>")
        target_link_options(${target} PRIVATE "$<$<CONFIG:Debug>:/INCREMENTAL:NO>")
    elseif(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
            "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
            "$<$<CONFIG:Debug>:-fno-sanitize-recover=all>")
        target_link_options(${target} PRIVATE
            "$<$<CONFIG:Debug>:-fsanitize=address,undefined>")
    else()
        message(FATAL_ERROR "Sanitizers are not configured for this compiler")
    endif()
endfunction()
