cmake_minimum_required(VERSION 3.20)
project(OpenHDR LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "OpenHDR currently supports x64 only (configure with -A x64)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the core DLL is loaded into games and must not depend on a redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# ---- HLSL compiler (fxc from the Windows SDK) -------------------------------
file(GLOB _fxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
list(SORT _fxc_candidates)
list(REVERSE _fxc_candidates)
if(_fxc_candidates)
    list(GET _fxc_candidates 0 FXC)  # newest SDK
else()
    find_program(FXC NAMES fxc REQUIRED)
endif()
message(STATUS "fxc: ${FXC}")

set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")

# compile_shader(<out list var> <hlsl> <profile> <entry> <C array name>)
function(compile_shader OUT_VAR SRC PROFILE ENTRY VARNAME)
    set(out "${GEN_DIR}/${VARNAME}.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${FXC}" /nologo /O3 /T ${PROFILE} /E ${ENTRY} /Vn ${VARNAME} /Fh "${out}" "${SRC}"
        DEPENDS "${SRC}"
        COMMENT "fxc ${ENTRY} -> ${VARNAME}.h"
        VERBATIM)
    set(${OUT_VAR} ${${OUT_VAR}} "${out}" PARENT_SCOPE)
endfunction()

# ---- Shaders, shared by the injected and capture modes ----------------------
set(ITM_HLSL "${CMAKE_SOURCE_DIR}/shaders/itm.hlsl")
compile_shader(ITM_SHADERS "${ITM_HLSL}" vs_4_0 VSMain g_ItmVS)
compile_shader(ITM_SHADERS "${ITM_HLSL}" ps_4_0 PSMain g_ItmPS)
compile_shader(ITM_SHADERS "${ITM_HLSL}" vs_5_0 VSMain g_ItmVS50)  # D3D12
compile_shader(ITM_SHADERS "${ITM_HLSL}" ps_5_0 PSMain g_ItmPS50)
set(SCENE_HLSL "${CMAKE_SOURCE_DIR}/shaders/scene.hlsl")
compile_shader(ITM_SHADERS "${SCENE_HLSL}" vs_4_0 VSMain g_SceneVS)
compile_shader(ITM_SHADERS "${SCENE_HLSL}" ps_4_0 PSMain g_ScenePS)
compile_shader(ITM_SHADERS "${SCENE_HLSL}" vs_5_0 VSMain g_SceneVS50)  # D3D12
compile_shader(ITM_SHADERS "${SCENE_HLSL}" ps_5_0 PSMain g_ScenePS50)
add_custom_target(openhdr_shaders DEPENDS ${ITM_SHADERS})

# ---- Shared code: settings, settings panel, conversion parameters -------------
add_library(openhdr_shared STATIC
    src/common.h
    src/common.cpp
    src/config.h
    src/config.cpp
    src/display.h
    src/display.cpp
    src/frame_dump.h
    src/frame_dump.cpp
    src/hotkeys.h
    src/hotkeys.cpp
    src/itm_params.h
    src/itm_params.cpp
    src/osd.h
    src/osd.cpp
    src/scene.h
    src/scene.cpp)
target_include_directories(openhdr_shared PUBLIC "${CMAKE_SOURCE_DIR}/src" "${GEN_DIR}")
target_compile_definitions(openhdr_shared PUBLIC UNICODE _UNICODE)
target_compile_options(openhdr_shared PRIVATE /W4 /permissive- /utf-8)
add_dependencies(openhdr_shared openhdr_shaders)  # scene.cpp embeds the analysis shaders

# ---- OpenHDR64.dll: injected mode (hooks, swap chain wrapper, presenter) --------
add_library(openhdr_core SHARED
    src/crash_log.h
    src/crash_log.cpp
    src/dllmain.cpp
    src/dx11_presenter.h
    src/dx11_presenter.cpp
    src/dx12_presenter.h
    src/dx12_presenter.cpp
    src/presenter.h
    src/presenter.cpp
    src/hooks.h
    src/hooks.cpp
    src/inject.cpp
    src/swapchain.h
    src/swapchain.cpp)
set_target_properties(openhdr_core PROPERTIES OUTPUT_NAME OpenHDR64)
target_compile_options(openhdr_core PRIVATE /W4 /permissive- /utf-8)
# No dxgi.lib/d3d11.lib: the DLL is loaded into every GUI process by the global hook, so it must
# not drag D3D in. It loads dxgi.dll itself, only inside games it is active in.
target_link_libraries(openhdr_core PRIVATE openhdr_shared user32 gdi32)
add_dependencies(openhdr_core openhdr_shaders)

# ---- OpenHDR.exe: tray app, hook host, capture mode ---------------------------
add_executable(openhdr_host WIN32 src/host/main.cpp src/host/host.rc src/host/flyout.h src/host/flyout.cpp src/host/library.h src/host/library.cpp src/host/main_window.h src/host/main_window.cpp src/ui/ui.h src/ui/ui.cpp src/capture/capture.h src/capture/capture.cpp)
set_target_properties(openhdr_host PROPERTIES OUTPUT_NAME OpenHDR)
target_compile_options(openhdr_host PRIVATE /W4 /permissive- /utf-8 /bigobj)
target_link_libraries(openhdr_host PRIVATE openhdr_shared user32 gdi32 shell32 comdlg32 d3d11 dxgi dcomp dwmapi runtimeobject d2d1 dwrite windowscodecs ole32 version avrt)
add_dependencies(openhdr_host openhdr_core openhdr_shaders)
add_custom_command(TARGET openhdr_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/OpenHDR.ini" "$<TARGET_FILE_DIR:openhdr_host>/OpenHDR.ini")

# ---- SDR test application ---------------------------------------------------
set(PATTERN_HLSL "${CMAKE_SOURCE_DIR}/test/pattern.hlsl")
compile_shader(PATTERN_SHADERS "${PATTERN_HLSL}" vs_4_0 VSMain g_PatternVS)
compile_shader(PATTERN_SHADERS "${PATTERN_HLSL}" ps_4_0 PSMain g_PatternPS)

add_executable(sdr_test test/sdr_test.cpp ${PATTERN_SHADERS})
target_include_directories(sdr_test PRIVATE "${GEN_DIR}")
target_compile_definitions(sdr_test PRIVATE UNICODE _UNICODE)
target_compile_options(sdr_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(sdr_test PRIVATE d3d11 dxgi user32)


compile_shader(PATTERN12_SHADERS "${PATTERN_HLSL}" vs_5_0 VSMain g_PatternVS50)
compile_shader(PATTERN12_SHADERS "${PATTERN_HLSL}" ps_5_0 PSMain g_PatternPS50)
add_executable(sdr_test12 test/sdr_test12.cpp ${PATTERN12_SHADERS})
target_include_directories(sdr_test12 PRIVATE "${GEN_DIR}")
target_compile_definitions(sdr_test12 PRIVATE UNICODE _UNICODE)
target_compile_options(sdr_test12 PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(sdr_test12 PRIVATE d3d12 dxgi user32)


# ---- OpenHDR-Setup.exe: installer with the app embedded ----------------------
# The built app files are copied into generated/payload and embedded as RCDATA resources.
set(PAYLOAD_DIR "${GEN_DIR}/payload")
set(PAYLOAD_FILES "${PAYLOAD_DIR}/OpenHDR.exe" "${PAYLOAD_DIR}/OpenHDR64.dll" "${PAYLOAD_DIR}/OpenHDR.ini" "${PAYLOAD_DIR}/openhdr.ico")
add_custom_command(
    OUTPUT ${PAYLOAD_FILES}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PAYLOAD_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:openhdr_host>" "${PAYLOAD_DIR}/OpenHDR.exe"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:openhdr_core>" "${PAYLOAD_DIR}/OpenHDR64.dll"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/OpenHDR.ini" "${PAYLOAD_DIR}/OpenHDR.ini"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/assets/openhdr.ico" "${PAYLOAD_DIR}/openhdr.ico"
    DEPENDS openhdr_host openhdr_core "$<TARGET_FILE:openhdr_host>" "$<TARGET_FILE:openhdr_core>" "${CMAKE_SOURCE_DIR}/OpenHDR.ini" "${CMAKE_SOURCE_DIR}/assets/openhdr.ico"
    COMMENT "Collecting installer payload"
    VERBATIM)
file(WRITE "${GEN_DIR}/setup_payload.rc"
    "1 ICON \"payload/openhdr.ico\"\n101 RCDATA \"payload/OpenHDR.exe\"\n102 RCDATA \"payload/OpenHDR64.dll\"\n103 RCDATA \"payload/OpenHDR.ini\"\n")
set_source_files_properties("${GEN_DIR}/setup_payload.rc" PROPERTIES OBJECT_DEPENDS "${PAYLOAD_FILES}")

add_executable(openhdr_setup WIN32 src/setup/setup.cpp src/ui/ui.h src/ui/ui.cpp "${GEN_DIR}/setup_payload.rc" ${PAYLOAD_FILES})
set_target_properties(openhdr_setup PROPERTIES OUTPUT_NAME OpenHDR-Setup)
target_include_directories(openhdr_setup PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(openhdr_setup PRIVATE UNICODE _UNICODE)
target_compile_options(openhdr_setup PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)
target_link_libraries(openhdr_setup PRIVATE user32 gdi32 shell32 ole32 dwmapi d2d1 dwrite windowscodecs advapi32)
add_dependencies(openhdr_setup openhdr_host openhdr_core)
