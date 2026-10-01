# AviCapStudio.exe - the Windows desktop application.
set(AVICAP_RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
configure_file(resources/studio.rc.in "${CMAKE_CURRENT_BINARY_DIR}/studio.rc" @ONLY)

add_executable(AviCapStudio WIN32
    main.cpp
    self_test.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/studio.rc")
target_link_libraries(AviCapStudio PRIVATE avicap_options AviCap::ui AviCap::diagnostics AviCap::json)
set_target_properties(AviCapStudio PROPERTIES OUTPUT_NAME "AviCapStudio")
if(MINGW)
    target_link_options(AviCapStudio PRIVATE -municode)
elseif(MSVC)
    # The manifest comes from studio.rc.
    target_link_options(AviCapStudio PRIVATE /MANIFEST:NO)
endif()
