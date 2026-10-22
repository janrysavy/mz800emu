/**
 * @file snapshot_request.h
 * @brief Fronta požadavků na snapshot z GUI, vyřizuje je emu vlákno v pauze.
 *
 * GUI (Load Snapshot, Save Snapshot, Quick Load, Quick Save) dřív volalo
 * snapshot_load() / snapshot_save() přímo z UI vlákna. Pauza z menu ale
 * nezaručí, že emu vlákno stojí: s debuggerem je emu vlákno v paused smyčce
 * a dál obsluhuje frontu dbgapi (MCP, obnovu obrazu), bez debuggeru pauza
 * jen nastaví příznak a emu vlákno může být uprostřed snímku. Snapshot pak
 * četl nebo přepisoval stav stroje souběžně s emulací.
 *
 * Nově UI požadavek jen zařadí (snapshot_request_submit()) a vyřídí ho emu
 * vlákno v paused smyčce (snapshot_request_poll(), obě smyčky v mzarch.c,
 * tedy i v buildu bez debuggeru). Výsledek si UI vyzvedne přes
 * snapshot_request_take_result() a ukáže ho uživateli.
 *
 * Synchronizace: fronta požadavků i výsledků je pod vlastním mutexem,
 * počet čekajících požadavků je atomický (paused smyčka bez požadavku
 * provede jedno atomické čtení). Modul je bez lokalizačních maker (jádro).
 */

#ifndef SNAPSHOT_REQUEST_H
#define SNAPSHOT_REQUEST_H

#include "snapshot.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Délka bufferu cesty v požadavku a výsledku (včetně nuly). */
#define SNAPSHOT_REQUEST_PATH_MAX 1024

/** @brief Délka bufferu popisu snapshotu v požadavku (včetně nuly). */
#define SNAPSHOT_REQUEST_DESC_MAX 256

/**
 * @brief Druh požadavku na snapshot.
 */
typedef enum en_SNAPSHOT_REQ_KIND {
    SNAPSHOT_REQ_SAVE = 0,   /**< snapshot_save(path, description) */
    SNAPSHOT_REQ_LOAD,       /**< snapshot_load(path); při běžícím replay odmítnuto */
    SNAPSHOT_REQ_QUICKSAVE,  /**< snapshot_quicksave() podle g_snapshot_settings */
    SNAPSHOT_REQ_QUICKLOAD,  /**< snapshot_quickload(); při běžícím replay odmítnuto */
} en_SNAPSHOT_REQ_KIND;

/**
 * @brief Výsledek vyřízeného požadavku.
 *
 * @invariant `seq` odpovídá hodnotě, kterou vrátil snapshot_request_submit()
 *            pro tentýž požadavek; `path` je kopie cesty z požadavku
 *            (u Quick Save / Quick Load prázdná, cestu volí snapshot vrstva).
 */
typedef struct st_SNAPSHOT_REQ_RESULT {
    uint32_t             seq;                              /**< Pořadové číslo požadavku (od 1). */
    en_SNAPSHOT_REQ_KIND kind;                             /**< Druh požadavku. */
    en_SNAPSHOT_RESULT   result;                           /**< Výsledek operace. */
    char                 path[SNAPSHOT_REQUEST_PATH_MAX];  /**< Cesta z požadavku ("" u Quick). */
} st_SNAPSHOT_REQ_RESULT;

/**
 * @brief Zařadí požadavek na snapshot pro emu vlákno.
 *
 * @param kind        Druh požadavku.
 * @param path        Cesta k .mzs (SAVE, LOAD); u QUICKSAVE / QUICKLOAD se
 *                    ignoruje a smí být NULL. Zkopíruje se (oříznutí na
 *                    SNAPSHOT_REQUEST_PATH_MAX - 1 bajtů).
 * @param description Popis snapshotu (jen SAVE, smí být NULL = bez popisu).
 *                    Zkopíruje se.
 * @return Pořadové číslo požadavku (od 1); 0 při chybějící cestě u SAVE
 *         nebo LOAD (požadavek se nezařadí).
 *
 * @note Libovolné vlákno (typicky UI). Emulaci nepauzuje: volající zajistí
 *       pauzu, jinak požadavek čeká do příští pauzy.
 * @post Při nenulovém návratu požadavek čeká ve frontě, výsledek přijde
 *       přes snapshot_request_take_result().
 */
uint32_t snapshot_request_submit(en_SNAPSHOT_REQ_KIND kind, const char *path,
                                 const char *description);

/**
 * @brief Vyřídí všechny čekající požadavky (emu vlákno, paused smyčka).
 *
 * Bez čekajícího požadavku jedno atomické čtení. Požadavky se vyřizují
 * v pořadí zařazení; LOAD a QUICKLOAD při běžícím záznamu nebo přehrávání
 * vstupů vrátí SNAPSHOT_ERR_REPLAY_ACTIVE (snapshot by záznam rozbil).
 * Výsledek každého požadavku se zařadí do fronty výsledků.
 *
 * @pre Volá emu vlákno v pauze (snapshot vrstva jinak vrátí
 *      SNAPSHOT_ERR_NOT_PAUSED); mutex modulu nesmí volající držet.
 * @post Fronta požadavků byla v okamžiku kontroly prázdná.
 * @note Vedlejší efekty podle druhu: zápis souboru, načtení stavu stroje.
 */
void snapshot_request_poll(void);

/**
 * @brief Vyzvedne nejstarší výsledek vyřízeného požadavku.
 *
 * @param out Výstup (nesmí být NULL).
 * @return true = výsledek vyzvednut do `out`; false = žádný výsledek
 *         (`out` beze změny).
 * @note Libovolné vlákno (typicky UI každý snímek).
 */
bool snapshot_request_take_result(st_SNAPSHOT_REQ_RESULT *out);

/**
 * @brief Počet požadavků, které ještě nevyřídilo emu vlákno.
 * @return Počet čekajících požadavků (atomické čtení, libovolné vlákno).
 */
int snapshot_request_pending(void);

/**
 * @brief Zahodí čekající požadavky i nevyzvednuté výsledky.
 *
 * Pro ukončení emulátoru a testy; pořadové číslo se nenuluje.
 * @note Libovolné vlákno.
 */
void snapshot_request_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* SNAPSHOT_REQUEST_H */
