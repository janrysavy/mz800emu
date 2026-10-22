/**
 * @file snapshot_request_ui.h
 * @brief GUI strana požadavků na snapshot: zařazení s pauzou, oznámení
 *        výsledku a obnovení běhu.
 *
 * Dialogy Load / Save Snapshot a Quick Load / Quick Save snapshot samy
 * neprovádějí; požadavek zařadí snapshot_ui_submit() a vyřídí ho emu vlákno
 * v pauze (emulator/snapshot/snapshot_request.h). imgui_snapshot_request_results()
 * každý snímek vyzvedne výsledky, ukáže oznámení a podle nastavení obnoví
 * běh emulace.
 */

#ifndef SNAPSHOT_REQUEST_UI_H
#define SNAPSHOT_REQUEST_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "snapshot/snapshot_request.h"

/**
 * @brief Zařadí požadavek na snapshot; emulaci předtím pozastaví.
 *
 * Když emulace běží, pozastaví ji (dbg_ui_pause(): s debuggerem synchronně
 * přes frontu dbgapi, bez něj příznakem), aby požadavek vyřídila paused
 * smyčka. Stav "běželo před pauzou" pro obnovení běhu drží menu
 * (snapshot_ui_was_running_before_pause()). Požadavek bez cesty (SAVE,
 * LOAD) se nezařadí a ohlásí se rovnou jako chyba.
 *
 * @param kind        Druh požadavku.
 * @param path        Cesta k .mzs (SAVE, LOAD), jinak NULL.
 * @param description Popis (jen SAVE), smí být NULL.
 * @pre Volat z UI vlákna.
 */
void snapshot_ui_submit(en_SNAPSHOT_REQ_KIND kind, const char *path,
                        const char *description);

/**
 * @brief Vyzvedne výsledky vyřízených požadavků, ukáže oznámení a obnoví běh.
 *
 * Obnovení běhu podle druhu: Save a Quick Save rozběhnou emulaci, pokud
 * před pauzou běžela; Load a Quick Load po úspěchu podle
 * `load_resume_mode` / `quickload_resume_mode`, po chybě zůstává pauza.
 *
 * @pre Volat z UI vlákna každý snímek (main_window).
 */
void imgui_snapshot_request_results(void);

#ifdef __cplusplus
}
#endif

#endif /* SNAPSHOT_REQUEST_UI_H */
