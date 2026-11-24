# badger-sandbox: draws the engine's test scenes in a window, to look at by
# eye. A developer tool: built with the engine, never shipped with a game.
add_executable(badger-sandbox sandbox.c sandbox_capture.c sandbox_scenes.c)
target_link_libraries(badger-sandbox PRIVATE TenaciousBadger::badger)

# The fonts of the tests' data, built into the sandbox to draw text with, as
# the tests embed them (cmake/EmbedTestData.cmake).
set(sandbox_font_directory "${PROJECT_SOURCE_DIR}/tests/data/fonts")
set(sandbox_font_source "${CMAKE_CURRENT_BINARY_DIR}/sandbox_font_data.c")
file(GLOB sandbox_font_files CONFIGURE_DEPENDS "${sandbox_font_directory}/*.ttf")
add_custom_command(OUTPUT "${sandbox_font_source}"
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${sandbox_font_source}" "-DNAME=sandbox_font_files"
        "-DDIRECTORY=${sandbox_font_directory}" "-DPATTERN=*.ttf"
        -P "${PROJECT_SOURCE_DIR}/cmake/EmbedTestData.cmake"
    DEPENDS ${sandbox_font_files} "${PROJECT_SOURCE_DIR}/cmake/EmbedTestData.cmake"
    COMMENT "Embedding the sandbox's fonts"
    VERBATIM)
target_sources(badger-sandbox PRIVATE "${sandbox_font_source}")
target_include_directories(badger-sandbox PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
tb_configure_target(badger-sandbox)
tb_analyze_target(badger-sandbox)

if(TB_BUILD_TESTS)
    add_test(NAME sandbox.lists_its_scenes COMMAND badger-sandbox list)
    set_tests_properties(sandbox.lists_its_scenes PROPERTIES PASS_REGULAR_EXPRESSION "shapes")
    # A few frames on the null backend, wherever there is a window to open.
    add_test(NAME sandbox.runs_frames_on_the_null_backend
        COMMAND badger-sandbox --renderer null --scene triangle --frames 3)
    add_test(NAME sandbox.draws_text_on_the_null_backend
        COMMAND badger-sandbox --renderer null --scene text --frames 3)
    # A machine without a display, as some CI runners are, has no window.
    set_tests_properties(sandbox.runs_frames_on_the_null_backend
        sandbox.draws_text_on_the_null_backend PROPERTIES
        SKIP_REGULAR_EXPRESSION "no windowing")
    if(WIN32)
        # The shapes drawn by D3D12 on WARP, the last frame saved.
        add_test(NAME sandbox.draws_and_captures_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene shapes
                --frames 3 --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_shapes.bmp")
        set_tests_properties(sandbox.draws_and_captures_on_d3d12 PROPERTIES
            PASS_REGULAR_EXPRESSION "saved")
        # Materials: textures uploaded and mipmapped on the GPU, under validation.
        add_test(NAME sandbox.draws_materials_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene materials
                --frames 3 --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_materials.bmp")
        set_tests_properties(sandbox.draws_materials_on_d3d12 PROPERTIES
            PASS_REGULAR_EXPRESSION "saved")
        # Text: paragraphs, alignment, pixel fonts and labels.
        add_test(NAME sandbox.draws_text_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene text
                --frames 3 --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_text.bmp")
        set_tests_properties(sandbox.draws_text_on_d3d12 PROPERTIES
            PASS_REGULAR_EXPRESSION "saved")
    endif()
endif()
