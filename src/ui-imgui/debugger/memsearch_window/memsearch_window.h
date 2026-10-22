/*
 * memsearch_window.h - okno Memory Search (postupné hledání proměnných).
 *
 * Stavový vyhledávač nad jádrem memsearch (emulator/debugger/memsearch):
 * výběr prostoru a typu hodnoty, nové hledání, vzorek, filtry, undo/redo,
 * tabulka kandidátů a akce nad kandidátem (Memory Browser, Watch, write
 * breakpoint, Freeze, Who wrote, vyřazení). Vzorky pořizuje emu vlákno přes
 * DBGAPI_CMD_MEMSEARCH_SAMPLE (dbg_ui_memsearch_sample), filtr běží na UI
 * vlákně. Sezení okna je oddělené od sezení MCP.
 *
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
 */

#ifndef MEMSEARCH_WINDOW_H
#define MEMSEARCH_WINDOW_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vykreslí okno Memory Search.
 *
 * Tlačítka New search, Sample a Filter synchronně čekají na vzorek z emu
 * vlákna (za běhu nejbližší hranice snímku, v pauze hned).
 *
 * @param p_open Viditelnost okna; NULL nebo false = nic.
 * @pre Volá se z UI vlákna v rámci ImGui snímku.
 */
void memsearch_window_render(bool *p_open);

/**
 * @brief Zaregistruje persistenci nastavení okna do sekce INI.
 *
 * Klíče sekce [MEMSEARCH_WINDOW]: space (předvolba 0-5), type (0-9:
 * u8, s8, u16, s16, u24, s24, u32, s32, bcd, digits), width (bcd 1-4,
 * digits 1-8), big_endian, aligned, digit_encoding (0 holé, 1 ASCII,
 * 2 display kód), leading_space, allow_large. Kandidáti se neukládají.
 *
 * @param cmod st_CFGMODULE* sekce (vytvořené volajícím).
 * @pre Volá se jednou při startu před cfgmodule_parse().
 */
void memsearch_window_register_persistence(void *cmod);

#ifdef __cplusplus
}
#endif

#endif /* MEMSEARCH_WINDOW_H */
