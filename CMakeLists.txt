cmake_minimum_required(VERSION 3.25)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum supported macOS")
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TB_VERSION LIMIT_COUNT 1)
if(NOT TB_VERSION MATCHES "^0\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a development semantic version: 0.x.y")
endif()
project(TenaciousBadger VERSION "${TB_VERSION}" LANGUAGES C)

set(TB_PUBLISHER "bluesentinelsec")
set(TB_APP_ID "io.github.${TB_PUBLISHER}.burrow")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} "${PROJECT_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config} "${PROJECT_BINARY_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config} "${PROJECT_BINARY_DIR}/lib")
endforeach()

option(TB_BUILD_BURROW "Build the Burrow showcase executable" ${PROJECT_IS_TOP_LEVEL})
option(TB_BUILD_SANDBOX "Build badger-sandbox, which draws test scenes to look at by eye"
    ${PROJECT_IS_TOP_LEVEL})
option(TB_BUILD_TESTS "Build the in-house test harness" ${PROJECT_IS_TOP_LEVEL})
option(TB_BUILD_BENCHMARKS "Build the benchmarks of the foundation modules" ${PROJECT_IS_TOP_LEVEL})
option(TB_ENABLE_SANITIZERS
    "Add standalone Debug test binaries with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(TB_ENABLE_THREAD_SANITIZER
    "Add standalone Debug test binaries with ThreadSanitizer, in place of the other sanitizers"
    OFF)
option(TB_USE_DEFAULT_PLATFORM_ADAPTER
    "Build with the platform adapter whose every function reports \"not implemented\"" OFF)
option(TB_ENABLE_STATIC_ANALYSIS
    "Analyze every project-owned target with the compiler's static analyzer" OFF)
option(TB_ENABLE_COVERAGE
    "Record which lines of the engine the tests run, with the compiler's instrumentation" OFF)
set(TB_EMBED_FONTS "defaults" CACHE STRING
    "Which fonts of resources/fonts the engine library carries: defaults (the families of the default font roles), all, or none")
set_property(CACHE TB_EMBED_FONTS PROPERTY STRINGS all defaults none)

include(cmake/WindowsSigning.cmake)
# tb_add_package_key, for the engine's tests and for games that build it in their tree.
include(cmake/BadgerPackageKey.cmake)
include(cmake/StaticAnalysis.cmake)
include(cmake/Coverage.cmake)
include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)
include(cmake/EmbedFonts.cmake)
configure_file(cmake/version.h.in generated/badger/version.h @ONLY)
add_subdirectory(src)
include(cmake/InstallSDK.cmake)

if(TB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    tb_add_windows_signing_tests()
endif()

if(TB_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# After the tests, so that its smoke tests are registered with them.
if(TB_BUILD_SANDBOX)
    add_subdirectory(tools/sandbox)
endif()

include(cmake/Formatting.cmake)
