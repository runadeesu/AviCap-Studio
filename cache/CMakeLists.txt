avicap_add_module(cache
    SOURCES cache.cpp
    DEPS AviCap::audio AviCap::decode AviCap::timeline AviCap::stb)
