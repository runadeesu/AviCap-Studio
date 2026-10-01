set(_platform_libs "")
if(WIN32)
    set(_platform_libs shell32 ole32 psapi winhttp crypt32)
endif()

avicap_add_module(core
    SOURCES
        time.cpp
        log.cpp
        strings.cpp
        platform.cpp
        jobs.cpp
        ids.cpp
        file_io.cpp
        settings.cpp
        i18n.cpp
        process.cpp
        net.cpp
    DEPS
        AviCap::json
        ${_platform_libs})
