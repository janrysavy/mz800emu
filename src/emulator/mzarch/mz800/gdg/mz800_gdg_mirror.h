/**
 * @file mz800_gdg_mirror.h
 * @brief Side-effect free read API pro GDG state MZ-800 (gdg-panel F2).
 *
 * GDG (Graphic Display Generator) je custom video LSI obvod. Hlavní emulační
 * struktura `st_GDG g_gdg` je veřejná v `mz800_gdg.h`, ale UI debugger panel
 * by neměl číst raw fieldy přímo - místo toho používá tento mirror modul,
 * který:
 *
 *  - kopíruje relevantní fieldy do vlastní snapshot struktury per volání
 *    (= "atomic" pohled na stav v daný okamžik, žádný tearing mezi fieldy
 *    napříč iteracemi panelu),
 *  - dokumentuje JMÉNA a VÝZNAMY fieldů ze strany konzumenta (UI), zatímco
 *    `st_GDG` je orientována na emulační implementaci,
 *  - udržuje single-direction závislost: emulator -> mirror -> UI panel.
 *    UI NIKDY nepoužívá emulační hlavičku přímo.
 *
 * Side-effect free invariant:
 *  - Funkce `gdg_mirror_snapshot()` POUZE čte fieldy `g_gdg` a kopíruje
 *    je do output structu. Žádný state mutation, žádné `gdg_write_*()`,
 *    žádné stepping či scheduling.
 *
 * Co tato hlavička reálně exportuje (F2 + F3):
 *  - regDMD (port 0xCEh) - dekód do bits MZ700/SCRW640/HICOLOR/VBANK,
 *  - regBOR (port 0x06CFh) - 4-bit IGRB border color,
 *  - regPALGRP + regPAL0..3 (port 0xF0h) - paleta MZ-800,
 *  - raster pozice (beam_row, ticks v aktuálním screen, vbln, hbln,
 *    sts_hsync, sts_vsync, total elapsed screens),
 *  - tempo, cksw, regct53g7,
 *  - F3: vramctrl RF/WF state (regWF_PLANE, regWF_MODE, regRF_PLANE,
 *    regRF_SEARCH, regWFRF_VBANK) a WRITE FF modelu WAIT u VRAM,
 *  - F3: hwscroll state (regSSA, regSEA, regSW, regSOF, enabled).
 *
 * NENI součástí mirror snapshot:
 *  - BCOL (mid-frame border change) - field neexistuje v `st_GDG`,
 *    pravděpodobně se realizuje přes deferred render mechanismus
 *    (`screen_need_update_from` / `last_updated_border_pixel`) bez
 *    samostatného field. [neověřeno] Panel zobrazuje pouze regBOR
 *    klidovou hodnotu, live BCOL = TODO pro budoucí F.
 *  - MEX (memory extension flags) - není v GDG, žije v memory modulu
 *    (`MEMEXT_TEST_CONNECTED` v `mz800_memory.c`). Panel je nezobrazuje
 *    (= Memory Map okno).
 *  - CG-RAM data: prvních 4 KB plane I (`g_memoryVRAM[0..0xFFF]`),
 *    žádný dedikovaný field; panel ji nezobrazuje (= Memory Map okno).
 *
 * License: GPLv3.
 */

#ifndef MZ800_GDG_MIRROR_H
#define MZ800_GDG_MIRROR_H

#include "mzarch/mzarch_config.h"

#if MZARCH == 800

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Snapshot stavu MZ-800 GDG určený pro UI debugger.
 *
 * Vyplňuje funkce `gdg_mirror_snapshot()`. Struktura je čistě hodnotová
 * (POD, kopie by value) - nedrží žádné pointery na živá data, takže
 * konzument může držet kopii libovolně dlouho.
 *
 * Invariant: všechny fieldy jsou snapshot v okamžiku volání. UI vlákno
 * čte tuto kopii, emulační vlákno mezitím může `g_gdg` libovolně mutovat;
 * konzistence napříč voláními NENÍ garantována (= panel se obnovuje
 * per frame a získá nový snapshot pokaždé).
 */
typedef struct st_GDG_MIRROR_MZ800
{
    /* ---- DMD register (port 0xCEh) ------------------------------------- */
    uint8_t  regDMD;        /**< Surová hodnota DMD registru (low 4 bity). */
    uint8_t  dmd_mz700;     /**< Bit 3: 1 = MZ-700 kompatibilní mód. */
    uint8_t  dmd_scrw640;   /**< Bit 2: 1 = 640x200, 0 = 320x200. */
    uint8_t  dmd_hicolor;   /**< Bit 1: 1 = 16 barev (ext VRAM), 0 = 4 barvy. */
    uint8_t  dmd_vbank;     /**< Bit 0: aktivní VBANK (0 = A, 1 = B). */

    /* ---- Border + palette registry (port 0x06CFh / 0xF0) --------------- */
    uint8_t  regBOR;        /**< Border color (IGRB, 4 bity). */
    uint8_t  regPALGRP;     /**< Aktivní palette group (0..3). */
    uint8_t  regPAL0;       /**< PAL slot 0: IGRB barva. */
    uint8_t  regPAL1;       /**< PAL slot 1: IGRB barva. */
    uint8_t  regPAL2;       /**< PAL slot 2: IGRB barva. */
    uint8_t  regPAL3;       /**< PAL slot 3: IGRB barva. */

    /* ---- Raster pozice ------------------------------------------------- */
    unsigned beam_row;      /**< Aktuální řádek paprsku (0..VIDEO_SCREEN_HEIGHT-1). */
    unsigned screen_ticks;  /**< Pozice v aktuálním screenu (pixel
                             *   tick 0..VIDEO_SCREEN_TICKS-1). */
    unsigned screens_done;  /**< Celkový počet vykreslených snímků od resetu. */

    uint8_t  hbln;          /**< HBLN signál: 0 = mimo screen (sloupec), 1 = on. */
    uint8_t  vbln;          /**< VBLN signál: 0 = mimo screen (řádek), 1 = on. */
    uint8_t  sts_hsync;     /**< STS HSYNC viditelný v Status registru. */
    uint8_t  sts_vsync;     /**< STS VSYNC viditelný v Status registru. */

    /* ---- Ostatní ------------------------------------------------------- */
    uint8_t  tempo;         /**< Tempo signál (1 Hz bit) - low bit pro Status. */
    unsigned tempo_divider; /**< Aktuální stav děliče pro tempo. */
    uint8_t  cksw;          /**< CKSW (Superimpose) bit, set zapisem 0xCF07 b7. */
    uint8_t  regct53g7;     /**< Stav řízení GATE pro CTC0 v MZ-700 módu. */

    /* ---- VRAM controller (g_vramctrl, F3) ------------------------------ */
    /*
     * Read/Write Format registry pro 800 grafický mode. Pojmenování fieldů
     * a sémantika převzata z `mz800_vramctrl.h`. Hodnoty jsou snapshot v okamžiku
     * volání - emulátor je mutuje při zápisech do GDG registrů 0xCC..0xCD a 0xCF.
     */
    uint8_t  vc_wf_plane;   /**< WF: maska 4 plane (bity 0..3 = I,II,III,IV). */
    uint8_t  vc_wf_mode;    /**< WF: režim zápisu (enum WFR_MODE: SINGLE/EXOR/OR/RESET/REPLACE/PSET). */
    uint8_t  vc_rf_plane;   /**< RF: výběr plane pro čtení (bity 0..3). */
    uint8_t  vc_rf_search;  /**< RF: search mode bity (= komparátor pro plane match). */
    uint8_t  vc_wfrf_vbank; /**< WF/RF: aktivní VBANK pro VRAM přístup (0 = A, 1 = B). */
    uint8_t  vc_vram_wrff_set; /**< WRITE FF modelu WAIT u VRAM: 1 = poslední zápis CPU ještě není v DRAM (další přístup čeká). */

    /* ---- Hardware scroll (g_hwscroll, F3) ------------------------------ */
    /*
     * HW scroll registry pro 800 grafický mode. SSA = scroll start address,
     * SEA = scroll end address, SW = scroll width (= velikost okna v VRAM
     * jednotkách bajtů plane), SOF = scroll offset (= o kolik bajtů paplane
     * posouvat zobrazené řádky). Pole `enabled` je interní cache (z `regSEA`
     * != 0 odvozen).
     */
    unsigned hs_ssa;        /**< Scroll Start Address (bajty plane). */
    unsigned hs_sea;        /**< Scroll End Address (bajty plane). */
    unsigned hs_sw;         /**< Scroll Width (bajty plane). */
    unsigned hs_sof;        /**< Scroll Offset (bajty plane). */
    uint8_t  hs_enabled;    /**< Cached flag: 1 = HW scroll aktivní. */
} st_GDG_MIRROR_MZ800;

/**
 * @brief Vyplní snapshot strukturu aktuálním stavem `g_gdg`.
 *
 * Side-effect free: čte pouze raw fieldy `g_gdg`, žádný state mutation.
 *
 * @param out  Pointer na cílovou strukturu (kterou funkce přepíše).
 *             Nesmí být NULL.
 *
 * @pre  `out != NULL`. Emulátor inicializován (`gdg_init()` proběhl).
 * @post `*out` obsahuje aktuální snapshot. Návratová hodnota žádná.
 */
void gdg_mirror_snapshot(st_GDG_MIRROR_MZ800 *out);

#ifdef __cplusplus
}
#endif

#endif /* MZARCH == 800 */

#endif /* MZ800_GDG_MIRROR_H */
