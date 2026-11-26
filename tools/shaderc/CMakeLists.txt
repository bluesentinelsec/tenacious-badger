# badger-shaderc: compiles shaders ahead of time into shader libraries, as
# Unreal's ShaderCompileWorker does. A developer tool: built with the engine,
# never shipped with a game. Linked as the tests are, coverage included,
# since the engine library it links may be built for coverage.
add_executable(badger-shaderc shaderc.c)
target_link_libraries(badger-shaderc PRIVATE TenaciousBadger::badger_static)
tb_configure_target(badger-shaderc)
tb_analyze_target(badger-shaderc)

# The engine's shaders, compiled when the engine is built into a library for
# each backend of this platform that needs compiled shaders, beside the
# executables: bin/Shaders/d3d12.tbsl and d3d11.tbsl. The renderer finds them there by the
# keys it compiles them by. shaders/Engine/Shaders.manifest lists them.
set(TB_SHADER_MANIFEST "${PROJECT_SOURCE_DIR}/shaders/Engine/Shaders.manifest")
set(TB_SHADER_LIBRARY_DIRECTORY "${PROJECT_BINARY_DIR}/bin/Shaders")
# The backends this platform builds whose shaders are compiled ahead of time.
# D3D12's are DXIL, compiled by DXC; D3D11's DXBC, compiled by D3DCompiler,
# part of Windows. The other backends' compilers come later.
set(TB_SHADER_BACKENDS "")
if(WIN32)
    list(APPEND TB_SHADER_BACKENDS d3d12 d3d11)
endif()
set(TB_SHADER_LIBRARIES "")
set(backend_arguments "")
foreach(backend IN LISTS TB_SHADER_BACKENDS)
    list(APPEND TB_SHADER_LIBRARIES "${TB_SHADER_LIBRARY_DIRECTORY}/${backend}.tbsl")
    list(APPEND backend_arguments --backend ${backend})
endforeach()
if(TB_SHADER_LIBRARIES)
    file(GLOB_RECURSE shader_files CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/shaders/*.hlsl" "${PROJECT_SOURCE_DIR}/shaders/*.hlsli")
    add_custom_command(OUTPUT ${TB_SHADER_LIBRARIES}
        COMMAND badger-shaderc --quiet --root "${PROJECT_SOURCE_DIR}/shaders"
            --output "${TB_SHADER_LIBRARY_DIRECTORY}" ${backend_arguments} "${TB_SHADER_MANIFEST}"
        DEPENDS badger-shaderc "${TB_SHADER_MANIFEST}" ${shader_files}
        COMMENT "Compiling the engine's shaders for ${TB_SHADER_BACKENDS}"
        VERBATIM)
    message(STATUS "badger: compiling the engine's shaders ahead of time for ${TB_SHADER_BACKENDS}")
else()
    message(STATUS "badger: no backend of ${CMAKE_SYSTEM_NAME} needs shaders compiled ahead of "
        "time yet; no shader library is built")
endif()
add_custom_target(badger_shaders ALL DEPENDS ${TB_SHADER_LIBRARIES})
set(TB_SHADER_LIBRARIES "${TB_SHADER_LIBRARIES}" CACHE INTERNAL "The engine's shader libraries")

if(TB_BUILD_TESTS)
    add_test(NAME shaderc.reads_its_manifest_and_compiles_nothing_without_a_backend
        COMMAND badger-shaderc --no-cache "${TB_SHADER_MANIFEST}")
    set_tests_properties(shaderc.reads_its_manifest_and_compiles_nothing_without_a_backend
        PROPERTIES PASS_REGULAR_EXPRESSION "nothing to compile")
    add_test(NAME shaderc.refuses_a_backend_without_libraries
        COMMAND badger-shaderc --no-cache --backend vulkan "${TB_SHADER_MANIFEST}")
    set_tests_properties(shaderc.refuses_a_backend_without_libraries PROPERTIES WILL_FAIL TRUE)
    add_test(NAME shaderc.refuses_a_manifest_with_errors
        COMMAND badger-shaderc --no-cache "${PROJECT_SOURCE_DIR}/shaders/Engine/Private/Sky.hlsl")
    set_tests_properties(shaderc.refuses_a_manifest_with_errors PROPERTIES WILL_FAIL TRUE)
endif()
