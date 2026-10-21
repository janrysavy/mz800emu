/*
 * dbgapi_dispatcher.cpp - UI dispatcher pro thread switch EMU -> UI
 *
 * Implementace dispatcheru registrovaného v dbgapi přes
 * dbgapi_emu_register_msg_dispatcher(). Volaný z EMU vlákna při
 * dbgapi_emu_send_msg(); pošle SDL custom event s MSG payloadem do
 * hlavního okna emulátoru. SDL event loop v UI vlákně poté zavolá
 * dispatcher_sdl_handler_cb(), který volá dbgapi_ui_invoke_msg_callback()
 * pro doručení do registrovaného UI listeneru.
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */
#include "main.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <SDL3/SDL.h>
#include <glib.h>
#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_window.h"
#include "emulator/debugger/dbgapi_emu.h"
#include "emulator/debugger/dbgapi_ui.h"
#include "emulator/debugger/dbgapi_msg.h"
#include "debugger_state.h"
#include "dbgapi_dispatcher.h"
#include "breakpoints/bpt_state.h"
#include "ui-imgui/bootstrap/myimgui.h"   /* V1.7: g_gui pro auto-open BP window na HIT */

#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
/* V1.C.2 mutant mcp-server: feed MCP Activity ring buffer při každém
 * DBGAPI_MSG_MCP_ACTION broadcastu. log_action() je thread-safe a
 * idempotentní pokud okno není inicializované. */
#include "ui-imgui/mcp_activity/mcp_activity_window.h"
#include "ui-imgui/mcp_activity/action_toast.h"  /* V1.C.3 destruktivní toast */
#endif


/**
 * @brief SDL handler volaný v UI vlákně po doručení custom eventu.
 *
 * Vykoná se v kontextu UI vlákna (SDL event loop). Extrahuje typ MSG
 * z `event->user.code` a payload z `event->user.data2`, pak volá
 * dbgapi_ui_invoke_msg_callback() pro doručení do registrovaného
 * UI listeneru.
 *
 * Vlastnictví dat (data2) přebírá listener (per kontrakt
 * dbgapi_msg_callback_t). Pokud žádný listener registrován,
 * dbgapi_ui_invoke_msg_callback() data uvolní.
 *
 * @param win    Handle hlavního okna (nepoužitý, jen splňuje SDL signaturu).
 * @param event  SDL custom event s payloadem.
 */
static void dispatcher_sdl_handler_cb(SdlAppWindow *win, SDL_Event *event)
{
    (void)win;

    en_DBGAPI_MSG msg = (en_DBGAPI_MSG)event->user.code;
    st_DBGAPI_MSG_DATA *data = (st_DBGAPI_MSG_DATA *)event->user.data2;

    dbgapi_ui_invoke_msg_callback(msg, data);
}


/**
 * @brief Dispatcher volaný z EMU vlákna při dbgapi_emu_send_msg().
 *
 * Posílá SDL custom event SDLAPP_WINDOW_EVENT_WINDOW_CALLBACK_REQUEST
 * do hlavního okna emulátoru ("main_window"). SDL event loop v UI vlákně
 * následně zavolá dispatcher_sdl_handler_cb().
 *
 * Pokud window manager nebo hlavní okno nejsou dostupné (typicky
 * v testovém prostředí bez UI nebo při shutdown), MSG se zahodí
 * a data uvolní.
 *
 * @param msg        Typ MSG.
 * @param data       Heap-alokovaná data; vlastnictví předáno dispatcheru.
 * @param user_data  Nepoužito (ponechání kompatibility se signaturou).
 *
 * @note Volá se z EMU vlákna - nesmí přímo manipulovat ImGui ani
 *       UI stavem. Smí pouze sdlapp_send_message_for_SDL_windowID()
 *       která je thread-safe.
 */
static void dispatcher_emu_send_cb(en_DBGAPI_MSG msg,
                                     st_DBGAPI_MSG_DATA *data,
                                     void *user_data)
{
    (void)user_data;

    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_sdlapp);
    if (!wm)
    {
        if (data)
            g_free(data);
        return;
    };

    SdlAppWindow *win = sdlapp_winmanager_get_window_by_name(wm, "main_window");
    if (!win || !win->sdl_window)
    {
        if (data)
            g_free(data);
        return;
    };

    /*
     * SDL custom event:
     * - type: SDLAPP_WINDOW_EVENT_WINDOW_CALLBACK_REQUEST
     * - code: typ MSG (en_DBGAPI_MSG)
     * - data1: callback funkce - SDL handler pro UI vlákno
     * - data2: heap-alokovaná data (NULL nebo st_DBGAPI_MSG_DATA *)
     */
    sdlapp_send_message_for_SDL_windowID(
        SDL_GetWindowID(win->sdl_window),
        SDLAPP_WINDOW_EVENT_WINDOW_CALLBACK_REQUEST,
        (Sint32)msg,
        (gpointer)dispatcher_sdl_handler_cb,
        (gpointer)data);
}


/**
 * @brief Default UI MSG listener - reaguje na známé MSGy.
 *
 * Volaný v UI vlákně přes dbgapi_ui_invoke_msg_callback() po thread
 * switchi z EMU. Pro V1 jen routuje známé MSGy na refresh request
 * a později na konkrétní akce (= otevřít BP okno při HIT, sync paused
 * stavu pro mode, ...).
 *
 * Pro V1 smart BP (D.1+) může listener registrovat plnohodnotnější
 * routing (= sync g_dbg_ui mode, otevřít BP listou, atd.).
 *
 * Listener přebírá vlastnictví `data` (per kontrakt
 * dbgapi_msg_callback_t) a uvolňuje je přes g_free().
 */
static void dispatcher_default_listener_cb(en_DBGAPI_MSG msg,
                                              st_DBGAPI_MSG_DATA *data,
                                              void *user_data)
{
    (void)user_data;

    switch (msg)
    {
        case DBGAPI_MSG_BREAKPOINT_HIT:
            /* V1.7: zachytíme id zasaženého BP do g_bpt_ui pro UI použití
             * (highlight v BP list, auto-scroll, tooltip "last hit" v
             * disasm gutter). Payload data->id obsahuje bpt_id přímo -
             * není potřeba další lookup. Auto-otevřeme BP window aby
             * uživatel okamžitě viděl "viníka" pauzy. */
            if (data) {
                g_bpt_ui.last_triggered_id = data->id;
                /* last_triggered_at_frame zatím nepoužíváme (fade-out je
                 * volitelný feature pro pozdější iteraci). */
                g_bpt_ui.scroll_to_last_triggered = true;
            };
            if (g_gui) {
                g_gui->showBreakpointsWindow = true;
            };
            dbg_refresh_request();
            break;

        case DBGAPI_MSG_RUNNING:
            /* User pustil Play / Resume - last triggered highlight zmizí.
             * Pokud bude nový HIT, dispatcher nastaví znovu. */
            g_bpt_ui.last_triggered_id = -1;
            g_bpt_ui.scroll_to_last_triggered = false;
            dbg_refresh_request();
            break;

        case DBGAPI_MSG_PAUSED:
        case DBGAPI_MSG_STEP_COMPLETE:
        case DBGAPI_MSG_RESET_DONE:
            /* Refresh debug oken - state se přečte při dalším iter. */
            dbg_refresh_request();
            break;

        case DBGAPI_MSG_MCP_ACTION:
            /* V1.C.2 mutant mcp-server: feed Activity ring buffer. MCP_ACTION
             * je per definici emitován pouze pro cmd_origin = MCP (viz
             * dbgapi_emit_mcp_action() v dbgapi.c), takže origin badge je
             * vždy DBGAPI_CMD_ORIGIN_MCP. Pokud budou v budoucnu broadcastované
             * i USER/TEST/INTERNAL akce, MSG datová struktura dostane samostatný
             * origin field a hodnotu si přečteme odtud. log_action() je
             * idempotentní pokud okno init neproběhl (= no-op). */
            if (data && data->description)
            {
                g_debug("dbgapi: MCP action: %s (cmd=%d, ts=%" G_GUINT64_FORMAT " us)",
                        data->description, (int)data->cmd,
                        (guint64)data->timestamp_us);
#ifdef MZ800EMU_CFG_MCP_SERVER_ENABLED
                mcp_activity_window_log_action(DBGAPI_CMD_ORIGIN_MCP,
                                                data->cmd,
                                                data->description,
                                                data->timestamp_us,
                                                data->entity_kind,
                                                data->entity_id,
                                                data->rejected != 0);
                /* V1.C.3 - selektivní toast pro destruktivní MCP akce.
                 * Funkce sama filtruje (= USER/TEST/INTERNAL no-op a
                 * read-only cmds no-op), takže bezpodmínečné volání je OK.
                 * Odmítnutý podmíněný zápis (observe-cas M10) nic nezměnil,
                 * toast pro něj nevzniká, stačí řádek v Activity. */
                if (!data->rejected)
                {
                    action_toast_show(DBGAPI_CMD_ORIGIN_MCP,
                                      data->cmd,
                                      data->description);
                };
#endif
            };
            break;

        case DBGAPI_MSG_EXITING:
        case DBGAPI_MSG_NONE:
        default:
            /* Bez akce */
            break;
    };

    /* Uvolnit payload včetně případného MCP_ACTION description (g_strdup). */
    dbgapi_msg_data_free(data);
}


extern "C" void dbgapi_dispatcher_init(void)
{
    dbgapi_emu_register_msg_dispatcher(dispatcher_emu_send_cb, NULL);
    dbgapi_ui_register_msg_callback(dispatcher_default_listener_cb, NULL);
}


extern "C" void dbgapi_dispatcher_shutdown(void)
{
    dbgapi_ui_unregister_msg_callback();
    dbgapi_emu_register_msg_dispatcher(NULL, NULL);
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
