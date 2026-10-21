/**
 * @file   cputrack.h
 * @brief  CPU Tracking Log - jeden event per vykonanou Z80 instrukci.
 *
 * Subsystém trace-suite, který loguje každou dokončenou CPU instrukci
 * jako 12-byte fixed záznam:
 *
 * | Offset | Velikost | Pole                                          |
 * |--------|----------|------------------------------------------------|
 * | 0      | 2 B      | regPC (uint16_t LE)                            |
 * | 2      | 1 B      | insn_length (1-4)                              |
 * | 3      | 4 B      | insn_bytes (jen prvních insn_length valid)     |
 * | 7      | 4 B      | wait_clk (uint32_t LE) - WAIT T-states navíc   |
 * | 11     | 1 B      | t_std - standardní T-stavy instrukce (v2)      |
 *
 * Formát v2 (@ref CPUTRACK_RECORD_VERSION, `event_record_version` v
 * manifestu): bajt 11 = standardní délka instrukce v T-stavech (bez WAIT),
 * u sbaleného záznamu (HALT / smyčka na sebe) 0, protože @c wait_clk nese
 * celý čas smyčky. Ve v1 byl bajt 11 rezerva (vždy 0). Formát v2 navíc
 * zapisuje přijetí přerušení jako záznam s `insn_length` 0 (regPC =
 * adresa obsluhy, bajt 11 = T-stavy přijetí), viz
 * @ref cputrack_on_interrupt_accept.
 *
 * @c wait_clk == 0 => běžná instrukce (žádný WAIT).
 * @c wait_clk == 0xFFFFFFFF => "saturated" (např. dlouhá HALT smyčka).
 *
 * Hlavička recordingu (RAM dump + initial regs) se vytvoří v
 * @ref cputrack_start(). Hlavičkové binární soubory:
 *   `<dir>/<name>_initial_ram.bin`     - 64 KB RAM
 *   `<dir>/<name>_initial_vram.bin`    - 32 KB VRAM (jen MZ-800)
 *   `<dir>/<name>_initial_cgram.bin`   - 4 KB CG-RAM (jen MZ-1500)
 *   `<dir>/<name>_initial_memext.bin`  - 512 KB Memext (pokud připojeno)
 *   `<dir>/<name>_initial_rdN.bin`     - per-Ramdisk (pokud aktivní)
 *
 * Per-event záznamy: viz @ref tlog_writer chunk soubory.
 *
 * @section halt_collapse HALT a self-loop collapse
 *
 * HALT, JP $, JR $-2, CALL na sebe (= libovolná instrukce která zanechá
 * PC na stejné adrese a opakuje se) se sbaluje do JEDNOHO eventu s
 * @c wait_clk = celkový čas strávený ve smyčce (do exit přes IRQ /
 * state change). Detection: po @c z80_step() pokud nový PC == prev_PC,
 * suppress emit a akumuluj T-states. Při exit (PC změněno, IRQ accept)
 * emit collapsed event.
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#ifndef CPUTRACK_H
#define CPUTRACK_H

/** @brief Verze formátu záznamu cputrack (2 = bajt 11 nese standardní T-stavy). */
#define CPUTRACK_RECORD_VERSION 2

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "tlog_common.h"

/**
 * @brief Konfigurace cputrack subsystému.
 *
 * Drží uživatelské nastavení (mode, dir, name, limity). Ekvivalent
 * mhmap polí v g_debugger.
 *
 * @section range_scope Range-scoped tracking (PC filtr)
 *
 * @c pc_range_lo / @c pc_range_hi omezují záznam jen na instrukce, jejichž
 * adresa (PC před vykonáním) leží v intervalu @c [lo, hi] (oboustranně
 * uzavřeném). Výchozí hodnota @c lo=0, @c hi=0xFFFF pokrývá celý adresový
 * prostor, tj. žádný filtr. Vyhodnocení probíhá v hot-path hooku
 * (@ref mzarch_main_emulator_run) jako dvě porovnání na registrech UVNITŘ
 * již existujícího @c TEST_TRACE_CPUTRACK_ACTIVE bloku - při neaktivním
 * subsystému se neprovede žádné porovnání navíc (zero impact v OFF stavu).
 *
 * Typické použití: TPA-only metodika MZ DOS (@c lo=0x0100, @c hi=0xDBFF) -
 * záznam jen kódu transient-program-area, bez balastu z BDOS/BIOS.
 */
typedef struct st_CPUTRACK_CONFIG
{
    en_TLOG_MODE mode;            /**< Off / WithWindow / Always */
    char *dir;                    /**< Cílový adresář (g_strdup) */
    char *name;                   /**< Basename (g_strdup) */
    unsigned chunk_mb;            /**< Per-chunk velikost v MB (0 = default) */
    unsigned max_total_mb;        /**< Max total v MB (0 = unlimited) */
    unsigned save_on_exit;        /**< Při ukončení emu vyflushnout zbytek (default 1) */
    uint16_t pc_range_lo;         /**< Dolní mez PC pro range-scope filtr (vč.) */
    uint16_t pc_range_hi;         /**< Horní mez PC pro range-scope filtr (vč.) */
} st_CPUTRACK_CONFIG;

extern st_CPUTRACK_CONFIG g_cputrack_config;

/**
 * @brief Inicializace cputrack modulu.
 *
 * Volá se z `debugger_init()` po `cfgmain` registraci. Zaregistruje
 * konfigurační klíče (`cputrack_mode`, `cputrack_dir`, ...).
 */
void cputrack_init ( void );

/**
 * @brief Aplikace CLI options (--cputrack-*).
 *
 * Přepíše hodnoty z INI configu hodnotami z `--cputrack-mode`,
 * `--cputrack-dir`, `--cputrack-name` atd.
 */
void cputrack_apply_cli_options ( void );

/**
 * @brief Živá aplikace range-scope mezí @c pc_range_lo / @c pc_range_hi.
 *
 * Přečte aktuální hodnoty elementů @c TRACE_CPUTRACK/pc_range_lo a
 * @c .../pc_range_hi z @c g_cfgmain (= to, co právě zapsal MCP
 * @c settings_set přes @ref cfgelement_set_unsigned_value) a propíše je
 * do @ref g_cputrack_config (ořez na uint16_t). Hot-path filtr v
 * @ref cputrack_hot_path_hook čte přímo @c g_cputrack_config, takže nové
 * meze se uplatní při dalším záznamu bez restartu emulátoru.
 *
 * @pre Volat z emulátorového vlákna (typicky z dbgapi SETTINGS_SET
 *      handleru), aby zápis do @c g_cputrack_config byl thread-safe vůči
 *      hot-path čtení.
 * @note Pokud modul @c TRACE_CPUTRACK není zaregistrovaný, je no-op.
 */
void cputrack_apply_pc_range_live ( void );

/**
 * @brief Test, zda je cputrack aktivně recordující.
 *
 * Logika paralelní k @c TEST_DEBUGGER_MHMAP_ACTIVE.
 */
extern int g_cputrack_active;

#define TEST_TRACE_CPUTRACK_ACTIVE (g_cputrack_active)

/**
 * @brief Triggernout přepočet @c g_cputrack_active z mode + debugger state.
 *
 * Volá se při změně cputrack_mode (UI/CLI) a při změně debugger_active
 * (debug okno open/close).
 *
 * @post Maska háčků po instrukci (g_mzarch_post_insn_hooks) odpovídá
 *       novému stavu @c g_cputrack_active.
 */
void cputrack_recompute_active ( int debugger_active );

/**
 * @brief Spustit recording (alokovat writer, zapsat header + RAM dump).
 *
 * Volá se interně při přechodu z neaktivního na aktivní stav. Idempotentní.
 *
 * @return 0 OK, -1 error.
 */
int cputrack_start ( void );

/**
 * @brief Zastavit recording (flushnout buffer, zavřít writer).
 *
 * Volá se interně při přechodu z aktivního na neaktivní stav. Idempotentní.
 */
void cputrack_stop ( void );

/**
 * @brief Hot-path hook - emit eventu za právě dokončenou instrukci.
 *
 * Volat z `mzarch_main_emulator_run()` po `z80_step()` POKUD je
 * cputrack aktivní (caller checkuje TEST_TRACE_CPUTRACK_ACTIVE).
 *
 * Implementuje HALT/self-loop collapse:
 *  - Pokud nový PC == prev_PC: suppress emit, akumulovat tstates do
 *    pending_wait_clk
 *  - Pokud změna PC: pokud byly pending T-states, emit s wait_clk=accumulated;
 *    jinak emit s wait_clk=0
 *
 * @param pc                Aktuální PC (po z80_step, = adresa vykonané instr
 *                          BEFORE step, tj. caller drží před voláním).
 *                          Pozn.: caller předá `instruction_addr` (= PC pred step).
 * @param insn_tstates      Počet T-states které vykonaná instr trvala.
 * @param wait_extra_tstates Extra WAIT cycles (mimo standardní instr délku).
 *                          0 = žádný WAIT.
 */
void cputrack_on_instruction_complete ( uint16_t pc,
                                        uint8_t insn_tstates,
                                        uint32_t wait_extra_tstates );

/**
 * @brief Cold-path obal hot-path hooku (range-check + výpočet + emit).
 *
 * @section hot_path_compactness Důvod existence
 *
 * Hot per-instrukční smyčka @ref mzarch_main_emulator_run volá tuto funkci
 * jen v aktivním stavu, uvnitř @c if(TEST_TRACE_CPUTRACK_ACTIVE). Veškerá
 * práce range-scope filtru (porovnání @c pc_range_lo / @c pc_range_hi),
 * odečet WAIT cyklů a volání @ref cputrack_on_instruction_complete je
 * přesunuta SEM, do samostatné funkce s atributem @c noinline. Tím zůstane
 * tělo hot smyčky kompaktní (menší code footprint -> stabilnější instrukční
 * cache / code-layout) a přidaný range-scope kód žije v této "studené"
 * funkci, do které se v OFF stavu (g_cputrack_active==0) vůbec nezavolá.
 *
 * Funkce NIKDY nesmí být inlinovaná do mzarch_main - to je celý smysl
 * (jinak by se přidaná práce a zarovnání vrátily zpět do hot smyčky).
 *
 * @param pc                Adresa právě vykonané instrukce (PC před stepem).
 * @param insn_tstates      Celkový počet T-states instrukce vč. WAIT.
 * @param wait_extra_tstates Extra WAIT cykly (mimo standardní délku); 0 = žádný.
 *
 * @pre Volat jen když @c TEST_TRACE_CPUTRACK_ACTIVE (caller gatuje).
 */
void cputrack_hot_path_hook ( uint16_t pc,
                              uint32_t insn_tstates,
                              uint32_t wait_extra_tstates );

/**
 * @brief Záznam přijetí přerušení (formát v2: insn_length 0).
 *
 * Volá cesta přijetí přerušení v mzarch (ne instrukční smyčka) po
 * úspěšném z80_process_interrupt(), jen při aktivním cputrack. Nejdřív
 * zapíše rozpracovaný sbalený záznam (HALT nebo smyčka na sebe, kterou
 * přerušení ukončilo), pak záznam přijetí: `regPC` = adresa obsluhy (PC
 * po přijetí), `insn_length` = 0, bajty instrukce 0, `wait_clk` = 0,
 * `t_std` = T-stavy přijetí. Součet `t_std + wait_clk` všech záznamů tak
 * dává uplynulý čas CPU včetně přijetí přerušení.
 *
 * @param isr_pc       PC po přijetí (adresa obsluhy přerušení).
 * @param ack_tstates  T-stavy přijetí (návrat z80_process_interrupt).
 *
 * @pre Volat jen když @c TEST_TRACE_CPUTRACK_ACTIVE (caller gatuje).
 * @note Filtr PC (`pc_range_lo/hi`) se uplatní na @p isr_pc stejně jako
 *       na instrukce.
 */
void cputrack_on_interrupt_accept ( uint16_t isr_pc, uint32_t ack_tstates );

/**
 * @brief Reset stavu collapse logiky (volat při emu reset).
 */
void cputrack_reset_collapse_state ( void );

/**
 * @brief Force flush + finalize (volá se z debugger_exit pokud save_on_exit=1).
 */
void cputrack_finalize ( void );

/**
 * @brief Uzavřít (uložit) aktuální recording segment, volitelně přesměrovat.
 *
 * Runtime analogie @c mhmap_export pro streamovaný tlog. Dosud bylo finalizace
 * možné jen přes exit-gate (@c save_on_exit). Tato funkce umožní finalizovat
 * segment za běhu přes MCP (@c DBGAPI_CMD_TRACE_SAVE).
 *
 * Chování:
 *  - Pokud běží recording, uzavře aktuální segment (flush + close => zapíše
 *    poslední částečný chunk a finální manifest <name>.json).
 *  - Pokud je @p path zadané, přesměruje @c dir/name z @p path (split na
 *    dirname + basename, přípona se nechá jako prefix) PŘED restartem.
 *  - Pokud recording běžel, otevře čerstvý segment (pokračování záznamu);
 *    při zadaném @p path už do nové lokace.
 *
 * Pozn.: již zapsané chunk soubory se NEpřejmenovávají - tlog writer váže
 * jména v okamžiku @ref cputrack_start. @p path tedy směruje NÁSLEDNÝ segment.
 *
 * @param path  Cílová cesta dalšího segmentu, nebo NULL pro ponechání
 *              stávajícího @c dir/name (= jen flush+restart na stejné místo).
 * @return 0 OK, -1 chyba (např. restart writeru selhal).
 */
int cputrack_save_segment ( const char *path );

/**
 * @brief Test, zda byl recording zastaven kvůli max_total_mb limitu.
 *
 * Public wrapper nad @ref tlog_writer_is_truncated() pro interní
 * @c s_writer instanci tohoto subsystému. Použití typ. v UI status
 * indicators (např. titulek SDL okna).
 *
 * @return 1 = truncated (recording stopnut), 0 = stále aktivní recording
 *         nebo neaktivní subsystém.
 */
int cputrack_is_truncated ( void );

/**
 * @brief Writer kanálu cputrack pro čtení proudu záznamů (trace_read).
 *
 * Vrací writer i po stopu (popis posledního uzavřeného segmentu), viz
 * tlog_writer_read_snapshot() a devdoc/trace/stream-read.md.
 *
 * @return Ukazatel na statický writer modulu (nikdy NULL).
 * @pre Snímek writeru pořizovat jen z emu vlákna; tlog_writer_segment()
 *      z libovolného vlákna.
 */
const st_TLOG_WRITER *cputrack_get_writer ( void );

#ifdef __cplusplus
}
#endif

#endif /* CPUTRACK_H */
