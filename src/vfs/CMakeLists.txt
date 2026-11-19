# The virtual file system: mounted directories and archives, one namespace.
set(TB_VFS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/vfs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/vfs_archive.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/vfs_directory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/vfs_index.c"
    CACHE INTERNAL "Virtual file system sources")
target_sources(badger PRIVATE ${TB_VFS_SOURCES})
target_sources(badger_static PRIVATE ${TB_VFS_SOURCES})
