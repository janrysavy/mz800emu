/**
 * @file snapshot_request_ui.cpp
 * @brief GUI strana požadavků na snapshot: zařazení s pauzou, oznámení
 *        výsledku a obnovení běhu (kontrakt v snapshot_request_ui.h).
 */

#include "main.h"
#include <stdio.h>

// Lokalizace
#include "i18n.h"

#include "emulator.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/snapshot/snapshot_request_ui.h"

extern "C"
{
#include "snapshot/snapshot.h"
}

/* Deklarace z menu_snapshot.cpp */
extern "C" bool snapshot_ui_was_running_before_pause(void);

/* Deklarace z snapshot_notification.cpp */
extern "C" void snapshot_notification_show(const char *message, bool is_error);


/**
 * @brief Obnoví běh po Load / Quick Load podle režimu obnovení.
 * @param mode SNAPSHOT_RESUME_ALWAYS_RUN / _ALWAYS_PAUSE / _PREVIOUS.
 */
static void snapshot_ui_resume_after_load(int mode)
{
    switch (mode) {
        case SNAPSHOT_RESUME_ALWAYS_RUN:
            dbg_ui_run();
            break;
        case SNAPSHOT_RESUME_ALWAYS_PAUSE:
            /* Nechat v pauze */
            break;
        case SNAPSHOT_RESUME_PREVIOUS:
        default:
            if (snapshot_ui_was_running_before_pause())
                dbg_ui_run();
            break;
    }
}


/**
 * @brief Ohlásí výsledek jednoho požadavku a obnoví běh.
 * @param r Výsledek z fronty (nebo sestavený UI při nezařazeném požadavku).
 */
static void snapshot_ui_handle_result(const st_SNAPSHOT_REQ_RESULT *r)
{
    char msg[256];
    bool ok = (r->result == SNAPSHOT_OK);
    const char *err = snapshot_result_to_string(r->result);

    switch (r->kind) {
        case SNAPSHOT_REQ_SAVE:
            if (ok) snprintf(msg, sizeof(msg), "%s: %.200s", _("Snapshot saved"), r->path);
            else snprintf(msg, sizeof(msg), "%s: %s", _("Save failed"), err);
            snapshot_notification_show(msg, !ok);
            if (snapshot_ui_was_running_before_pause())
                dbg_ui_run();
            break;

        case SNAPSHOT_REQ_LOAD:
            if (ok) snprintf(msg, sizeof(msg), "%s: %.200s", _("Snapshot loaded"), r->path);
            else snprintf(msg, sizeof(msg), "%s: %s", _("Load failed"), err);
            snapshot_notification_show(msg, !ok);
            /* Po selhání loadu nechat emulátor v pauze. */
            if (ok) snapshot_ui_resume_after_load(g_snapshot_settings.load_resume_mode);
            break;

        case SNAPSHOT_REQ_QUICKSAVE:
            if (ok) snprintf(msg, sizeof(msg), "%s", _("Quick Save OK"));
            else snprintf(msg, sizeof(msg), "%s: %s", _("Quick Save failed"), err);
            snapshot_notification_show(msg, !ok);
            if (snapshot_ui_was_running_before_pause())
                dbg_ui_run();
            break;

        case SNAPSHOT_REQ_QUICKLOAD:
            if (ok) snprintf(msg, sizeof(msg), "%s", _("Quick Load OK"));
            else snprintf(msg, sizeof(msg), "%s: %s", _("Quick Load failed"), err);
            snapshot_notification_show(msg, !ok);
            if (ok) snapshot_ui_resume_after_load(g_snapshot_settings.quickload_resume_mode);
            break;
    }
}


extern "C" void snapshot_ui_submit(en_SNAPSHOT_REQ_KIND kind, const char *path,
                                   const char *description)
{
    /* Požadavek vyřizuje jen paused smyčka emu vlákna. Uživatel mohl
     * emulaci mezitím rozběhnout (otevřený dialog), proto pauza znovu. */
    if (!EMULATOR_TEST_PAUSED)
        dbg_ui_pause();

    if (snapshot_request_submit(kind, path, description) == 0) {
        st_SNAPSHOT_REQ_RESULT r = {};
        r.kind = kind;
        r.result = SNAPSHOT_ERR_IO;
        snapshot_ui_handle_result(&r);
    }
}


extern "C" void imgui_snapshot_request_results(void)
{
    st_SNAPSHOT_REQ_RESULT r;
    while (snapshot_request_take_result(&r))
        snapshot_ui_handle_result(&r);
}
