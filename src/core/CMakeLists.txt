# The SIMD kernels of badger/math.h are chosen by the processor the build
# targets: SSE2 on x64, NEON on 64-bit ARM, and portable C on anything else,
# including a macOS build for more than one architecture at once.
if(CMAKE_C_COMPILER_ARCHITECTURE_ID)
    set(tb_math_processor "${CMAKE_C_COMPILER_ARCHITECTURE_ID}")
elseif(CMAKE_OSX_ARCHITECTURES)
    set(tb_math_processor "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(tb_math_processor "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${tb_math_processor}" tb_math_processor)
if(tb_math_processor MATCHES "^(x86_64|amd64|x64)$")
    set(tb_math_kernels math_sse2.c)
elseif(tb_math_processor MATCHES "^(arm64|aarch64)$")
    set(tb_math_kernels math_neon.c)
else()
    set(tb_math_kernels math_kernel_portable.c)
endif()
message(STATUS "badger: math kernels for '${tb_math_processor}': ${tb_math_kernels}")

set(TB_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/allocator.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ap.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/arena.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/array.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/assert.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/atom.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/bit.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/bytes.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/cli.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/cli_completion.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/d3dcompiler.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/decimal.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/dxc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/exclusive.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fatal.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fmt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fmt_float.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/frame_memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gamepad.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/geometry.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/hash.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/library.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/list.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/log.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/math.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/math_portable.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/${tb_math_kernels}"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory_debug.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/path.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/pool.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ring.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/rng.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/set.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/size.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sort.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/str.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stream.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stream_file.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stream_memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sum.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/table.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/text.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/utf8.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/virtual_memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/window.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/xp.c"
    CACHE INTERNAL "Platform-independent core sources")
target_sources(badger PRIVATE ${TB_CORE_SOURCES})
target_sources(badger_static PRIVATE ${TB_CORE_SOURCES})
