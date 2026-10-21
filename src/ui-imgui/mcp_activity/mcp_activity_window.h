/*
 * File:   mcp_activity_window.h
 *
 * MCP Activity okno (V1.C.2 mutant mcp-server).
 *
 * Real-time GUI log MCP akcí, které vykonali připojení klienti (= AI agent,
 * test framework, interní emu). Slouží pro "human in the loop" workflow -
 * uživatel paralelně vidí co automat dělá s emulátorem.
 *
 * Zdroj událostí:
 *   DBGAPI_MSG_MCP_ACTION (= V-1.3 broadcast) doručený UI dispatcherem
 *   (= src/ui-imgui/debugger/dbgapi_dispatcher.cpp). Dispatcher po
 *   thread switchi z EMU vlákna volá mcp_activity_window_log_action(),
 *   která bezpečně vloží záznam do ring bufferu.
 *
 * Threading model:
 *   - mcp_activity_window_log_action() je volána z UI vlákna (SDL event
 *     loop po thread switchi z EMU). Vlastní render také běží v UI vlákně.
 *   - Přesto všechna data jsou chráněna GMutex - defenzivní pro případ
 *     budoucího volání z jiných vláken (= harvest MCP wrapper logu, ...).
 *
 * Ring buffer:
 *   - Pevná velikost MCP_ACTIVITY_RING_MAX_ENTRIES (= 1000 záznamů).
 *   - Při přetečení FIFO drop nejstaršího (head pointer rolování).
 *
 * UI prvky:
 *   - Tabulka 4 sloupců: Time | Origin | Cmd | Description.
 *   - Toolbar: Clear, Pause, Auto-scroll, Filter (= textový hledač).
 *   - Status bar: počet záznamů, počet po filtru, indikátor "PAUSED".
 *
 * Owner badge:
 *   - Origin sloupec ukazuje textový badge [AI] / [USR] / [TEST] / [SYS]
 *     (= odpovídá enum en_DBGAPI_CMD_ORIGIN). Emoji se zatím nepoužívají
 *     kvůli omezení defaultního ImGui fontu.
 *
 * Visibilita:
 *   - g_gui->showMcpActivityWindow toggle (= menu Debugger / MCP Activity).
 *   - Show flag NENÍ persistovaný v INI (= per-session).
 *
 * Scope V1.C.2:
 *   - Žádný persist na disk (= action_log_path je TODO pro V1.C.3+).
 *   - Žádné owner badges na BP/Watch UI (= V1.C.3).
 *   - Žádný toast (= V1.C.3).
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

#ifndef MCP_ACTIVITY_WINDOW_H
#define MCP_ACTIVITY_WINDOW_H

#include <stdbool.h>
#include <stdint.h>
#include "emulator/debugger/dbgapi_cmdrq.h"

/**
 * @brief Maximální počet záznamů držených v ring bufferu.
 *
 * Při přetečení FIFO drop nejstaršího záznamu (drop-tail-old). Velikost
 * je kompromis mezi spotřebou paměti (každý záznam ~80 B vč. description)
 * a viditelnou historií. Hodnotu lze v budoucnu udělat konfigurovatelnou
 * přes INI.
 */
#define MCP_ACTIVITY_RING_MAX_ENTRIES 1000

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Jednorázová inicializace okna a ring bufferu.
 *
 * Vytvoří interní GMutex a vyprázdní ring buffer. Volat jednou před
 * prvním renderem (= z bootstrap UI vrstvy). Idempotentní - opakované
 * volání je no-op.
 *
 * @pre Žádné.
 * @post Ring buffer je prázdný, GMutex inicializován,
 *       mcp_activity_window_log_action() je možné volat.
 */
void mcp_activity_window_init(void);

/**
 * @brief Uvolnění interních struktur okna.
 *
 * Vyprázdní ring buffer (uvolní heap description stringy) a zruší
 * GMutex. Po návratu už mcp_activity_window_log_action() ani
 * mcp_activity_window_render() nesmí být voláno.
 *
 * @pre mcp_activity_window_init() byl zavolán (nebo no-op pokud nebyl).
 * @post Ring buffer prázdný, GMutex zrušen.
 */
void mcp_activity_window_shutdown(void);

/**
 * @brief Render okna v aktuálním ImGui frame.
 *
 * Pokud `*p_open` je false, funkce je no-op. Jinak vykreslí toolbar,
 * status bar a tabulku záznamů z ring bufferu (případně filtrovaných).
 *
 * @param p_open  Pointer na show flag; ImGui ho může nastavit na false
 *                pokud uživatel klikne na "X" v záhlaví okna.
 *
 * @pre Volá se v UI vlákně, ImGui kontext aktivní.
 * @post Bez vedlejších efektů na ring buffer (jen čtení).
 */
void mcp_activity_window_render(bool *p_open);

/**
 * @brief Vloží jeden záznam do ring bufferu.
 *
 * Thread-safe - chráněno interním GMutex. Pokud je buffer plný, dropuje
 * se nejstarší záznam (drop-tail-old FIFO). Pokud `description` je NULL,
 * uloží se prázdný řetězec. Buffer si dělá vlastní kopii description
 * (g_strdup), volající si dál spravuje původní řetězec.
 *
 * V "Pause" módu (= uživatel zaškrtl Pause v toolbar) jsou všechny
 * nové záznamy ignorovány - ring buffer není zapisován.
 *
 * V1.E.6.A: entity_kind + entity_id přidávají routing informaci pro
 * dvojklikový skok do cílového okna (BP / Watch / Symbol / Bookmark).
 * Pro řádky bez entity (= entity_kind = DBGAPI_ENTITY_KIND_NONE) je
 * dvojklik no-op.
 *
 * @param origin       Zdroj akce (USER/MCP/TEST/INTERNAL).
 * @param cmd          Identifikátor CMDRQ příkazu (en_DBGAPI_CMD).
 * @param description  Human-readable popis akce (nemůže být NULL je OK).
 * @param timestamp_us Wall-clock monotonic v mikrosekundách
 *                     (= g_get_monotonic_time() z emit-site).
 * @param entity_kind  DBGAPI_ENTITY_KIND_* (0 = NONE = bez routingu).
 * @param entity_id    ID nebo adresa cílové entity (význam dle entity_kind).
 * @param rejected     true = podmíněný zápis odmítnutý nesplněnou podmínkou
 *                     (observe-cas M10); řádek se zobrazí jako odmítnutý
 *                     a do souboru logu se připíše " (rejected)".
 *
 * @pre mcp_activity_window_init() byl zavolán.
 * @post Pokud Pause vypnut, v ring bufferu je o jeden záznam víc
 *       (případně po dropu nejstaršího).
 */
void mcp_activity_window_log_action(en_DBGAPI_CMD_ORIGIN origin,
                                     en_DBGAPI_CMD cmd,
                                     const char *description,
                                     uint64_t timestamp_us,
                                     uint8_t entity_kind,
                                     int32_t entity_id,
                                     bool rejected);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MCP_ACTIVITY_WINDOW_H */
