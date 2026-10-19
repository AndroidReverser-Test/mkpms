set(WXUPROBE_LINUX_ADAPTER_OBJECT "" CACHE FILEPATH
    "wxuprobe_linux.o built with Linux 5.10-6.6 Kbuild (BTF target auto-adaptation)")
if(WXUPROBE_LINUX_ADAPTER_OBJECT)
    if(NOT EXISTS "${WXUPROBE_LINUX_ADAPTER_OBJECT}")
        message(FATAL_ERROR "WXUPROBE_LINUX_ADAPTER_OBJECT does not exist")
    endif()
    add_kpm_module(wxuprobe wxuprobe.c ../wxshadow/wxshadow_runtime.c)
    target_include_directories(wxuprobe_obj PRIVATE ../../shared)
    target_compile_options(wxuprobe_obj PRIVATE -mgeneral-regs-only -ffixed-x18)
    add_custom_command(TARGET wxuprobe.kpm POST_BUILD
        COMMAND ${CMAKE_C_COMPILER} -r -o wxuprobe-combined.kpm wxuprobe.kpm
            "${WXUPROBE_LINUX_ADAPTER_OBJECT}"
        COMMAND ${CMAKE_COMMAND} -E rename wxuprobe-combined.kpm wxuprobe.kpm
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
add_executable(wxuprobe_client wxuprobe_client.c)
target_include_directories(wxuprobe_client PRIVATE ../../shared)
target_link_options(wxuprobe_client PRIVATE -static -s)
