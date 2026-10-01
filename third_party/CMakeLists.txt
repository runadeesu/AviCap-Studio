# Vendored third-party code. Licenses: see THIRD_PARTY_LICENSES.md.

add_library(avicap_json INTERFACE)
target_include_directories(avicap_json SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(AviCap::json ALIAS avicap_json)

add_library(avicap_doctest INTERFACE)
target_include_directories(avicap_doctest SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/doctest")
add_library(AviCap::doctest ALIAS avicap_doctest)

add_library(avicap_stb STATIC stb_impl.cpp)
target_include_directories(avicap_stb SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/stb")
add_library(AviCap::stb ALIAS avicap_stb)

# Dear ImGui core builds everywhere (the UI layer is unit-tested headless on
# Linux); the Win32 / Direct3D 11 backends only on Windows.
if(AVICAP_BUILD_UI)
    add_library(avicap_imgui STATIC
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/imgui_demo.cpp)
    target_include_directories(avicap_imgui SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/imgui")
    target_compile_definitions(avicap_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_USE_WCHAR32 IMGUI_DEFINE_MATH_OPERATORS)
    add_library(AviCap::imgui ALIAS avicap_imgui)
    if(WIN32)
        add_library(avicap_imgui_win32 STATIC
            imgui/backends/imgui_impl_win32.cpp
            imgui/backends/imgui_impl_dx11.cpp)
        target_include_directories(avicap_imgui_win32 SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends")
        target_compile_definitions(avicap_imgui_win32 PRIVATE UNICODE _UNICODE)
        target_link_libraries(avicap_imgui_win32 PUBLIC avicap_imgui d3d11 dxgi d3dcompiler dwmapi imm32)
        add_library(AviCap::imgui_win32 ALIAS avicap_imgui_win32)
    endif()
endif()
