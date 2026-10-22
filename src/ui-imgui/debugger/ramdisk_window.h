/**
 * @file ramdisk_window.h
 * @brief Debugger: deklarace Memory Disk State inspector okna.
 *
 * Side-effect free ImGui okno pro live view stavu vsech paametovych disku
 * (ramdisku) emulatoru najednou:
 *   - MZ-1R18 kompatibilni standardni ramdisk (`g_ramdisk.std`),
 *   - Pezik 0xE8-0xEF (`g_ramdisk.pezik[RAMDISK_PEZIK_E8]`, jen MZ-700/MZ-800),
 *   - Pezik 0x68-0x6F (`g_ramdisk.pezik[RAMDISK_PEZIK_68]`).
 *
 * Cte vyhradne z `g_ramdisk`, zadny setter ani write. Refresh per frame.
 *
 * Render guardovan `CFG_HWEXT_HAVE_RAMDISK` + `MZ800EMU_CFG_DEBUGGER_ENABLED`
 * v `imgui_ramdisk_state_window` (stubed implementace pro architektury bez
 * ramdisku).
 *
 * License: GPLv3.
 */

#ifndef RAMDISK_WINDOW_H_INCLUDED
#define RAMDISK_WINDOW_H_INCLUDED

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Render Memory Disk State debugger okno.
     *
     * Vykresli ImGui okno s CollapsingHeader sekcemi pro kazdy typ ramdisku
     * (MZ-1R18 standard, Pezik E8, Pezik 68). Cte aktualni stav z globalniho
     * `g_ramdisk` pri kazdem volani (kazdy frame); zadny interni cache.
     *
     * @param p_open Pointer na bool s viditelnosti okna (ImGui idiom).
     *               Pri zavreni [X] se nastavi na false.
     *
     * Preconditions: ImGui kontext aktivni (volani mezi NewFrame a Render).
     * Side effects: zadne mimo ImGui draw listu.
     */
    void imgui_ramdisk_state_window(bool *p_open);

#ifdef __cplusplus
}
#endif

#endif /* RAMDISK_WINDOW_H_INCLUDED */
