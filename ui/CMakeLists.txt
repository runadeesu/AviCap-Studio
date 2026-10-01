# UI layer (Dear ImGui). Everything except the Win32 platform layer builds on
# every platform so the editor can be driven and tested headless.
set(AVICAP_UI_SOURCES
    app.cpp
    app_commands.cpp
    assets.cpp
    color_panel.cpp
    commands.cpp
    diagnostics_panel.cpp
    edit_helpers.cpp
    effects_panel.cpp
    export_panel.cpp
    headless.cpp
    history_panel.cpp
    inspector_panel.cpp
    main_window.cpp
    media_panel.cpp
    mixer_panel.cpp
    preview.cpp
    scopes_panel.cpp
    settings_window.cpp
    theme.cpp
    timeline_panel.cpp
    viewer_panel.cpp
    widgets.cpp)

if(WIN32)
    list(APPEND AVICAP_UI_SOURCES dialogs_win32.cpp platform_win32.cpp)
endif()

avicap_add_module(ui
    SOURCES ${AVICAP_UI_SOURCES}
    DEPS AviCap::imgui AviCap::cache AviCap::export AviCap::proxy AviCap::project AviCap::audio
         AviCap::render AviCap::effects AviCap::text AviCap::media AviCap::timeline AviCap::core)

if(WIN32)
    target_link_libraries(avicap_ui PUBLIC AviCap::imgui_win32 AviCap::stb ole32 shell32 uuid)
endif()
