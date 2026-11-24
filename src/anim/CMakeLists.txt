# Skeletal animation on the CPU: skeletons, clips, sampling and blending.
set(TB_ANIM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/anim.c"
    CACHE INTERNAL "Animation sources")
target_sources(badger PRIVATE ${TB_ANIM_SOURCES})
target_sources(badger_static PRIVATE ${TB_ANIM_SOURCES})
