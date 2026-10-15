/**
 * @file   screenshot_config.h
 * @brief  Nastavení screenshotu z GUI a INI modul `[SCREENSHOT]`.
 *
 * GUI (Tools -> Save Screenshot, Alt+F) ukládá PNG s naposledy zvolenou
 * oblastí a měřítkem do výstupního adresáře se jménem podle času. MCP tato
 * nastavení nepoužívá (volby dostává v requestu).
 *
 * INI klíče sekce `[SCREENSHOT]`:
 * - `area`       - "full" (výchozí) nebo "canvas",
 * - `scale`      - "native" nebo "1".."4" (výchozí "2" = okno na 100 %);
 *                  "framebuffer" se z INI nepřijímá (nahradí se výchozím),
 * - `output_dir` - adresář; prázdný = `<home_dir>/screenshots`.
 * - `series_only_changed` - série snímků z GUI ukládá jen změněné snímky
 *                  (výchozí 1).
 *
 * @par Vlákna
 * g_screenshot_settings čte a mění jen UI vlákno (a cfgmain při startu
 * a ukládání INI).
 *
 * @par Licence: GPLv3
 */

#ifndef SCREENSHOT_CONFIG_H
#define SCREENSHOT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "screenshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Nastavení screenshotu z GUI. */
typedef struct st_SCREENSHOT_SETTINGS {
    en_SCREENSHOT_AREA area;             /**< Oblast obrazu. */
    en_SCREENSHOT_SCALE_MODE scale_mode; /**< SCALE_NATIVE nebo SCALE_WINDOW (ne FRAMEBUFFER). */
    unsigned window_scale;               /**< Měřítko okna 1..4 (při SCALE_WINDOW). */
    char output_dir[1024];               /**< Výstupní adresář; prázdný = výchozí. */
    bool series_only_changed;            /**< Série snímků z GUI: jen změněné snímky (jinak každý). */
} st_SCREENSHOT_SETTINGS;

/** @brief Nastavení screenshotu (UI vlákno). */
extern st_SCREENSHOT_SETTINGS g_screenshot_settings;

/**
 * @brief Zaregistruje INI modul `[SCREENSHOT]`, načte ho a propaguje do
 *        g_screenshot_settings.
 * @pre Volá cfgmain_init() po vytvoření g_cfgmain.
 */
void screenshot_config_init(void);

/**
 * @brief Převede nastavení na volby pro screenshot_encode_png() (bez ořezu).
 * @param s    Nastavení.
 * @param opts Výstup.
 */
void screenshot_settings_to_opts(const st_SCREENSHOT_SETTINGS *s, st_SCREENSHOT_OPTS *opts);

/**
 * @brief Výstupní adresář podle nastavení; vytvoří ho, pokud neexistuje.
 *
 * Prázdný output_dir = `<home_dir>/screenshots`, nejde-li vytvořit, adresář
 * Obrázky uživatele, jinak ".".
 *
 * @param out  Výstupní buffer.
 * @param size Velikost bufferu.
 * @return true = adresář existuje (případně byl vytvořen).
 */
bool screenshot_resolve_output_dir(char *out, size_t size);

/**
 * @brief Sestaví jméno nového souboru `<platforma>_RRRRMMDD_HHMMSS.png`
 *        v adresáři `dir`; existuje-li, připojí `_2`, `_3`, ...
 * @param dir  Adresář.
 * @param out  Výstupní buffer na celou cestu.
 * @param size Velikost bufferu.
 */
void screenshot_make_filename(const char *dir, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* SCREENSHOT_CONFIG_H */
