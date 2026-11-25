# badger-sandbox: draws the engine's test scenes in a window, to look at by
# eye. A developer tool: built with the engine, never shipped with a game.
add_executable(badger-sandbox sandbox.c sandbox_capture.c sandbox_scenes.c sandbox_skinning.c
    sandbox_gui.c sandbox_gui_options.c sandbox_gui_tool.c sandbox_inspector.c
    sandbox_lighting.c sandbox_ui.c)
target_link_libraries(badger-sandbox PRIVATE TenaciousBadger::badger)

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
    add_test(NAME sandbox.draws_the_interface_on_the_null_backend
        COMMAND badger-sandbox --renderer null --scene ui --frames 3)
    add_test(NAME sandbox.draws_the_gui_on_the_null_backend
        COMMAND badger-sandbox --renderer null --scene gui --frames 3)
    add_test(NAME sandbox.draws_the_gui_tool_on_the_null_backend
        COMMAND badger-sandbox --renderer null --scene gui_tool --frames 3)
    add_test(NAME sandbox.shows_the_inspector_on_the_null_backend
        COMMAND badger-sandbox --renderer null --scene shapes --inspector --frames 3)
    add_test(NAME sandbox.lights_materials_on_the_null_backend
        COMMAND badger-sandbox --renderer null --scene lighting --frames 3)
    # A machine without a display, as some CI runners are, has no window.
    set_tests_properties(sandbox.runs_frames_on_the_null_backend
        sandbox.draws_text_on_the_null_backend sandbox.draws_the_interface_on_the_null_backend
        sandbox.draws_the_gui_on_the_null_backend sandbox.draws_the_gui_tool_on_the_null_backend
        sandbox.shows_the_inspector_on_the_null_backend
        sandbox.lights_materials_on_the_null_backend PROPERTIES
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
        # The raster lighting mode: materials under moving lights, under validation.
        add_test(NAME sandbox.lights_materials_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene lighting
                --frames 3 --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_lighting.bmp")
        set_tests_properties(sandbox.lights_materials_on_d3d12 PROPERTIES
            PASS_REGULAR_EXPRESSION "saved")
        # A cube map's faces uploaded and mipmapped, drawn as the sky.
        add_test(NAME sandbox.draws_a_cube_sky_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene sky_cube
                --frames 3 --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_sky_cube.bmp")
        set_tests_properties(sandbox.draws_a_cube_sky_on_d3d12 PROPERTIES
            PASS_REGULAR_EXPRESSION "saved")
        # Text: paragraphs, alignment, pixel fonts and labels.
        add_test(NAME sandbox.draws_text_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene text
                --frames 3 --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_text.bmp")
        set_tests_properties(sandbox.draws_text_on_d3d12 PROPERTIES
            PASS_REGULAR_EXPRESSION "saved")
        # Skinning: clips blended on the job system, skinned by a compute pass.
        add_test(NAME sandbox.draws_skinning_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene skinning
                --frames 3 --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_skinning.bmp")
        set_tests_properties(sandbox.draws_skinning_on_d3d12 PROPERTIES
            PASS_REGULAR_EXPRESSION "saved")
        # The same skinned on the CPU, from the frame's uploads, with the
        # inspector saying so.
        add_test(NAME sandbox.draws_skinning_on_the_cpu_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene skinning
                --cpu-skinning --inspector --frames 3
                --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_skinning_cpu.bmp")
        set_tests_properties(sandbox.draws_skinning_on_the_cpu_on_d3d12 PROPERTIES
            PASS_REGULAR_EXPRESSION "saved")
        # The inspector: every setting, and the frame's statistics.
        add_test(NAME sandbox.shows_the_inspector_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene shapes
                --inspector --frames 3 --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_inspector.bmp")
        set_tests_properties(sandbox.shows_the_inspector_on_d3d12 PROPERTIES
            PASS_REGULAR_EXPRESSION "saved")
        # The interface: windows of every widget, over the shapes.
        add_test(NAME sandbox.draws_the_interface_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene ui
                --frames 3 --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_ui.bmp")
        set_tests_properties(sandbox.draws_the_interface_on_d3d12 PROPERTIES
            PASS_REGULAR_EXPRESSION "saved")
        # The retained GUI: a form laid out by grid, at two scales.
        add_test(NAME sandbox.draws_the_gui_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene gui
                --frames 3 --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_gui.bmp")
        add_test(NAME sandbox.draws_the_gui_larger_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene gui
                --ui-scale 150 --frames 3
                --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_gui_150.bmp")
        set_tests_properties(sandbox.draws_the_gui_on_d3d12 sandbox.draws_the_gui_larger_on_d3d12
            PROPERTIES PASS_REGULAR_EXPRESSION "saved")
        # The retained GUI as an editor.
        add_test(NAME sandbox.draws_the_gui_tool_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene gui_tool
                --frames 3 --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_gui_tool.bmp")
        set_tests_properties(sandbox.draws_the_gui_tool_on_d3d12 PROPERTIES
            PASS_REGULAR_EXPRESSION "saved")
    endif()
endif()
