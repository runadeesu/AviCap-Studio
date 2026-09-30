# FindFFmpeg: locates the FFmpeg libraries (libav*) and creates FFmpeg::<component>
# imported targets plus FFmpeg::FFmpeg (all requested components).
#
# Works with: system packages (pkg-config), the MinGW cross prefix produced by
# tools/deps/build_deps_mingw.sh, and vcpkg (MSVC) installations.

find_package(PkgConfig QUIET)

set(_ffmpeg_all_found TRUE)
foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(PC_FFmpeg_${_comp} QUIET lib${_comp})
    endif()
    find_path(FFmpeg_${_comp}_INCLUDE_DIR
        NAMES lib${_comp}/${_comp}.h lib${_comp}/version.h
        HINTS ${PC_FFmpeg_${_comp}_INCLUDE_DIRS} ${FFMPEG_ROOT}/include)
    find_library(FFmpeg_${_comp}_LIBRARY
        NAMES ${_comp} lib${_comp}
        HINTS ${PC_FFmpeg_${_comp}_LIBRARY_DIRS} ${FFMPEG_ROOT}/lib)
    if(FFmpeg_${_comp}_INCLUDE_DIR AND FFmpeg_${_comp}_LIBRARY)
        set(FFmpeg_${_comp}_FOUND TRUE)
        if(NOT TARGET FFmpeg::${_comp})
            add_library(FFmpeg::${_comp} UNKNOWN IMPORTED)
            set_target_properties(FFmpeg::${_comp} PROPERTIES
                IMPORTED_LOCATION "${FFmpeg_${_comp}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_${_comp}_INCLUDE_DIR}")
        endif()
        list(APPEND FFmpeg_LIBRARIES "${FFmpeg_${_comp}_LIBRARY}")
        list(APPEND FFmpeg_INCLUDE_DIRS "${FFmpeg_${_comp}_INCLUDE_DIR}")
    else()
        set(FFmpeg_${_comp}_FOUND FALSE)
        if(FFmpeg_FIND_REQUIRED_${_comp})
            set(_ffmpeg_all_found FALSE)
        endif()
    endif()
endforeach()

if(FFmpeg_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES FFmpeg_INCLUDE_DIRS)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
    REQUIRED_VARS FFmpeg_LIBRARIES FFmpeg_INCLUDE_DIRS
    HANDLE_COMPONENTS)

if(FFmpeg_FOUND AND NOT TARGET FFmpeg::FFmpeg)
    add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
    foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
        if(TARGET FFmpeg::${_comp})
            set_property(TARGET FFmpeg::FFmpeg APPEND PROPERTY INTERFACE_LINK_LIBRARIES FFmpeg::${_comp})
        endif()
    endforeach()
endif()

# Directory holding the runtime DLLs (used when packaging Windows builds).
if(WIN32 AND FFmpeg_avcodec_LIBRARY)
    get_filename_component(_libdir "${FFmpeg_avcodec_LIBRARY}" DIRECTORY)
    get_filename_component(_root "${_libdir}" DIRECTORY)
    set(FFmpeg_RUNTIME_DIR "${_root}/bin" CACHE PATH "Directory containing FFmpeg DLLs")
endif()
