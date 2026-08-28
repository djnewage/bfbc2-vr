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
    src/draw_policy.cpp
    src/draw_diag.cpp
    src/console.cpp
    src/memscan.cpp
    src/os_input.cpp
    src/input_bus.cpp
    src/dinput8_proxy.cpp
    src/aim_policy.cpp
    src/vrinput.cpp
    src/shader_registry.cpp
    src/vr_tracking.cpp
    src/vr_compositor.cpp
    src/wrapper_d3d9.cpp
    src/wrapper_device.cpp
    src/d3d9.def
)

target_include_directories(d3d9 PRIVATE
    src
    thirdparty/openvr
    thirdparty/dxvk       # vendored d3d9_interfaces.h (interop, dxvk v3.0.2)
    thirdparty/vulkan     # vendored Vulkan core headers (types only, no loader)
)

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
    dinput8.lib
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/openvr/lib/win32/openvr_api.lib
)

# ---------------------------------------------------------------------------
# Deterministic tests for the pure policy modules. No D3D, no VR runtime - a
# console exe that exits 0. Run with:  ctest --test-dir build -C Release
option(BFBC2VR_BUILD_TESTS "Build the draw_policy unit tests" ON)
if(BFBC2VR_BUILD_TESTS)
    enable_testing()
    add_executable(draw_policy_tests tests/draw_policy_test.cpp src/draw_policy.cpp)
    target_include_directories(draw_policy_tests PRIVATE src)
    if(MSVC)
        target_compile_options(draw_policy_tests PRIVATE /W4 /permissive-)
        set_property(TARGET draw_policy_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME draw_policy COMMAND draw_policy_tests)

    add_executable(aim_policy_tests tests/aim_policy_test.cpp src/aim_policy.cpp)
    target_include_directories(aim_policy_tests PRIVATE src)
    if(MSVC)
        target_compile_options(aim_policy_tests PRIVATE /W4 /permissive-)
        set_property(TARGET aim_policy_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME aim_policy COMMAND aim_policy_tests)
endif()
