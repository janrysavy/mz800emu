/*
 * dbgapi_msg.h — MSG kanál: asynchronní notifikace z Emulátoru do UI
 *
 * Jednosměrný komunikační kanál pro krátké zprávy (signály).
 * Doručení EMU→UI řeší registrovaný dispatcher (viz dbgapi_emu.h
 * dbgapi_emu_register_msg_dispatcher()). Implementace dispatcheru
 * je v UI vrstvě (src/ui-imgui/debugger/dbgapi_dispatcher.cpp) a
 * obvykle používá SDL custom event mechanismus pro thread switch.
 *
 * MSG jsou stručné — „probuď se, něco se stalo". Pokud příjemce potřebuje
 * detailní data, pošle CMDRQ dotaz.
 *
 * Předávání dat:
 * - Jednoduchá data (int): GINT_TO_POINTER() v st_DBGAPI_MSG_DATA
 * - Složitější data: heap alokace (g_new0), příjemce uvolní (g_free)
 * - Většinou MSG nepotřebuje data — je to jen „zvonění budíku"
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
#ifndef DBGAPI_MSG_H
#define DBGAPI_MSG_H

#include <stdint.h>
#include <stdbool.h>
#include "app/app_thread.h"
#include "dbgapi_cmdrq.h"

/* ============================================================================
 * TYPY ZPRÁV (MSG)
 *
 * Emulátor posílá tyto notifikace do UI vlákna. UI na ně reaguje buď
 * přímou akcí (refresh), nebo přes CMDRQ dotazy (zjištění stavu).
 * ============================================================================ */

typedef enum en_DBGAPI_MSG
{
    DBGAPI_MSG_NONE = 0,       /* Bez efektu - ping */
    DBGAPI_MSG_PAUSED,         /* Emulace pozastavena (důvod v datech) */
    DBGAPI_MSG_RUNNING,        /* Emulace spuštěna */
    DBGAPI_MSG_EXITING,        /* Emulátor se chystá ukončit - konec komunikace */
    DBGAPI_MSG_BREAKPOINT_HIT, /* Breakpoint zasažen - emulace automaticky pozastavena */
    DBGAPI_MSG_STEP_COMPLETE,  /* Krok dokončen - emulace automaticky pozastavena */
    DBGAPI_MSG_RESET_DONE,     /* Reset CPU proveden */
    DBGAPI_MSG_MCP_ACTION,     /* Broadcast: byl proveden CMDRQ s cmd_origin = MCP (description v datech) */
} en_DBGAPI_MSG;


/* ============================================================================
 * DOPLŇKOVÁ DATA PRO MSG
 *
 * Některé MSG mohou nést doplňková data. Struktura se alokuje na heapu
 * odesílatelem (g_new0) a uvolňuje příjemcem (g_free). MCP_ACTION navíc
 * vlastní heap-alokovaný `description` string, který musí příjemce před
 * uvolněním celé struktury uvolnit přes g_free() (= viz dbgapi_msg_data_free).
 *
 * Použití dle msg_type:
 *   - BREAKPOINT_HIT: addr = adresa breakpointu, id = ID breakpointu
 *   - STEP_COMPLETE:  addr = aktuální PC po kroku
 *   - PAUSED:         addr = aktuální PC v okamžiku pauzy
 *   - MCP_ACTION:     cmd = provedený CMDRQ, description = human-readable
 *                     popis akce (heap, g_free), timestamp_us = wall-clock
 *                     monotonic z g_get_monotonic_time(), entity_kind +
 *                     entity_id = identifikátor cílové entity pro UI routing
 *                     (V1.E.6.A; viz DBGAPI_ENTITY_KIND_* makra),
 *                     rejected = 1 u podmíněného zápisu odmítnutého
 *                     nesplněnou podmínkou (observe-cas M10)
 *   - Ostatní:        data nejsou potřeba (msg_data = NULL)
 *
 * Fields cmd, description, timestamp_us, entity_id, entity_kind a rejected se používají
 * pouze pro MCP_ACTION; u ostatních MSG zůstávají nulové (memset přes g_new0).
 * Tento společný tvar zachovává signaturu dispatcher_t (st_DBGAPI_MSG_DATA *),
 * takže neexistující dispatcher signatura nemusí být měněna.
 * ============================================================================ */

/**
 * @brief Typ entity, na kterou MCP_ACTION cílí (pro dvojklikové UI routing).
 *
 * Vyplňuje emit-site v dbgapi_emit_mcp_action() podle CMDRQ payload typu.
 * UI vrstva (mcp_activity_window) podle entity_kind rozhodne, které okno
 * otevřít a jakou hodnotu (entity_id) hledat.
 *
 * NONE = žádné routing target (= operace bez cílové entity, např. step,
 *        eventlog_*). Dvojklik takový řádek nic nedělá.
 * BP   = entity_id je breakpoints_find_by_id() vstupní int ID.
 * WATCH= entity_id je stabilní runtime st_WATCH_ROW.id (= ne index v poli).
 * SYM  = entity_id je 16-bit CPU adresa (uint16_t) symbolu, cast na int32.
 * BOOKMARK = entity_id je bookmarks_get_by_id() vstupní uint32_t.
 *            Zatím není žádný CMDRQ BOOKMARK_*, který by sem mapoval -
 *            rezervováno pro V1.E.6.B+ (= scope rozhodnutí Michala).
 */
#define DBGAPI_ENTITY_KIND_NONE      0u   /**< Bez routing target. */
#define DBGAPI_ENTITY_KIND_BP        1u   /**< Breakpoint (BP_ADD/REMOVE/UPDATE/CREATE_WITH_INIT). */
#define DBGAPI_ENTITY_KIND_WATCH     2u   /**< Watch řádek (WATCH_ADD). */
#define DBGAPI_ENTITY_KIND_SYMBOL    3u   /**< Symbol (SYMBOL_ADD), entity_id=addr. */
#define DBGAPI_ENTITY_KIND_BOOKMARK  4u   /**< Bookmark - rezervováno, žádný CMDRQ mapping v V1.E.6.A. */

typedef struct st_DBGAPI_MSG_DATA
{
    en_DBGAPI_MSG msg_type;    /* Typ zprávy (redundantní, pro kontrolu) */
    uint16_t addr;             /* Relevantní adresa (PC, breakpoint, ...) */
    int id;                    /* Relevantní ID (breakpoint ID, ...) */

    /* --- Pole použitá pouze pro DBGAPI_MSG_MCP_ACTION ---
     * U ostatních MSG typů jsou nulová (cmd=0, description=NULL,
     * timestamp_us=0, entity_id=0, entity_kind=NONE). */
    en_DBGAPI_CMD cmd;         /* Provedený CMDRQ (jen MCP_ACTION) */
    char *description;         /* Heap g_strdup, vlastní příjemce; uvolnit g_free (jen MCP_ACTION) */
    uint64_t timestamp_us;     /* Wall-clock monotonic timestamp v mikrosekundách (jen MCP_ACTION) */

    /* --- V1.E.6.A: routing target pro Activity dvojklik (jen MCP_ACTION) --- */
    int32_t entity_id;         /* ID/adresa cílové entity dle entity_kind (0 = NONE) */
    uint8_t entity_kind;       /* DBGAPI_ENTITY_KIND_* (0 = NONE = nesměrovat) */

    /* --- observe-cas (M10): odmítnutý podmíněný zápis (jen MCP_ACTION) --- */
    uint8_t rejected;          /* 1 = podmíněný zápis odmítnut (stav se změnil), nic se neprovedlo */
} st_DBGAPI_MSG_DATA;


#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Uvolní celou st_DBGAPI_MSG_DATA strukturu včetně volitelného description.
 *
 * Použít místo přímého g_free(data) v listenerech, aby se korektně uvolnil
 * heap-alokovaný `description` field (relevantní pro DBGAPI_MSG_MCP_ACTION).
 * U ostatních MSG typů je description NULL a chování je shodné s g_free.
 *
 * @param data  Ukazatel na heap-alokovanou strukturu nebo NULL. Po návratu
 *              je pointer neplatný (= vlastnictví předáno funkci).
 *
 * @note Bezpečné s NULL vstupem.
 */
void dbgapi_msg_data_free(st_DBGAPI_MSG_DATA *data);

#ifdef __cplusplus
}
#endif


#endif /* DBGAPI_MSG_H */
