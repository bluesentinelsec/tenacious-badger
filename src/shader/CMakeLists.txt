# The shader compiler: HLSL written once, for every backend.
set(TB_SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shader.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_condition.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_lexer.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_path.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_preprocessor.c"
    CACHE INTERNAL "Shader compiler sources")
target_sources(badger PRIVATE ${TB_SHADER_SOURCES})
target_sources(badger_static PRIVATE ${TB_SHADER_SOURCES})
