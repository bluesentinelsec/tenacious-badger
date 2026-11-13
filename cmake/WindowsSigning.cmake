include_guard(GLOBAL)

option(TB_WINDOWS_SELF_SIGN "Self-sign project-owned Windows executables and DLLs" ${WIN32})
set(TB_WINDOWS_SIGNING_THUMBPRINT "" CACHE STRING
    "Existing CurrentUser/My code-signing certificate; empty creates/reuses the local identity")

function(tb_sign_windows_target target)
    if(NOT WIN32 OR NOT TB_WINDOWS_SELF_SIGN)
        return()
    endif()
    get_target_property(target_type ${target} TYPE)
    if(NOT target_type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
            "${TB_SIGNING_POWERSHELL}" -NoProfile -NonInteractive
            -ExecutionPolicy Bypass -File "${TB_SIGNING_SCRIPT}"
            -Action Sign -FilePath "$<TARGET_FILE:${target}>"
            -Publisher "${TB_SIGNING_PUBLISHER}"
            -CertificateThumbprint "${TB_SIGNING_ACTIVE_THUMBPRINT}"
        COMMENT "Self-signing Windows binary: ${target}"
        VERBATIM)
    set_property(GLOBAL APPEND PROPERTY TB_WINDOWS_SIGNED_TARGETS ${target})
endfunction()

function(tb_add_windows_signing_tests)
    if(NOT WIN32 OR NOT TB_WINDOWS_SELF_SIGN)
        return()
    endif()
    get_property(targets GLOBAL PROPERTY TB_WINDOWS_SIGNED_TARGETS)
    set(manifest "")
    foreach(target IN LISTS targets)
        string(APPEND manifest "$<TARGET_FILE:${target}>\n")
    endforeach()
    set(manifest_path "${TB_SIGNING_DIRECTORY}/targets-$<CONFIG>.txt")
    file(GENERATE OUTPUT "${manifest_path}" CONTENT "${manifest}")
    add_test(NAME windows.signatures
        COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
            "${TB_SIGNING_POWERSHELL}" -NoProfile -NonInteractive
            -ExecutionPolicy Bypass -File "${TB_SIGNING_SCRIPT}"
            -Action VerifyManifest -FilePath "${manifest_path}"
            -CertificateFile "${TB_SIGNING_DIRECTORY}/self-signing.cer")
    if(TARGET burrow)
        add_test(NAME windows.signing_contract
            COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                "${TB_SIGNING_POWERSHELL}" -NoProfile -NonInteractive
                -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/windows/Test-Signing.ps1"
                -BinaryPath "$<TARGET_FILE:burrow>"
                -BuildDirectory "${CMAKE_BINARY_DIR}"
                -SigningDirectory "${TB_SIGNING_DIRECTORY}"
                -Publisher "${TB_SIGNING_PUBLISHER}")
    endif()
endfunction()

if(NOT WIN32 OR NOT TB_WINDOWS_SELF_SIGN)
    return()
endif()

find_program(TB_SIGNING_POWERSHELL NAMES powershell.exe pwsh.exe REQUIRED)
set(TB_SIGNING_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../scripts/windows/Sign-Binary.ps1"
    CACHE INTERNAL "Windows self-signing script" FORCE)
set(TB_SIGNING_DIRECTORY "${CMAKE_BINARY_DIR}/signing"
    CACHE INTERNAL "Public Windows signing metadata" FORCE)
if(DEFINED TB_PUBLISHER)
    set(TB_SIGNING_PUBLISHER "${TB_PUBLISHER}" CACHE INTERNAL "Signing publisher" FORCE)
else()
    set(TB_SIGNING_PUBLISHER "bluesentinelsec" CACHE INTERNAL "Signing publisher" FORCE)
endif()

set(initialize_arguments -Action Initialize -OutputDirectory "${TB_SIGNING_DIRECTORY}"
    -Publisher "${TB_SIGNING_PUBLISHER}")
if(TB_WINDOWS_SIGNING_THUMBPRINT)
    list(APPEND initialize_arguments -CertificateThumbprint "${TB_WINDOWS_SIGNING_THUMBPRINT}")
endif()
# A parent PowerShell 7 session can otherwise pass incompatible module paths to
# Windows PowerShell 5.1. Let the child initialize its native module paths.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
    "${TB_SIGNING_POWERSHELL}" -NoProfile -NonInteractive
    -ExecutionPolicy Bypass -File "${TB_SIGNING_SCRIPT}" ${initialize_arguments}
    COMMAND_ERROR_IS_FATAL ANY)
file(STRINGS "${TB_SIGNING_DIRECTORY}/thumbprint.txt" active_thumbprint LIMIT_COUNT 1)
set(TB_SIGNING_ACTIVE_THUMBPRINT "${active_thumbprint}"
    CACHE INTERNAL "Active Windows self-signing identity" FORCE)
