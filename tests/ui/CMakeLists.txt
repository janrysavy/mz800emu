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

# Funkční testy UI logiky - video záznam (menu, dialog nastavení, REC indikátor,
# notifikace událostí, okno dálkového ovládání a tabulka zkratek). Linkuje UI
# zdroje přímo; notifikace stubuje test.
mz_add_test(ui-functional
    TYPE     UI
    SOURCES  test_ui_functional.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/videorec/videorec_menu.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/videorec/videorec_setup_dialog.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/videorec/videorec_remote_window.cpp
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/filechooser/igfd_ToggleButton.cpp
    LIBS     mzlib_imgui mzlib_igfd mzlib_imwidgets
    LABELS   "ui;videorec"
)
