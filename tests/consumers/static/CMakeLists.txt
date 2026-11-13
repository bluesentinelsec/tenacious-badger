cmake_minimum_required(VERSION 3.25)
project(BadgerStaticConsumer LANGUAGES C)
find_package(TenaciousBadger CONFIG REQUIRED)
add_executable(static_consumer main.c)
target_link_libraries(static_consumer PRIVATE TenaciousBadger::badger_static)
set_target_properties(static_consumer PROPERTIES
    C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/WindowsSigning.cmake")
tb_sign_windows_target(static_consumer)
enable_testing()
add_test(NAME static.installed_sdk COMMAND static_consumer)
tb_add_windows_signing_tests()
