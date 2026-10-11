#ifndef IMGUI_WINDOWS_H
#define IMGUI_WINDOWS_H

#include <stdint.h>
#include <stdbool.h>
#include "iface-video/sdl3/iface_video_sdl3.h"

#include "cmt/imgui_cmt.h"
#include "version_check/imgui_version_check.h"

#ifdef __cplusplus
extern "C"
{
#endif
    void imgui_main_window(VideoTexture texture);
    void imgui_ShowAboutWindow(bool *p_open);
    void imgui_fps_measurement(bool *p_open);
    void imgui_measuring_frame_timing(bool *p_open);
    void imgui_measuring_gdg(bool *p_open);
    void imgui_maxspeed_bench(bool *p_open);
    void imgui_audio(bool *p_open);
    void imgui_file_chooser_window(void);
    void imgui_vkbd(bool *p_open);
    void imgui_pezik_settings_window(bool *p_open);
    void imgui_rom_settings_window(bool *p_open);
    void imgui_autotype_window(bool *p_open);
    void imgui_memext_load_dialog(void);
    void imgui_memext_save_dialog(void);
    void imgui_memext_map_window(bool *p_open);
    void imgui_dsk_create_window(bool *p_open);
    void imgui_joy_setup_window(bool *p_open);
    void imgui_debugger_window(bool *p_open);
    void imgui_breakpoints_window(bool *p_open);
    void mhmap_window_render(bool *p_open);
    /**
     * @brief Render $vars panelu (smart BP user-defined proměnné, D.6.2).
     * @param p_open Pointer na bool řídící viditelnost.
     */
    void vars_window_render(bool *p_open);
    /**
     * @brief Render Watch panelu (user-defined paměťové hlídky, V1 Phase A).
     * @param p_open Pointer na bool řídící viditelnost.
     */
    void watch_window_render(bool *p_open);
    /**
     * @brief Render I/O Ports viewer panelu (D.7).
     * @param p_open Pointer na bool řídící viditelnost.
     */
    void io_window_render(bool *p_open);
    /**
     * @brief Render Symbol Browser panelu (D.8.6).
     * @param p_open Pointer na bool řídící viditelnost.
     */
    void sym_window_render(bool *p_open);
    /**
     * @brief Render Bookmarks panelu (pojmenované adresové záložky).
     * @param p_open Pointer na bool řídící viditelnost.
     */
    void bm_window_render(bool *p_open);
    /**
     * @brief Render Memory Map debug okna (banking + memext per 4 kB stránku).
     * @param p_open Pointer na bool řídící viditelnost.
     */
    void memmap_window_render(bool *p_open);
    /**
     * @brief Render CPU Registers okna (Variant B - samostatné plovoucí okno).
     * @param p_open Pointer na bool řídící viditelnost.
     */
    void cpu_window_render(bool *p_open);
    /**
     * @brief Render Stack Monitor okna (hex dump paměti kolem SP).
     * @param p_open Pointer na bool řídící viditelnost.
     */
    void stack_window_render(bool *p_open);
    /**
     * @brief Render Stack Regions okna (V8 - tabulka regionů 1 řádek per region).
     * @param p_open Pointer na bool řídící viditelnost.
     */
    void stack_regions_window_render(bool *p_open);
    /**
     * @brief Render Stack History okna (V9 - SP history sparkline resize s oknem).
     * @param p_open Pointer na bool řídící viditelnost.
     */
    void stack_history_window_render(bool *p_open);
    /**
     * @brief Render Callstack okna (mutant callstack Fáze 2C - shadow stack volání).
     * @param p_open Pointer na bool řídící viditelnost.
     */
    void callstack_window_render(bool *p_open);
    /**
     * @brief Render Events okna (eventlog ring viewer, Vlna 1 = Log tab).
     * @param p_open Pointer na bool řídící viditelnost.
     */
    void event_viewer_window_render(bool *p_open);
#ifdef __cplusplus
}
#endif

#endif /* IMGUI_WINDOWS_H */
