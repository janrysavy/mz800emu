/**
 * @file   screenshot_series.h
 * @brief  Série screenshotů: PNG každého (změněného) emulovaného snímku.
 *
 * Slouží k hledání chyb v animacích. Na konci každého emulovaného snímku
 * (screenshot_series_on_screen_done(), emu vlákno) se zkopíruje INDEX8
 * framebuffer; zapisovací vlákno ho vykreslí jádrem screenshotu (oblast,
 * měřítko, ořez - screenshot.h), a když se výsledek liší od posledního
 * uloženého (nebo se ukládá každý snímek), zapíše `frame_NNNNNNN.png`
 * a řádek do `index.csv`.
 *
 * Číslo NNNNNNN = pořadí emulovaného snímku od startu série (0 = první);
 * mezery v číslování znamenají nezměněné snímky. `index.csv` má sloupce
 * `frame,file,gdg_screen` (gdg_screen = absolutní počítadlo snímků GDG).
 *
 * Když zapisovací vlákno nestíhá, emu vlákno na konci snímku počká (zpětný
 * tlak), žádný snímek se neztratí - emulace se jen zpomalí.
 *
 * @par Vlákna
 * screenshot_series_start/stop/get_status smí volat libovolné vlákno
 * (interní mutex); screenshot_series_on_screen_done jen emu vlákno;
 * screenshot_series_exit při ukončení emulátoru.
 *
 * @par Licence: GPLv3
 */

#ifndef SCREENSHOT_SERIES_H
#define SCREENSHOT_SERIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "screenshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Výchozí pojistka počtu zapsaných souborů jedné série. */
#define SCREENSHOT_SERIES_MAX_FILES_DEFAULT 10000u
/** @brief Velikost bufferů cest (vč. NUL). */
#define SCREENSHOT_SERIES_PATH_MAX 1024
/** @brief Velikost textových bufferů (vč. NUL). */
#define SCREENSHOT_SERIES_TEXT_MAX 256

/** @brief Parametry startu série. */
typedef struct st_SCREENSHOT_SERIES_PARAMS {
    char dir[SCREENSHOT_SERIES_PATH_MAX]; /**< Adresář série; "" = nový podadresář výstupní složky screenshotů. */
    st_SCREENSHOT_OPTS opts;              /**< Oblast, měřítko, ořez (jako jeden screenshot). */
    bool only_changed;                    /**< true = uložit jen snímky odlišné od posledního uloženého. */
    uint64_t frames;                      /**< Počet sledovaných emulovaných snímků, pak automatický stop (0 = do stop). */
    unsigned max_files;                   /**< Pojistka počtu souborů (0 = SCREENSHOT_SERIES_MAX_FILES_DEFAULT). */
} st_SCREENSHOT_SERIES_PARAMS;

/** @brief Stav série (běžící nebo poslední dokončené). */
typedef struct st_SCREENSHOT_SERIES_STATUS {
    bool active;          /**< Série snímá (emu vlákno předává snímky). */
    bool writing;         /**< Zapisovací vlákno ještě zapisuje (i po stop). */
    uint64_t frames_seen; /**< Sledované emulované snímky. */
    uint64_t files;       /**< Zapsané PNG. */
    uint64_t unchanged;   /**< Snímky přeskočené jako nezměněné. */
    char dir[SCREENSHOT_SERIES_PATH_MAX];         /**< Adresář série ("" = žádná dosud). */
    char stop_reason[SCREENSHOT_SERIES_TEXT_MAX]; /**< Anglicky: proč série skončila ("" = běží / nikdy). */
    char last_error[SCREENSHOT_SERIES_TEXT_MAX];  /**< Anglicky: poslední chyba ("" = žádná). */
} st_SCREENSHOT_SERIES_STATUS;

/**
 * @brief Nastaví výchozí parametry: celý obraz, měřítko native, jen změněné, bez limitu snímků.
 * @param p Parametry (nesmí být NULL).
 * @note Inline, aby ji šlo použít i bez emu core (MCP dispatch test).
 */
static inline void screenshot_series_params_init(st_SCREENSHOT_SERIES_PARAMS *p)
{
    memset(p, 0, sizeof(*p));
    screenshot_opts_init(&p->opts);
    p->opts.scale_mode = SCREENSHOT_SCALE_NATIVE;
    p->only_changed = true;
}

/**
 * @brief Spustí sérii: vytvoří adresář, index.csv a zapisovací vlákno.
 *
 * První snímek se zachytí na nejbližším konci emulovaného snímku. Předchozí
 * dokončená série se nejdřív uklidí (join vlákna).
 *
 * @param p        Parametry.
 * @param err      Buffer na anglický popis chyby, nebo NULL.
 * @param err_size Velikost `err`.
 * @return true = série běží; false = už běží jiná, nelze vytvořit adresář
 *         nebo index.csv (popis v `err`).
 */
bool screenshot_series_start(const st_SCREENSHOT_SERIES_PARAMS *p, char *err, size_t err_size);

/**
 * @brief Ukončí snímání; zapisovací vlákno dopíše frontu a skončí samo.
 * @return true = série běžela; false = nic neběželo.
 * @post Stav `active` = false, `writing` zůstane true, dokud se nedopíše.
 */
bool screenshot_series_stop(void);

/**
 * @brief Zkopíruje stav série.
 * @param out Výstup.
 */
void screenshot_series_get_status(st_SCREENSHOT_SERIES_STATUS *out);

/**
 * @brief Háček konce emulovaného snímku (emu vlákno).
 *
 * Nečinný stav = jedno atomické čtení. Jinak zkopíruje framebuffer do
 * fronty (úplně stejný snímek jako předchozí přeskočí rovnou), hlídá limit
 * `frames` a při přeplněné frontě čeká na zapisovací vlákno.
 *
 * @pre g_framebuffer.pixels obsahuje dokončený snímek (volá se ze stejného
 *      místa jako videorec_on_screen_done()).
 */
void screenshot_series_on_screen_done(void);

/**
 * @brief Ukončení emulátoru: zastaví sérii a počká na dopsání (join).
 */
void screenshot_series_exit(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREENSHOT_SERIES_H */
