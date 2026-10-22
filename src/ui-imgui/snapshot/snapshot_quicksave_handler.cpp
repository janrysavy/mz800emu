/**
 * @file snapshot_quicksave_handler.cpp
 * @brief Zpracování Quick Save/Load požadavků z menu
 *
 * Požadavek z menu (nebo klávesy) jen zařadí do fronty snapshotů; uloží
 * nebo načte ho emu vlákno v pauze. Oznámení a obnovení běhu dělá
 * imgui_snapshot_request_results() (snapshot_request_ui.h).
 */

#include "main.h"
#include <stdio.h>
#include <glib.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/bootstrap/myimgui.h"

#include "emulator.h"
#include "ui-imgui/snapshot/snapshot_request_ui.h"

/* Deklarace z menu_snapshot.cpp */
extern "C" bool snapshot_ui_is_quicksave_requested(void);
extern "C" void snapshot_ui_clear_quicksave_request(void);
extern "C" bool snapshot_ui_is_quickload_requested(void);
extern "C" void snapshot_ui_clear_quickload_request(void);


extern "C" void imgui_snapshot_quicksave_handler(void)
{
    if (snapshot_ui_is_quicksave_requested()) {
        snapshot_ui_clear_quicksave_request();
        snapshot_ui_submit(SNAPSHOT_REQ_QUICKSAVE, NULL, NULL);
    }

    if (snapshot_ui_is_quickload_requested()) {
        snapshot_ui_clear_quickload_request();
        snapshot_ui_submit(SNAPSHOT_REQ_QUICKLOAD, NULL, NULL);
    }
}
