/**
 * @file baseui_fchooser_lastdir.h
 * @brief Paměť naposledy použitých adresářů dialogu pro výběr souboru.
 *
 * Dialog si pro každou kategorii souborů (CMT, DSK, ROM, ...) pamatuje
 * adresář, ve kterém uživatel naposledy úspěšně vybral soubor, dále jeden
 * společný poslední adresář a seznam několika naposledy použitých adresářů
 * (nejnovější první). Vše se ukládá do INI emulátoru (sekce [FILECHOOSER]),
 * takže paměť přežije restart.
 *
 * Modul je čisté C nad GLib a nezávisí na ImGui. Napojení na ImGuiFileDialog
 * (panel Places, zachycení výsledku dialogu) je v
 * ui-imgui/filechooser/imgui_filechooser.cpp.
 *
 * Synchronizace: všechny funkce jsou thread-safe (interní GMutex), lze je
 * volat z UI i EMU vlákna.
 */

#ifndef BASEUI_FCHOOSER_LASTDIR_H
#define BASEUI_FCHOOSER_LASTDIR_H

#include <stdbool.h>
#include <glib.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Kategorie souborů, pro které se pamatuje vlastní adresář.
     *
     * Hodnoty jsou indexy do interní tabulky; každá kategorie kromě
     * GENERIC má v INI vlastní klíč "<jméno>_dir".
     */
    typedef enum baseui_fchooser_category_t
    {
        BASEUI_FCHOOSER_CAT_GENERIC = 0, /**< bez vlastní paměti, jen společný poslední adresář */
        BASEUI_FCHOOSER_CAT_CMT,         /**< obrazy pásky (MZF, MZT, WAV, ...), nahrávání WAV */
        BASEUI_FCHOOSER_CAT_MZF,         /**< jednotlivé MZF soubory (CMT hack, oprava velikosti MZF) */
        BASEUI_FCHOOSER_CAT_DSK,         /**< obrazy disket */
        BASEUI_FCHOOSER_CAT_QDISK,       /**< obrazy Quick Disku a virtuální QD adresář */
        BASEUI_FCHOOSER_CAT_RAMDISK,     /**< zálohy ramdisků a MemExt flash */
        BASEUI_FCHOOSER_CAT_MEMEXT,      /**< načtení / uložení obsahu MemExt */
        BASEUI_FCHOOSER_CAT_HDD,         /**< obrazy disku IDE8 */
        BASEUI_FCHOOSER_CAT_ROM,         /**< ROM soubory */
        BASEUI_FCHOOSER_CAT_SDCARD,      /**< kořen SD karty Unicard */
        BASEUI_FCHOOSER_CAT_PLOTTER,     /**< výstup plotteru */
        BASEUI_FCHOOSER_CAT_SNAPSHOT,    /**< snapshoty */
        BASEUI_FCHOOSER_CAT_VIDEO,       /**< video záznam */
        BASEUI_FCHOOSER_CAT_DBG_MEMORY,  /**< debugger: načtení / uložení / porovnání paměti */
        BASEUI_FCHOOSER_CAT_DBG_SYMBOLS, /**< debugger: symboly */
        BASEUI_FCHOOSER_CAT_DBG_LISTS,   /**< debugger: seznamy breakpointů, záložek, watch, proměnných */
        BASEUI_FCHOOSER_CAT_DBG_EXPORT,  /**< debugger: exporty (disassembler, profiler, PSG, trace, CDL) */
        BASEUI_FCHOOSER_CAT_COUNT        /**< počet kategorií, není platná kategorie */
    } baseui_fchooser_category_t;

/** @brief Maximální počet adresářů v seznamu naposledy použitých. */
#define BASEUI_FCHOOSER_RECENT_MAX 10

    /**
     * @brief Zaregistruje sekci [FILECHOOSER] v INI a načte uložené hodnoty.
     *
     * @pre Volat jednou z cfgmain_init(), po vytvoření g_cfgmain.
     * @post Paměť obsahuje hodnoty z INI; při ukládání INI se zapíše aktuální
     *       stav paměti (save callbacky elementů).
     */
    void baseui_fchooser_lastdir_config_init(void);

    /**
     * @brief Smaže celou paměť (adresáře, seznam posledních, záložky).
     *
     * Určeno pro testy a pro inicializaci. Konfiguraci v INI nemění.
     */
    void baseui_fchooser_lastdir_reset(void);

    /**
     * @brief Vrátí jméno kategorie (základ INI klíče), např. "cmt".
     * @param category Kategorie.
     * @return Statický řetězec; pro neplatnou kategorii "generic".
     */
    const char *baseui_fchooser_lastdir_category_name(baseui_fchooser_category_t category);

    /**
     * @brief Zapamatuje si adresář úspěšně dokončeného dialogu.
     *
     * Nastaví adresář kategorie i společný poslední adresář a přesune
     * adresář na začátek seznamu posledních (bez duplicit, nejvýše
     * @ref BASEUI_FCHOOSER_RECENT_MAX položek). Koncový oddělovač se odstraní
     * (kromě kořene, např. "C:\" nebo "/").
     *
     * @param category Kategorie dialogu; GENERIC mění jen společné hodnoty.
     * @param dirpath Absolutní cesta k adresáři. NULL, prázdný řetězec a "."
     *        se ignorují.
     */
    void baseui_fchooser_lastdir_remember(baseui_fchooser_category_t category, const char *dirpath);

    /**
     * @brief Vrátí zapamatovaný adresář pro kategorii.
     *
     * Pořadí: adresář kategorie, adresář náhradní kategorie (CMT <-> MZF),
     * společný poslední adresář. Použije se první, který na disku existuje.
     *
     * @param category Kategorie dialogu.
     * @return Nově alokovaná cesta (uvolnit g_free()), nebo NULL, pokud
     *         žádný zapamatovaný adresář neexistuje.
     */
    char *baseui_fchooser_lastdir_get(baseui_fchooser_category_t category);

    /**
     * @brief Určí výchozí umístění dialogu z požadavku volajícího a paměti.
     *
     * Pravidla (první platné vyhrává):
     *  1. @p filePathName s existujícím adresářem -> beze změny (volající
     *     ví, který soubor nabídnout, např. právě připojený disk).
     *  2. @p path, pokud není prázdná ani "." a je to existující adresář;
     *     je-li to existující soubor, použije se jeho adresář a jeho jméno
     *     jako výchozí název souboru (není-li zadán jiný).
     *  3. zapamatovaný adresář (baseui_fchooser_lastdir_get()).
     *  4. ".".
     * Je-li @p filePathName jen jméno bez adresáře (např. "newfile.wav"),
     * nebo jeho adresář neexistuje, použije se jeho jméno jako výchozí název
     * souboru a adresář se určí podle bodů 2-4.
     *
     * @param category Kategorie dialogu.
     * @param path Výchozí adresář od volajícího, nebo NULL.
     * @param fileName Výchozí název souboru od volajícího, nebo NULL.
     * @param filePathName Plná cesta k souboru od volajícího, nebo NULL.
     * @param out_path [out] Adresář, nebo NULL, pokud se použije
     *        @p out_filePathName. Uvolnit g_free().
     * @param out_fileName [out] Výchozí název souboru, nebo NULL. Uvolnit g_free().
     * @param out_filePathName [out] Plná cesta k souboru, nebo NULL. Uvolnit g_free().
     *
     * @post Právě jedno z *out_path a *out_filePathName je nenulové.
     */
    void baseui_fchooser_lastdir_resolve(baseui_fchooser_category_t category,
                                         const char *path, const char *fileName, const char *filePathName,
                                         char **out_path, char **out_fileName, char **out_filePathName);

    /**
     * @brief Vrátí kopii seznamu naposledy použitých adresářů.
     * @return NULL-terminované pole (nejnovější první), uvolnit g_strfreev().
     *         Nikdy NULL; prázdný seznam = pole s jediným NULL.
     */
    char **baseui_fchooser_lastdir_get_recent(void);

    /**
     * @brief Vrátí uložené záložky dialogu (výstup IGFD SerializePlaces()).
     * @return Nově alokovaný řetězec (uvolnit g_free()), nikdy NULL.
     */
    char *baseui_fchooser_lastdir_get_bookmarks(void);

    /**
     * @brief Uloží záložky dialogu (výstup IGFD SerializePlaces()).
     * @param serialized Serializované záložky; NULL = prázdné.
     */
    void baseui_fchooser_lastdir_set_bookmarks(const char *serialized);

#ifdef __cplusplus
}
#endif

#endif /* BASEUI_FCHOOSER_LASTDIR_H */
