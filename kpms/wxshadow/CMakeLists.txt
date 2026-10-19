set(WXSHADOW_LINUX_ADAPTER_OBJECT "" CACHE FILEPATH "Runtime-compatible Linux VM adapter .o built by Kbuild")
if(WXSHADOW_LINUX_ADAPTER_OBJECT)
    set(WXSHADOW_PORTABLE_DEFAULT ON)
else()
    set(WXSHADOW_PORTABLE_DEFAULT OFF)
endif()
option(WXSHADOW_PORTABLE "Build the portable transaction-only Dobby bridge" ${WXSHADOW_PORTABLE_DEFAULT})
if(WXSHADOW_PORTABLE)
    if(NOT WXSHADOW_LINUX_ADAPTER_OBJECT)
        message(FATAL_ERROR "WXSHADOW_PORTABLE requires WXSHADOW_LINUX_ADAPTER_OBJECT")
    endif()
    add_kpm_module(wxshadow wxshadow_portable.c wxshadow_runtime.c)
else()
    add_kpm_module(wxshadow
        wxshadow.c
        wxshadow_pgtable.c
        wxshadow_handlers.c
        wxshadow_bp.c
        wxshadow_scan.c
        wxshadow_protocol.c
        wxshadow_runtime.c
    )
endif()
target_include_directories(wxshadow_obj PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared")
target_compile_options(wxshadow_obj PRIVATE -mgeneral-regs-only -ffixed-x18)
option(WXSHADOW_DEBUG_PROTOCOL "Log protocol diagnostics for device bring-up" OFF)
if(WXSHADOW_DEBUG_PROTOCOL)
    target_compile_definitions(wxshadow_obj PRIVATE WXSHADOW_DEBUG_PROTOCOL=1)
endif()
if(WXSHADOW_LINUX_ADAPTER_OBJECT)
    if(NOT EXISTS "${WXSHADOW_LINUX_ADAPTER_OBJECT}")
        message(FATAL_ERROR "WXSHADOW_LINUX_ADAPTER_OBJECT does not exist")
    endif()
    target_compile_definitions(wxshadow_obj PRIVATE WXSHADOW_NATIVE_ADAPTER=1)
    # Link the separately compiled Linux VM implementation into the KPM.
    add_custom_command(TARGET wxshadow.kpm POST_BUILD
        COMMAND ${CMAKE_C_COMPILER} -r -o wxshadow-combined.kpm wxshadow.kpm "${WXSHADOW_LINUX_ADAPTER_OBJECT}"
        COMMAND ${CMAKE_COMMAND} -E rename wxshadow-combined.kpm wxshadow.kpm
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# Build client tool (userspace)
add_executable(wxshadow_client wxshadow_client.c)
target_link_options(wxshadow_client PRIVATE -static -s)
add_executable(wxshadow_protocol_client wxshadow_protocol_client.c)
target_include_directories(wxshadow_protocol_client PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared")
target_link_options(wxshadow_protocol_client PRIVATE -static -s)
