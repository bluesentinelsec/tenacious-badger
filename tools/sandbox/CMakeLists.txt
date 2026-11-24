# badger-sandbox: draws the engine's test scenes in a window, to look at by
# eye. A developer tool: built with the engine, never shipped with a game.
add_executable(badger-sandbox sandbox.c sandbox_capture.c sandbox_scenes.c)
target_link_libraries(badger-sandbox PRIVATE TenaciousBadger::badger)
tb_configure_target(badger-sandbox)
tb_analyze_target(badger-sandbox)

if(TB_BUILD_TESTS)
    add_test(NAME sandbox.lists_its_scenes COMMAND badger-sandbox list)
    set_tests_properties(sandbox.lists_its_scenes PROPERTIES PASS_REGULAR_EXPRESSION "cube")
    # A few frames on the null backend, wherever there is a window to open.
    add_test(NAME sandbox.runs_frames_on_the_null_backend
        COMMAND badger-sandbox --renderer null --scene triangle --frames 3)
    # A machine without a display, as some CI runners are, has no window.
    set_tests_properties(sandbox.runs_frames_on_the_null_backend PROPERTIES
        SKIP_REGULAR_EXPRESSION "no windowing")
    if(WIN32)
        # The same frames drawn by D3D12 on WARP, the last saved.
        add_test(NAME sandbox.draws_and_captures_on_d3d12
            COMMAND badger-sandbox --renderer d3d12 --software --validation --scene cube
                --frames 3 --capture "${CMAKE_CURRENT_BINARY_DIR}/sandbox_cube.bmp")
        set_tests_properties(sandbox.draws_and_captures_on_d3d12 PROPERTIES
            PASS_REGULAR_EXPRESSION "saved")
    endif()
endif()
