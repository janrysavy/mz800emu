/**
 * @file   marklog.h
 * @brief  Marker Log - STMT_MARK BP action zapsaná do binárního trace logu.
 *
 * 5. subsystém trace-suite (vedle `cputrack`, `iorqlog`, `intlog`, `hwlog`).
 * Loguje `mark "name"` action z bp_action.c jako 24 B záznamy do
 * `<dir>/<name>.NNN.bin`. Marker name registry (string -> uint16 id) se
 * dumpuje do manifestu <name>.json `subsys_header.markers`, což analyzátory umožní
 * dekódovat id zpět na človek-čitelné jméno.
 *
 * Designové rozhodnutí: Mark name jsou parse-time konstantní string literály
 * (`bp_action.c:934-941` používá `read_string_literal()`). Registr se proto
 * naplňuje při BP parse a hot-path fire jen volá @ref marklog_record() s
 * pre-resolved marker_id - žádné string operace.
 *
 * Per-event záznam (24 B, alignment 8 B - konzistentní s intlog):
 *
 * | Offset | Velikost | Pole                                    |
 * |--------|----------|------------------------------------------|
 * | 0      | 8 B      | pxclk_total                              |
 * | 8      | 4 B      | screens_total                            |
 * | 12     | 4 B      | pxclk_in_screen                          |
 * | 16     | 2 B      | marker_id (uint16, 0xFFFF = invalid)     |
 * | 18     | 6 B      | reserved (padding, vyplněno nulami)      |
 *
 * @note Registr je perzistentní napříč start/stop recordingu (= ID stabilní
 *       v rámci jednoho běhu emulátoru, dumpuje se do manifestu <name>.json při každém
 *       @ref marklog_start). Reset registru až ve @ref marklog_finalize().
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#ifndef MARKLOG_H
#define MARKLOG_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "tlog_common.h"

/**
 * @brief Maximální délka marker name včetně NUL terminatoru.
 *
 * Delší jména jsou při registraci zkrácena + stderr warning.
 */
#define MARKLOG_NAME_MAX 64u

/**
 * @brief Rezervovaná hodnota marker_id signalizující "neplatný" záznam.
 *
 * Vrací @ref marklog_register() při overflow (registr plný, > 65535
 * unikátních jmen) nebo selhání alokace. STMT_MARK fire path s tímto
 * id volá no-op pro marklog (stdout část funguje nezávisle).
 */
#define MARKLOG_INVALID_ID 0xFFFFu

/**
 * @brief Konfigurace marklog subsystému.
 *
 * Načítá se z `[TRACE_MARKLOG]` cfg sekce + CLI options.
 */
typedef struct st_MARKLOG_CONFIG
{
    en_TLOG_MODE mode;          /**< OFF / WITH_WINDOW / ALWAYS (default OFF) */
    char *dir;                  /**< Cílový adresář (vlastní config, g_strdup) */
    char *name;                 /**< Basename recordingu (vlastní config, g_strdup) */
    unsigned chunk_mb;          /**< Velikost chunku v MB (default 64) */
    unsigned max_total_mb;      /**< Hard limit (0 = unlimited) */
    unsigned save_on_exit;      /**< 1 = flush + close při exit; 0 = drop */
    unsigned stdout_enabled;    /**< 1 = STMT_MARK stále printuje [BP-MARK] na stdout
                                     (back-compat, nezávislé na marklog mode) */
} st_MARKLOG_CONFIG;

extern st_MARKLOG_CONFIG g_marklog_config;
extern int g_marklog_active;

/**
 * @brief Test, zda je marklog writer aktivní (= ready to record).
 *
 * Inline-friendly check pro hot path. STMT_MARK consumer to volá
 * implicitně přes @ref marklog_record(), tento makro slouží primárně
 * pro UI status indicators.
 */
#define TEST_TRACE_MARKLOG_ACTIVE (g_marklog_active)

/**
 * @brief Inicializace cfg modulu + name registry.
 *
 * Volat z `debugger_init()` po `intlog_init`. Registruje `[TRACE_MARKLOG]`
 * sekci do `g_cfgmain`, naplní @ref g_marklog_config defaulty, alokuje
 * marker registry GHashTable/GPtrArray.
 *
 * @post @ref g_marklog_config naplněn, registr prázdný, writer NEběží.
 */
void marklog_init ( void );

/**
 * @brief Aplikuje CLI options přepsající cfg defaulty.
 *
 * Volat z `debugger_init()` po @ref marklog_init(). CLI flagy:
 *  - `--marklog-mode=off|window|always`
 *  - `--marklog-dir=<path>`
 *  - `--marklog-name=<basename>`
 *  - `--marklog-chunk-mb=<N>`
 *  - `--marklog-max-total-mb=<N>`
 *  - `--marklog-save-on-exit=on|off`
 *  - `--marklog-stdout=on|off`
 */
void marklog_apply_cli_options ( void );

/**
 * @brief Update active stav podle debugger state + cfg mode.
 *
 * Volat z `mzarch_platform_fn_debugger_state_changed()` per arch
 * (`mz800_main.c`, `mz700_main.c`, `mz1500_main.c`). Spouští/zastavuje
 * writer dle finálního active stavu.
 *
 * @param debugger_active  1 = debug okno otevřené, 0 = zavřené.
 */
void marklog_recompute_active ( int debugger_active );

/**
 * @brief Spustit recording (otevřít writer, dump registry do meta).
 *
 * @return 0 OK, -1 chyba (alloc / mkdir / cfg).
 * @post Při úspěchu writer aktivní, @ref TEST_TRACE_MARKLOG_ACTIVE platí.
 */
int marklog_start ( void );

/**
 * @brief Zastavit recording (flush + close + finalize manifest <name>.json).
 *
 * Idempotentní - druhé volání bez startu = no-op.
 */
void marklog_stop ( void );

/**
 * @brief Cleanup + uvolnění registru.
 *
 * Volat z `debugger_exit()`. Pokud běží recording a
 * `save_on_exit == 1`, flushne před close. Uvolní marker registry +
 * config stringy.
 */
void marklog_finalize ( void );

/**
 * @brief Test, zda byl writer zastaven kvůli `max_total_mb` limitu.
 *
 * Public wrapper nad @ref tlog_writer_is_truncated() pro interní
 * `s_writer` instanci.
 *
 * @return 1 = truncated (recording stopnut), 0 = stále aktivní nebo
 *         neaktivní subsystém.
 */
int marklog_is_truncated ( void );

/**
 * @brief Uložit aktuální segment, volitelně přesměrovat na @p path.
 *
 * Stejná sémantika jako iorqlog_save_segment():
 *  - @p path NULL nebo prázdná: zapíše rozpracovaný chunk a manifest,
 *    segment pokračuje (nic se nezahodí),
 *  - jinak uzavře segment, přesměruje dir/name na @p path a při aktivním
 *    nahrávání otevře nový segment.
 *
 * @param path Cílová cesta dalšího segmentu, nebo NULL (jen flush).
 * @return 0 OK, -1 chyba (restart writeru selhal).
 * @pre Emu vlákno (dbgapi trace lifecycle).
 */
int marklog_save_segment ( const char *path );

/**
 * @brief Zaregistrovat marker name -> uint16 id (parse-time API).
 *
 * Idempotentní per name - opakované volání se stejným `name` vrací
 * stejné id. Při overflow (> 65535 unikátních jmen) vrací
 * @ref MARKLOG_INVALID_ID + stderr warning. Při name delší než
 * @ref MARKLOG_NAME_MAX (vč. NUL) provádí truncate + stderr warning.
 *
 * Side effects:
 *  - Mutuje interní `name_to_id` hashtable a `id_to_name` array.
 *
 * Thread safety: NENÍ thread-safe. Volat jen z EMU vlákna (typ. parser
 * běží před spuštěním emu nebo pod debug pauzou).
 *
 * @param name  Marker name (NULL-terminated UTF-8). NULL = vrací
 *              MARKLOG_INVALID_ID bez warningu.
 * @return uint16 marker_id (0..65534), nebo @ref MARKLOG_INVALID_ID
 *         při chybě/overflow.
 */
uint16_t marklog_register ( const char *name );

/**
 * @brief Hot-path zápis 24B recordu pro daný marker.
 *
 * No-op pokud writer není aktivní (`s_writer_open == 0`) nebo
 * `marker_id == @ref MARKLOG_INVALID_ID`. Žádné string operace.
 *
 * Side effects:
 *  - Při overflow chunku trigger synchronní disk flush (přes tlog_common).
 *
 * @param marker_id  Pre-registered id z @ref marklog_register().
 */
void marklog_record ( uint16_t marker_id );

/**
 * @brief Vrátit registrované jméno pro daný marker_id.
 *
 * Pro debug výpisy a kontrolu z testů. Vrací NULL pokud id mimo
 * zaregistrovaný rozsah.
 *
 * @param marker_id  Id k vyhledání.
 * @return Vlastněný stringem registru (NEMĚN, NEUVOLŇUJ), nebo NULL.
 */
const char *marklog_get_name ( uint16_t marker_id );

/**
 * @brief Vrátit počet zaregistrovaných markerů.
 *
 * Primárně pro testy. Validní id rozsah = `[0 .. count-1]`.
 *
 * @return Počet zaregistrovaných jmen (0 .. 65535).
 */
unsigned marklog_registry_count ( void );

/**
 * @brief Writer kanálu marklog pro čtení proudu záznamů (trace_read).
 *
 * Vrací writer i po stopu (popis posledního uzavřeného segmentu), viz
 * tlog_writer_read_snapshot() a devdoc/trace/stream-read.md.
 *
 * @return Ukazatel na statický writer modulu (nikdy NULL).
 * @pre Snímek writeru pořizovat jen z emu vlákna; tlog_writer_segment()
 *      z libovolného vlákna.
 */
const st_TLOG_WRITER *marklog_get_writer ( void );

#ifdef __cplusplus
}
#endif

#endif /* MARKLOG_H */
