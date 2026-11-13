cmake_minimum_required(VERSION 3.25)
project(BadgerFetchConsumer LANGUAGES C)
include(FetchContent)
# A local source override exercises the same integration without any download.
if(NOT TB_SOURCE_ROOT)
    message(FATAL_ERROR "Set TB_SOURCE_ROOT to the Tenacious Badger source directory")
endif()
file(TO_CMAKE_PATH "${TB_SOURCE_ROOT}" TB_SOURCE_ROOT)
FetchContent_Declare(tenacious_badger SOURCE_DIR "${TB_SOURCE_ROOT}")
FetchContent_MakeAvailable(tenacious_badger)
if(TARGET burrow OR TARGET tb_core_tests)
    message(FATAL_ERROR "Embedded builds must default to libraries only")
endif()
add_executable(fetch_consumer ../static/main.c)
target_link_libraries(fetch_consumer PRIVATE TenaciousBadger::badger)
set_target_properties(fetch_consumer PROPERTIES
    C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(WIN32)
    add_custom_command(TARGET fetch_consumer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:badger>" "$<TARGET_FILE_DIR:fetch_consumer>"
        VERBATIM)
endif()
tb_sign_windows_target(fetch_consumer)
enable_testing()
add_test(NAME shared.fetch_content COMMAND fetch_consumer)
tb_add_windows_signing_tests()
