/*
 * ramlog_window.h - okno RAM Write Log (záznam zápisů do paměti).
 *
 * Ovládání záznamu (start / stop / vyprázdnit, rozsah, režim ringu,
 * kapacita), dotaz "kdo zapsal tento bajt" a tabulka nejnovějších zápisů.
 * Data čte emu vlákno přes DBGAPI_CMD_RAMLOG (dbg_ui_ramlog); okno drží jen
 * kopie výsledků.
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

#ifndef RAMLOG_WINDOW_H
#define RAMLOG_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vykreslí okno RAM Write Log.
 *
 * Obnovuje stav a tabulku nejnovějších zápisů nejvýš dvakrát za sekundu
 * (synchronní dotaz do emu vlákna, za běhu čeká na konec snímku) a provede
 * dotaz vyžádaný z Memory Browseru.
 *
 * @param p_open Viditelnost okna; NULL nebo false = nic.
 * @pre Volá se z UI vlákna v rámci ImGui snímku.
 */
void ramlog_window_render(bool *p_open);

/**
 * @brief Otevře okno a vyžádá dotaz "kdo zapsal" pro CPU adresu.
 *
 * Buňka se určí podle mapování paměti v okamžiku dotazu (provede se při
 * příštím vykreslení okna).
 *
 * @param addr CPU adresa.
 * @post g_gui->showRamlogWindow == true, okno dostane fokus.
 */
void ramlog_window_query_cpu(uint16_t addr);

/**
 * @brief Otevře okno a vyžádá dotaz "kdo zapsal" pro bajt regionu Memory Browseru.
 *
 * @param region_kind en_REGION_KIND (dbgapi_regions.h).
 * @param sub_id      sub_id regionu.
 * @param offset      Offset v regionu.
 * @post g_gui->showRamlogWindow == true, okno dostane fokus.
 */
void ramlog_window_query_region(int region_kind, int sub_id, uint32_t offset);

/**
 * @brief Zda Memory Browser nabízí pro region dotaz "kdo zapsal".
 *
 * @param region_kind en_REGION_KIND.
 * @return true = Logical (CPU adresa) nebo region s buňkami záznamu zápisů
 *         (RAM, MemExt RAM, VRAM, CG-RAM, PCG); false = ROM, RAMdisk, FLASH
 *         a další regiony mimo záznam.
 */
bool ramlog_window_region_supported(int region_kind);

#ifdef __cplusplus
}
#endif

#endif /* RAMLOG_WINDOW_H */
