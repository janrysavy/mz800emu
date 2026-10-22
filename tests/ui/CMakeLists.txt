# UI testy - smoke test infrastruktury (SDL3 + ImGui + OpenGL inicializace).
# Hodnota nízká (testuje vendored knihovny), ale rychlé a užitečné při ImGui
# upgrade - detekuje rozbitý backend.

mz_add_test(ui-smoke
    TYPE     UI
    SOURCES  test_ui_smoke.cpp
    LIBS     mzlib_imgui mzlib_igfd mzlib_imwidgets
    LABELS   "ui"
)

# Memory Browser V0 hex MVP - membrowser mutant
add_subdirectory(membrowser)

# Production upload/format/alpha checks use dummy software rendering only.
# Unlike ui-smoke this target never creates an OpenGL context.
mz_add_test(video-textures
    TYPE STANDALONE
    SOURCES test_video_textures.c
            ${CMAKE_SOURCE_DIR}/src/iface-video/sdl3/iface_video_sdl3.c
    LIBS mz::sdl3 mz::glib mz::opengl
    LABELS "standalone;ui;software"
)

# Final ImGui pixels at small/large/4K sizes, resize-back and debugger overlay.
# This target uses only the hidden SDL software renderer, never OpenGL.
mz_add_test(sdl-geometry
    TYPE STANDALONE
    SOURCES test_sdl_geometry.cpp
    LIBS mzlib_imgui
    LABELS "standalone;ui;software"
)

# Funkční testy UI logiky - video záznam (menu, dialog nastavení, REC indikátor,
# notifikace událostí, okno dálkového ovládání a tabulka zkratek), okno Replay
# Control (zkratky, vykreslení bez záznamu, při záznamu, rewindu a přehrávání)
# a okna Screenshot Control a Audio Capture Control (zkratky, vykreslení).
# Linkuje UI zdroje přímo; notifikace stubuje test.
mz_add_test(ui-functional
    TYPE     UI
    SOURCES  test_ui_functional.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/videorec/videorec_menu.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/videorec/videorec_setup_dialog.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/videorec/videorec_remote_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/topmenu/menu_replay.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/topmenu/replay_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/screenshot/screenshot_ui.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/audiocap/audio_capture_ui.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/topmenu/menu_snapshot.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/snapshot/snapshot_quicksave_handler.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/snapshot/snapshot_request_ui.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/filechooser/igfd_ToggleButton.cpp
    LIBS     mzlib_imgui mzlib_igfd mzlib_imwidgets
    LABELS   "ui;videorec"
)

# Hidden native platform windows; explicit SDL software, never OpenGL.
if(WIN32)
    mz_add_test(sdl-border TYPE STANDALONE SOURCES test_sdl_border.cpp
        LIBS mzlib_imgui LABELS "standalone;ui;software;native-windows")
    target_compile_definitions(test_sdl-border PRIVATE MZARCH=800)
    set_tests_properties(sdl-border PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=windows;SDL_RENDER_DRIVER=software")
    mz_add_test(sdl-viewports TYPE STANDALONE SOURCES test_sdl_viewports.cpp
        LIBS mzlib_imgui LABELS "standalone;ui;software;native-windows")
    # SDL dummy has no native viewports; all Windows remain explicitly hidden.
    set_tests_properties(sdl-viewports PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=windows;SDL_RENDER_DRIVER=software")
    mz_add_test(sdl-workspace TYPE STANDALONE SOURCES test_sdl_workspace.cpp
        ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger_workspace.cpp
        LIBS mzlib_imgui mz::glib LABELS "standalone;ui;software;native-windows")
    set_tests_properties(sdl-workspace PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=windows;SDL_RENDER_DRIVER=software")
endif()
