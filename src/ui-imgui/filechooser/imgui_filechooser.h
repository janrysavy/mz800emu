/**
 * @file imgui_filechooser.h
 * @brief Dialog pro výběr souboru nad ImGuiFileDialog (IGFD).
 *
 * Obsahuje backend pro baseui_filechooser a napojení všech IGFD dialogů
 * na paměť naposledy použitých adresářů (baseui_fchooser_lastdir.h):
 * výchozí adresář podle kategorie, skupina "Recent" a uživatelské záložky
 * v panelu Places.
 */

#ifndef IMGUI_FILECHOOSER_H
#define IMGUI_FILECHOOSER_H

#include <stdint.h>
#include <stdbool.h>
#include <glib.h>
#include "baseui/baseui_filechooser.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Otevře IGFD dialog pro požadavek z baseui_filechooser.
     *
     * Výchozí umístění určí podle fch->category (viz
     * imgui_filechooser_prepare_config()). Je-li už jiný dialog otevřený,
     * nastaví stav BASEUI_FCHOOSER_STATE_CLOSED_ERROR a zavolá callback.
     *
     * @param fch Požadavek; vlastní ho volající.
     */
    void imgui_filechooser_new(baseui_fchooser_t *fch);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include "libs/igfd/ImGuiFileDialog.h"

/**
 * @brief Připraví konfiguraci IGFD dialogu s pamětí posledního adresáře.
 *
 * Volá se těsně před ImGuiFileDialog::Instance()->OpenDialog(). Podle
 * config.path / config.fileName / config.filePathName (výchozí hodnoty
 * volajícího) a kategorie určí výchozí umístění
 * (baseui_fchooser_lastdir_resolve()): prázdná cesta nebo "." se nahradí
 * zapamatovaným adresářem kategorie.
 *
 * Zároveň si poznamená kategorii otevíraného dialogu; po jeho potvrzení
 * (IsOk) imgui_file_chooser_window() zapamatuje vybraný adresář. Dialog
 * otevřený bez této funkce se nezapamatuje.
 *
 * @param category Kategorie souboru.
 * @param config [in,out] Konfigurace dialogu.
 *
 * @note Thread-safe (volá se i z EMU vlákna přes baseui_filechooser).
 *       Je-li jiný dialog právě otevřený, poznámku o kategorii nemění
 *       (následné OpenDialog stejně nic neotevře).
 */
void imgui_filechooser_prepare_config(baseui_fchooser_category_t category, IGFD::FileDialogConfig &config);
#endif

#endif /* IMGUI_FILECHOOSER_H */
