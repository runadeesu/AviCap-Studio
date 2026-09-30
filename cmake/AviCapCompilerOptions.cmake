# Common compiler configuration and the module helper used by every engine library.

add_library(avicap_options INTERFACE)
target_compile_features(avicap_options INTERFACE cxx_std_20)
target_include_directories(avicap_options INTERFACE
    "${PROJECT_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/generated")

if(WIN32)
    target_compile_definitions(avicap_options INTERFACE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

if(MSVC)
    target_compile_options(avicap_options INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /MP /EHsc
        /wd4100   # unreferenced formal parameter (interfaces)
        /wd4324   # structure padded due to alignment specifier
    )
    if(AVICAP_WERROR)
        target_compile_options(avicap_options INTERFACE /WX)
    endif()
else()
    target_compile_options(avicap_options INTERFACE
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    if(AVICAP_WERROR)
        target_compile_options(avicap_options INTERFACE -Werror)
    endif()
    if(AVICAP_SANITIZE AND NOT MINGW)
        string(REPLACE ";" "," _san "${AVICAP_SANITIZE}")
        target_compile_options(avicap_options INTERFACE -fsanitize=${_san} -fno-omit-frame-pointer)
        target_link_options(avicap_options INTERFACE -fsanitize=${_san})
    endif()
endif()

if(MINGW)
    # Ship a self-contained exe: no libstdc++/libgcc/winpthread DLLs next to it.
    target_link_options(avicap_options INTERFACE -static-libgcc -static-libstdc++
        -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic)
endif()

target_link_libraries(avicap_options INTERFACE Threads::Threads)

# avicap_add_module(<name> SOURCES ... [DEPS ...] [PRIVATE_DEPS ...])
# Creates static library avicap_<name> and alias AviCap::<name>.
function(avicap_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
    set(target avicap_${name})
    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(AviCap::${name} ALIAS ${target})
    target_link_libraries(${target} PUBLIC avicap_options ${ARG_DEPS})
    if(ARG_PRIVATE_DEPS)
        target_link_libraries(${target} PRIVATE ${ARG_PRIVATE_DEPS})
    endif()
    set_target_properties(${target} PROPERTIES FOLDER "engine")
    set_property(GLOBAL APPEND PROPERTY AVICAP_MODULE_TARGETS ${target})
endfunction()
