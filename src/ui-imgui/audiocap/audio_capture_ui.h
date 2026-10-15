/**
 * @file   audio_capture_ui.h
 * @brief  Zachycení zvuku z GUI: menu Tools -> Audio Capture, zkratka Alt+Q, dialog nastavení.
 *
 * Start a stop volají jádro audio_capture.h přímo (je bezpečné pro volání
 * z UI vlákna). Výběr souborů a výstupní složku bere z
 * g_audio_capture_settings (INI `[AUDIO_CAPTURE]`); každé zachycení založí
 * podsložku ve výstupní složce (výchozí `audio_captures` vedle emulátoru).
 *
 * @par Vlákna
 * Všechny funkce jen z UI vlákna.
 *
 * @par Licence: GPLv3
 */

#ifndef AUDIO_CAPTURE_UI_H
#define AUDIO_CAPTURE_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Spustí nebo ukončí zachycení zvuku.
 *
 * Start vezme výběr souborů z nastavení; chybu startu ohlásí notifikací,
 * dokončení ohlásí imgui_audio_capture_poll().
 */
void audio_capture_ui_toggle(void);

/** @brief Podmenu "Audio Capture" v menu Tools (start/stop, nastavení, otevření výstupní složky). */
void imgui_audio_capture_menu(void);

/**
 * @brief Zkratka Alt+Q (start / stop); volá se v Alt větvi globálních zkratek.
 */
void imgui_audio_capture_shortcuts(void);

/**
 * @brief Indikátor běžícího zachycení v levém horním rohu obrazu (pod indikátorem série snímků).
 *
 * Volá se hned po vykreslení obrazu emulátoru (ImGui::Image), obdélník
 * obrazu bere z GetItemRect*.
 */
void imgui_audio_capture_overlay(void);

/**
 * @brief Po konci zachycení spuštěného z GUI zobrazí notifikaci (složka, délka, chyba).
 *
 * Volá se jednou za snímek UI.
 */
void imgui_audio_capture_poll(void);

/**
 * @brief Dialog nastavení (výstupní složka, výběr souborů); zobrazí se po Tools -> Audio Capture -> Settings...
 *
 * Edituje kopii nastavení, OK ji zapíše do g_audio_capture_settings (uloží
 * se do INI při ukončení emulátoru), Cancel ji zahodí.
 */
void imgui_audio_capture_setup_dialog(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CAPTURE_UI_H */
