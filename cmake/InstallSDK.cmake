include(CMakePackageConfigHelpers)

install(TARGETS badger badger_static EXPORT TenaciousBadgerTargets
    RUNTIME DESTINATION bin LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/badger" DESTINATION include)
install(FILES "${PROJECT_BINARY_DIR}/generated/badger/api.h"
    "${PROJECT_BINARY_DIR}/generated/badger/version.h" DESTINATION include/badger)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/NOTICE"
    DESTINATION .)
install(EXPORT TenaciousBadgerTargets NAMESPACE TenaciousBadger::
    DESTINATION lib/cmake/TenaciousBadger)
configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/TenaciousBadgerConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/TenaciousBadgerConfig.cmake"
    INSTALL_DESTINATION lib/cmake/TenaciousBadger)
write_basic_package_version_file("${PROJECT_BINARY_DIR}/TenaciousBadgerConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion)
install(FILES "${PROJECT_BINARY_DIR}/TenaciousBadgerConfig.cmake"
    "${PROJECT_BINARY_DIR}/TenaciousBadgerConfigVersion.cmake"
    DESTINATION lib/cmake/TenaciousBadger)

if(MSVC)
    set_target_properties(badger_static PROPERTIES
        COMPILE_PDB_NAME badger_static
        COMPILE_PDB_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib"
        COMPILE_PDB_OUTPUT_DIRECTORY_DEBUG "${PROJECT_BINARY_DIR}/lib")
    install(FILES "$<TARGET_PDB_FILE:badger>"
        "${PROJECT_BINARY_DIR}/lib/badger_static.pdb"
        DESTINATION lib CONFIGURATIONS Debug OPTIONAL)
endif()
