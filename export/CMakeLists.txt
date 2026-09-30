avicap_add_module(export
    SOURCES export_settings.cpp exporter.cpp verify.cpp
    DEPS AviCap::render AviCap::audio AviCap::encode)
