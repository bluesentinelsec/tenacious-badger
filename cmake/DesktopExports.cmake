configure_file("${PROJECT_SOURCE_DIR}/resources/desktop-metadata.json.in"
    "${PROJECT_BINARY_DIR}/desktop-metadata.json" @ONLY)

if(WIN32)
    foreach(target badger burrow)
        if(NOT TARGET ${target})
            continue()
        endif()
        if(target STREQUAL "badger")
            set(TB_RESOURCE_DESCRIPTION "Tenacious Badger Engine")
            set(TB_RESOURCE_FILENAME "badger.dll")
            set(TB_RESOURCE_TYPE VFT_DLL)
        else()
            set(TB_RESOURCE_DESCRIPTION "Burrow")
            set(TB_RESOURCE_FILENAME "burrow.exe")
            set(TB_RESOURCE_TYPE VFT_APP)
        endif()
        configure_file("${PROJECT_SOURCE_DIR}/resources/windows/version.rc.in"
            "${PROJECT_BINARY_DIR}/generated/${target}.rc" @ONLY)
        target_sources(${target} PRIVATE "${PROJECT_BINARY_DIR}/generated/${target}.rc")
    endforeach()
elseif(APPLE AND TARGET burrow)
    set_target_properties(burrow PROPERTIES
        MACOSX_BUNDLE TRUE
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks")
    configure_file("${PROJECT_SOURCE_DIR}/resources/macos/Info.plist.in"
        "${PROJECT_BINARY_DIR}/generated/Info.plist" @ONLY)
    set_target_properties(burrow PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${PROJECT_BINARY_DIR}/generated/Info.plist")
    target_sources(burrow PRIVATE "${PROJECT_SOURCE_DIR}/resources/icons/burrow.icns")
    set_source_files_properties("${PROJECT_SOURCE_DIR}/resources/icons/burrow.icns"
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set(library_directory "${PROJECT_BINARY_DIR}/bin/burrow.app/Contents/Frameworks")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(TARGET burrow)
    set_target_properties(burrow PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN/lib")
    endif()
    set(library_directory "${PROJECT_BINARY_DIR}/bin/lib")
    configure_file("${PROJECT_SOURCE_DIR}/resources/linux/burrow.desktop.in"
        "${PROJECT_BINARY_DIR}/generated/${TB_APP_ID}.desktop" @ONLY)
    configure_file("${PROJECT_SOURCE_DIR}/resources/linux/burrow.metainfo.xml.in"
        "${PROJECT_BINARY_DIR}/generated/${TB_APP_ID}.metainfo.xml" @ONLY)
endif()

if(DEFINED library_directory)
    set_target_properties(badger PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${library_directory}")
    foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(badger PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${config} "${library_directory}")
    endforeach()
endif()
