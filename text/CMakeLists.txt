set(_text_sources stb_rasterizer.cpp)
set(_text_libs "")
if(WIN32)
    list(APPEND _text_sources directwrite_rasterizer.cpp)
    set(_text_libs dwrite d2d1 windowscodecs ole32)
endif()
avicap_add_module(text
    SOURCES ${_text_sources}
    DEPS AviCap::timeline AviCap::stb ${_text_libs})
