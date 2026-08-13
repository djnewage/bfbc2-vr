cmake_minimum_required(VERSION 3.20)
project(bfbc2vr LANGUAGES CXX)

# BFBC2Game.exe is 32-bit. A 64-bit build will not load into it.
# Configure with:  cmake -B build -A Win32
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "bfbc2vr must be built for x86 - BFBC2Game.exe is 32-bit.\n"
        "Configure with:  cmake -B build -A Win32")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(d3d9 SHARED
    src/dllmain.cpp
    src/logger.cpp
    src/constants.cpp
    src/camera_override.cpp
    src/shader_registry.cpp
    src/vr_tracking.cpp
    src/wrapper_d3d9.cpp
    src/wrapper_device.cpp
    src/d3d9.def
)

target_include_directories(d3d9 PRIVATE src thirdparty/openvr)

# The output must be named exactly d3d9.dll so the game's LoadLibrary finds it.
set_target_properties(d3d9 PROPERTIES
    OUTPUT_NAME "d3d9"
    PREFIX ""
)

if(MSVC)
    target_compile_options(d3d9 PRIVATE /W4 /permissive-)
    # No CRT dependency surprises inside another process's address space.
    set_property(TARGET d3d9 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# dxguid.lib supplies the IID_IDirect3D9 / IID_IDirect3DDevice9 GUID data.
# Deliberately NOT linking d3d9.lib: this DLL *is* d3d9.dll, and importing from
# the real one would make it depend on itself. The backend is resolved at runtime
# by LoadLibrary instead - see dllmain.cpp.
target_link_libraries(d3d9 PRIVATE
    dxguid.lib
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/openvr/lib/win32/openvr_api.lib
)
