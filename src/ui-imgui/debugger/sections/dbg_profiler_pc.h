/**
 * @file   dbg_profiler_pc.h
 * @brief  Záložka "Per PC" okna CPU Profiler - profiler podle PC.
 *
 * Zobrazuje data profileru podle PC (emulator/debugger/profiler_pc.h):
 * pro každou adresu instrukce počet vykonání, T-stavy a WAIT. Data čte
 * přes dbgapi příkaz DBGAPI_CMD_PROFILER_PC (4x za sekundu), symboly
 * dohledává sama v UI vlákně (sym_db).
 *
 * @par Vlákna
 * Jen UI vlákno.
 *
 * @par Licence: GPLv3
 */

#ifndef DBG_PROFILER_PC_H
#define DBG_PROFILER_PC_H

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vykreslí obsah záložky (toolbar, tabulka, stavový řádek).
 *
 * @pre Volat uvnitř okna CPU Profiler (mezi ImGui::Begin a End), UI vlákno.
 * @post Případně odeslané příkazy (start, stop, reset, export) jsou
 *       synchronně vyřízené emu vláknem nebo zahozené po timeoutu.
 */
void dbg_profiler_pc_render ( void );

#ifdef __cplusplus
}
#endif

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */

#endif /* DBG_PROFILER_PC_H */
