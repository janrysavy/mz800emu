/**
 * @file hid_script.h
 * @brief Sekvence vstupu MCP (klávesy, joystick) odměřená snímky emulace.
 *
 * Klávesové a joystickové nástroje MCP potřebují držet vstup přesně N
 * snímků emulace. Měřit to v MCP vlákně nejde: čítač vykreslených snímků
 * při MAX SPEED roste nejvýš jednou za 20 ms reálného času a na statické
 * obrazovce neroste vůbec, a i přesný čítač snímků emulace by MCP vlákno
 * četlo se zpožděním, za které emulace při MAX SPEED uběhne desítky snímků.
 *
 * Proto MCP dispatch předá celou sekvenci (st_DBGAPI_HID_SCRIPT) emu vláknu
 * příkazem DBGAPI_CMD_HID_SCRIPT_START a emu vlákno samo provádí stisky,
 * uvolnění a mezery na hranicích snímků (hid_script_on_frame() z per-frame
 * bodu hlavní smyčky). Za běhu emulace se nic nepauzuje; pokud byla emulace
 * při startu v pauze, START ji rozběhne a po poslední události ji zastaví
 * přes frame-bounded stop (g_debugger.run_frames_*) přesně na hranici
 * snímku, stejně jako emu_run(frames=N).
 *
 * Všechny funkce tohoto modulu volá jen EMU vlákno.
 */

#ifndef HID_SCRIPT_H
#define HID_SCRIPT_H

#include <stdbool.h>

#include "debugger/dbgapi_cmdrq.h"

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief Ukazatel na právě běžící sekvenci, NULL = žádná.
     *
     * Hlavní smyčka testuje jen tento ukazatel (jedno čtení v per-frame
     * bodě), než zavolá hid_script_on_frame().
     *
     * @invariant Čte a píše ho jen EMU vlákno.
     */
    extern st_DBGAPI_HID_SCRIPT *g_hid_script_active;

    /**
     * @brief Spustí sekvenci vstupu (obsluha DBGAPI_CMD_HID_SCRIPT_START).
     *
     * Vynuluje OUT pole, provede první stisk (a případné nulové držení
     * a mezery), zapamatuje si sekvenci v g_hid_script_active a pokud byla
     * emulace v pauze, nastaví `pause_at_end` a emulaci rozběhne. Pokud
     * sekvence doběhne už tady (všechna držení i mezery nulové), dokončí ji
     * hned, včetně `done` = 1, a emulaci nerozbíhá.
     *
     * @param[in,out] script sekvence od volajícího (viz kontrakt
     *                       st_DBGAPI_HID_SCRIPT)
     * @return true = sekvence přijata; false = neplatný parametr (NULL,
     *         count < 1, neplatná klávesa nebo port) nebo už běží jiná
     *         sekvence. Při false se na strukturu nesahá a emu vlákno si ji
     *         nepamatuje.
     *
     * @pre EMU vlákno, bod mezi instrukcemi (drain fronty dbgapi).
     * @post Při true: buď `done` == 1, nebo g_hid_script_active == script.
     */
    bool hid_script_start ( st_DBGAPI_HID_SCRIPT *script );

    /**
     * @brief Krok sekvence na hranici snímku.
     *
     * Provede všechna uvolnění, stisky a konce mezer, jejichž cílový snímek
     * (g_gdg.total_elapsed.screens) už nastal. Po poslední události sekvenci
     * dokončí (OUT pole, `done` = 1) a při `pause_at_end` aktivuje
     * frame-bounded stop na aktuálním snímku, takže se emulace zastaví ve
     * stejném bodě hlavní smyčky jako u emu_run(frames=N).
     *
     * @pre EMU vlákno, per-frame bod hlavní smyčky (po zpracování konce
     *      snímku, mezi instrukcemi), g_hid_script_active != NULL.
     */
    void hid_script_on_frame ( void );

    /**
     * @brief Zruší běžící sekvenci (obsluha DBGAPI_CMD_HID_SCRIPT_CANCEL).
     *
     * Pokud `script` je právě běžící sekvence, uvolní drženou klávesu nebo
     * joystick, odzbrojí sondu dosednutí, zapíše OUT pole s `cancelled`
     * = true a nastaví `done` = 1. Emulaci nepauzuje ani nerozbíhá.
     *
     * @param[in,out] script sekvence, kterou chce volající zrušit
     * @return true = sekvence už emu vlákno nepoužívá (zrušena teď, nebo
     *         skončila dřív); false = `script` je NULL.
     *
     * @pre EMU vlákno, bod mezi instrukcemi.
     */
    bool hid_script_cancel ( st_DBGAPI_HID_SCRIPT *script );

    /**
     * @brief Zruší běžící sekvenci při resetu emulátoru.
     *
     * Reset nuluje čítač snímků, takže cíle sekvence by přestaly dávat
     * smysl. Sekvenci ukončí stejně jako hid_script_cancel(). Bez běžící
     * sekvence nedělá nic.
     *
     * @pre EMU vlákno.
     */
    void hid_script_on_reset ( void );

#ifdef __cplusplus
}
#endif

#endif /* HID_SCRIPT_H */
