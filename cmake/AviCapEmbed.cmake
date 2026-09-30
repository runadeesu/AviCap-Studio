# avicap_embed_files(<target> <namespace> <file>...)
# Generates a source file that embeds the given files as byte arrays and exposes
#   std::string_view <namespace>::find(std::string_view name)
# where name is the file name (without directory). Used for HLSL kernels, fonts
# and other resources that must ship inside the executable.
function(avicap_embed_files target ns)
    set(out_cpp "${CMAKE_CURRENT_BINARY_DIR}/${target}_embedded.cpp")
    set(files ${ARGN})
    set(abs_files "")
    foreach(f IN LISTS files)
        get_filename_component(af "${f}" ABSOLUTE)
        list(APPEND abs_files "${af}")
    endforeach()
    add_custom_command(
        OUTPUT "${out_cpp}"
        COMMAND ${CMAKE_COMMAND} -DOUT=${out_cpp} -DNS=${ns} "-DFILES=${abs_files}"
                -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFilesScript.cmake"
        DEPENDS ${abs_files} "${PROJECT_SOURCE_DIR}/cmake/EmbedFilesScript.cmake"
        COMMENT "Embedding resources for ${target}"
        VERBATIM)
    target_sources(${target} PRIVATE "${out_cpp}")
endfunction()
