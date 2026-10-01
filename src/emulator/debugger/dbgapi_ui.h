/*
 * dbgapi_ui.h — API pro stranu UI (klient)
 *
 * Funkce v tomto headeru volá UI vlákno (hlavní vlákno, SDL event loop):
 * - Odesílání CMDRQ příkazů do emulátoru a čekání na odpovědi
 * - Příjem MSG notifikací z emulátoru (callback v SDL event loop)
 *
 * Includovat pouze v UI části kódu (src/ui-imgui/, src/ui/).
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
#ifndef DBGAPI_UI_H
#define DBGAPI_UI_H

#include "main.h"
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>
#include "app/app_thread.h"
#include "dbgapi_cmdrq.h"
#include "dbgapi_msg.h"

/* Globální fronta CMDRQ — definice v dbgapi.c */
extern st_DBGAPI_CMDRQ_QUEUE g_dbgapi_cmdrq_queue;


#ifdef __cplusplus
extern "C"
{
#endif

/* ============================================================================
 * ODESÍLÁNÍ PŘÍKAZŮ (CMDRQ) — UI → EMU
 *
 * UI vlákno vkládá příkazy do kruhového bufferu a čeká na odpovědi.
 * Všechny funkce jsou blokující — UI vlákno čeká na zpracování emulátorem.
 *
 * Vlastnictví paměti:
 * - data_ptr a result_ptr alokuje a vlastní volající
 * - data_ptr: vstupní data pro emulátor (může být NULL)
 * - result_ptr: buffer kam emulátor zapíše odpověď (může být NULL)
 * - Po návratu z submit_cmd_sync() klient může číst result_ptr a success
 * ============================================================================ */

/*
 * Synchronní odeslání příkazu do emulátoru s podrobným výsledkem.
 *
 * Vloží příkaz do CMDRQ fronty s identifikací zdroje (cmd_origin),
 * probudí emulátorové vlákno (queue_cond) a čeká ve smyčce na zpracování
 * (slot->cond; předčasné probuzení čekání neukončí).
 *
 * Sémantika timeoutu (timeout_ms > 0):
 *   - pokud emu příkaz do timeoutu NEvyzvedlo z fronty, příkaz se zruší
 *     (slot CANCELLED), emu ho později přeskočí a NEprovede ho; vrací
 *     DBGAPI_SUBMIT_TIMEOUT,
 *   - pokud emu příkaz už vyzvedlo (rozpracovaný), funkce čeká bez limitu
 *     na jeho dokončení a vrátí skutečný výsledek handleru; timeout tedy
 *     omezuje jen čekání ve frontě, ne dobu zpracování.
 *   Slot se nikdy neopouští rozpracovaný, proto data_ptr/result_ptr stačí
 *   udržet platné do návratu z funkce.
 *
 * Parametry:
 *   queue:       ukazatel na CMDRQ frontu
 *   cmd:         příkaz (en_DBGAPI_CMD), volitelně OR s DBGAPI_CMDFLAG_BLOCKING
 *   origin:      zdroj příkazu (USER/MCP/TEST/INTERNAL)
 *   data_ptr:    vstupní data pro emulátor (NULL pokud příkaz nepotřebuje data)
 *   result_ptr:  buffer pro odpověď (NULL pokud příkaz nevrací data)
 *   timeout_ms:  limit čekání na vyzvednutí emulátorem v ms (0 = neomezený)
 *
 * Vrací (en_DBGAPI_SUBMIT_STATUS):
 *   DBGAPI_SUBMIT_OK         = provedeno, handler uspěl
 *   DBGAPI_SUBMIT_FAILED     = provedeno, handler neuspěl (rq->success == false)
 *   DBGAPI_SUBMIT_TIMEOUT    = nevyzvednuto do timeoutu, zrušeno, NEprovedeno
 *   DBGAPI_SUBMIT_QUEUE_FULL = fronta plná, nezařazeno, NEprovedeno
 *   DBGAPI_SUBMIT_ENDING     = emulátor se ukončuje, nezařazeno, NEprovedeno
 *
 * Předpoklady: nesmí se volat z emu vlákna (deadlock). Handler v dispatch
 * nesmí čekat na vlákno odesílatele (odesílatel čeká na rozpracovaný
 * příkaz bez limitu).
 */
en_DBGAPI_SUBMIT_STATUS dbgapi_ui_submit_cmd_sync_ex(st_DBGAPI_CMDRQ_QUEUE *queue,
                                                     en_DBGAPI_CMD cmd,
                                                     en_DBGAPI_CMD_ORIGIN origin,
                                                     void *data_ptr,
                                                     void *result_ptr,
                                                     int timeout_ms);


/*
 * Synchronní odeslání příkazu do emulátoru s explicitním origin.
 *
 * Obal nad dbgapi_ui_submit_cmd_sync_ex() se zjednodušeným výsledkem
 * (true jen pro DBGAPI_SUBMIT_OK). Sémantika čekání a timeoutu viz
 * dbgapi_ui_submit_cmd_sync_ex().
 *
 * Origin propagace:
 *   - cmd_origin se zkopíruje do slot->cmd_origin při zařazení do fronty
 *   - po úspěšném dispatchi, pokud cmd_origin == DBGAPI_CMD_ORIGIN_MCP,
 *     EMU vlákno emituje broadcast DBGAPI_MSG_MCP_ACTION
 *   - ostatní origin (USER/TEST/INTERNAL) jsou dnes equivalentní z pohledu
 *     side efektu (= jen audit), případný GUI Activity Log si filtr řeší sám
 *
 * Parametry:
 *   queue:       ukazatel na CMDRQ frontu
 *   cmd:         příkaz (en_DBGAPI_CMD), volitelně OR s DBGAPI_CMDFLAG_BLOCKING
 *   origin:      zdroj příkazu (USER/MCP/TEST/INTERNAL)
 *   data_ptr:    vstupní data pro emulátor (NULL pokud příkaz nepotřebuje data)
 *   result_ptr:  buffer pro odpověď (NULL pokud příkaz nevrací data)
 *   timeout_ms:  limit čekání na vyzvednutí emulátorem v ms (0 = neomezený)
 *
 * Vrací:
 *   true  = příkaz byl úspěšně zpracován (rq->success == true)
 *   false = chyba (timeout = příkaz neproveden, fronta plná, emulátor se
 *           ukončuje, rq->success == false); rozlišení viz _ex varianta
 */
bool dbgapi_ui_submit_cmd_sync_with_origin(st_DBGAPI_CMDRQ_QUEUE *queue,
                                            en_DBGAPI_CMD cmd,
                                            en_DBGAPI_CMD_ORIGIN origin,
                                            void *data_ptr,
                                            void *result_ptr,
                                            int timeout_ms);


/*
 * Synchronní odeslání příkazu do emulátoru (backward compat wrapper).
 *
 * Volá dbgapi_ui_submit_cmd_sync_with_origin() s implicitním
 * origin = DBGAPI_CMD_ORIGIN_USER. Pro GUI callsites (klik, hotkey, menu)
 * je toto výchozí cesta. Test framework a MCP wrapper by měly volat
 * _with_origin variantu přímo.
 *
 * Parametry: viz dbgapi_ui_submit_cmd_sync_with_origin (bez origin).
 *
 * Příklad:
 *   bool is_running;
 *   bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue,
 *                                       DBGAPI_CMD_IS_RUNNING,
 *                                       NULL, &is_running, 100);
 */
bool dbgapi_ui_submit_cmd_sync(st_DBGAPI_CMDRQ_QUEUE *queue,
                                en_DBGAPI_CMD cmd,
                                void *data_ptr,
                                void *result_ptr,
                                int timeout_ms);

/*
 * Zjistí, zda je CMDRQ fronta plná.
 * Vrací true pokud není místo pro další příkaz.
 * Thread-safe (zamyká queue_mutex).
 */
bool dbgapi_ui_queue_is_full(st_DBGAPI_CMDRQ_QUEUE *queue);

/*
 * Zjistí, zda se emulátor chystá ukončit.
 * Pokud vrátí true, UI by nemělo posílat další příkazy.
 * Thread-safe (zamyká queue_mutex).
 */
bool dbgapi_ui_is_ending(st_DBGAPI_CMDRQ_QUEUE *queue);


/* ============================================================================
 * PŘÍJEM MSG NOTIFIKACÍ — EMU → UI
 *
 * MSG přicházejí přes SDL event loop jako custom eventy.
 * Callback dbgapi_msg_handler_cb() se registruje jako handler
 * pro SDLAPP_WINDOW_EVENT_WINDOW_CALLBACK_REQUEST eventy.
 *
 * UI zaregistruje vlastní callback, který se zavolá při příjmu MSG.
 * Callback běží v kontextu UI vlákna (SDL event loop).
 * ============================================================================ */

/* Typ callbacku pro příjem MSG v UI */
typedef void (*dbgapi_msg_callback_t)(en_DBGAPI_MSG msg, st_DBGAPI_MSG_DATA *data, void *user_data);

/*
 * Registrace callbacku pro příjem MSG z emulátoru.
 * callback: funkce, která se zavolá v UI vlákně při příjmu MSG
 * user_data: libovolná data předávaná callbacku
 *
 * Může být zaregistrován pouze jeden callback. Opakované volání
 * přepíše předchozí registraci.
 *
 * Callback je zodpovědný za uvolnění msg_data (pokud není NULL):
 *   void my_msg_handler(en_DBGAPI_MSG msg, st_DBGAPI_MSG_DATA *data, void *ud)
 *   {
 *       switch (msg) { ... }
 *       if (data) g_free(data);
 *   }
 */
void dbgapi_ui_register_msg_callback(dbgapi_msg_callback_t callback, void *user_data);

/*
 * Odregistrace MSG callbacku.
 * Po zavolání nebudou MSG doručovány.
 */
void dbgapi_ui_unregister_msg_callback(void);


/**
 * @brief Doručit MSG do registrovaného UI callbacku.
 *
 * Volá registrovaný callback (přes dbgapi_ui_register_msg_callback)
 * synchronně v aktuálním vlákně. Volat z UI vlákna - typicky z SDL
 * handleru po thread switchi z EMU.
 *
 * Pokud žádný callback není zaregistrován, data se uvolní přes
 * g_free() a nic se neudělá. Jinak je zodpovědnost za uvolnění dat
 * na callbacku (per kontrakt dbgapi_msg_callback_t).
 *
 * @param msg   Typ zprávy.
 * @param data  Volitelná data - vlastnictví předáno callbacku, nebo
 *              uvolněno pokud žádný callback.
 *
 * @note Tato funkce je veřejné API mezi UI dispatcherem a registrovaným
 *       MSG callbackem. dbgapi.c neví o SDL ani sdlapp - thread switch
 *       řeší výhradně UI vrstva (= dbgapi_dispatcher v src/ui-imgui/).
 */
void dbgapi_ui_invoke_msg_callback(en_DBGAPI_MSG msg, st_DBGAPI_MSG_DATA *data);


#ifdef __cplusplus
}
#endif

#endif /* DBGAPI_UI_H */
