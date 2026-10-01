cmake_minimum_required(VERSION 3.25)

# Version is the single source of truth for the application (Semantic Versioning).
project(AviCapStudio
    VERSION 0.1.0
    DESCRIPTION "AviCap Studio - professional non-linear video editor for Windows"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(WIN32)
    set(AVICAP_DEFAULT_APP ON)
else()
    set(AVICAP_DEFAULT_APP OFF)
endif()

option(AVICAP_BUILD_TESTS "Build unit and integration tests" ON)
option(AVICAP_BUILD_APP "Build the AviCap Studio desktop application (Windows only)" ${AVICAP_DEFAULT_APP})
option(AVICAP_BUILD_CLI "Build avicap-cli (headless export / probe / worker process)" ON)
option(AVICAP_BUILD_UI "Build the UI layer (Dear ImGui); headless-testable on every platform" ON)
option(AVICAP_WERROR "Treat compiler warnings as errors" OFF)
set(AVICAP_SANITIZE "" CACHE STRING "Sanitizers for non-MSVC debug builds, e.g. address;undefined")

if(AVICAP_BUILD_APP AND NOT WIN32)
    message(FATAL_ERROR "The desktop application targets Windows. Use -DAVICAP_BUILD_APP=OFF on this platform.")
endif()
if(AVICAP_BUILD_APP AND NOT AVICAP_BUILD_UI)
    message(FATAL_ERROR "AVICAP_BUILD_APP requires AVICAP_BUILD_UI.")
endif()

include(AviCapCompilerOptions)
include(AviCapEmbed)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ------------------------------------------------------------------ dependencies
find_package(Threads REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil swscale swresample avfilter)

add_subdirectory(third_party)

# ------------------------------------------------------------------ build info
string(TIMESTAMP AVICAP_BUILD_DATE "%Y-%m-%d" UTC)
configure_file(cmake/BuildInfo.h.in "${CMAKE_BINARY_DIR}/generated/avicap_build_info.h" @ONLY)

# ------------------------------------------------------------------ engine modules
# Dependency direction (lower never includes higher):
#   core -> storage -> timeline -> project
#   core -> media/decode/encode -> render/effects/text -> audio -> export
#   ai/sounds/subtitles/proxy/cache build on the above; ui/apps sit on top.
set(AVICAP_ENGINE_MODULES
    core storage timeline project media decode encode cache text effects render
    audio subtitles proxy export ai sounds integrations plugins diagnostics)
foreach(_module IN LISTS AVICAP_ENGINE_MODULES)
    add_subdirectory(${_module})
endforeach()

if(AVICAP_BUILD_UI)
    add_subdirectory(ui)
endif()

add_subdirectory(apps)

if(AVICAP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/cmake/AviCapPackaging.cmake")
    include(AviCapPackaging)
endif()
