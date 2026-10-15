/**
 * @file   profiler_pc.h
 * @brief  Profiler podle PC: počet vykonání, T-stavy a WAIT pro každou adresu instrukce.
 *
 * Doplněk funkčního profileru (profiler.h, po funkcích přes callstack):
 * pro každou logickou adresu 0000h-FFFFh, na které začíná vykonaná
 * instrukce, sčítá počet vykonání, T-stavy instrukce (včetně čekacích
 * stavů) a zvlášť čekací stavy (WAIT od VRAM a PSG). Slouží k nalezení
 * míst, kde program tráví čas (optimalizace).
 *
 * Napojení: hlavní smyčka (mzarch.c) po každé instrukci testuje jedinou
 * podmínkou `g_cputrack_active | g_profiler_pc_active` a teprve uvnitř
 * volá háčky; v neaktivním stavu se tedy proti stavu bez profileru přidá
 * jen jedno čtení proměnné, žádná větev.
 *
 * Zvlášť se vedou (nejsou přičtené k žádné adrese):
 * - nečinný HALT: kroky, kdy CPU stojí v HALT (4 T každý); vykonání samotné
 *   instrukce HALT se počítá na její adrese,
 * - zbytek do uplynulého času (přijetí přerušení, ...): rozdíl mezi
 *   uplynulým emulovaným časem a součtem instrukcí a nečinného HALT.
 *
 * Uplynulý čas se sčítá z přírůstků `cpu->total_cycles` mezi dvěma po sobě
 * jdoucími instrukcemi. Přírůstek, který neodpovídá (menší než instrukce,
 * nebo o víc než PROFILER_PC_GAP_MAX větší), se bere jako nespojitost
 * (reset CPU, načtení snapshotu) a započte se jen instrukce.
 *
 * Nečinný HALT se pozná tak, že CPU byl po předchozí instrukci v HALT
 * a nová instrukce začíná na stejné adrese (jádro během HALT drží PC
 * na adrese instrukce HALT).
 *
 * Klíčem je logické PC (bez rozlišení bankingu, stejně jako funkční
 * profiler). Při ladění kódu ve více bankách se čísla stejné adresy sčítají.
 *
 * @par Vlákna
 * Data zapisuje emu vlákno (profiler_pc_on_instruction). Ostatní funkce
 * volat z emu vlákna (dbgapi handler), nebo když emu vlákno neběží.
 *
 * @par Licence: GPLv3
 */

#ifndef PROFILER_PC_H
#define PROFILER_PC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Profiler podle PC sbírá data (čte hlavní smyčka). */
extern int g_profiler_pc_active;

/** @brief Podmínka pro hlavní smyčku (sčítá se s TEST_TRACE_CPUTRACK_ACTIVE). */
#define TEST_PROFILER_PC_ACTIVE (g_profiler_pc_active)

/**
 * @brief Největší počet T-stavů mimo instrukce mezi dvěma instrukcemi, který se
 *        ještě bere jako souvislý běh (přijetí přerušení má desítky T).
 */
#define PROFILER_PC_GAP_MAX 1000u

/** @brief Řazení výsledků. */
typedef enum en_PROFILER_PC_SORT {
    PROFILER_PC_SORT_TSTATES = 0, /**< Podle T-stavů sestupně (výchozí). */
    PROFILER_PC_SORT_COUNT,       /**< Podle počtu vykonání sestupně. */
    PROFILER_PC_SORT_WAIT,        /**< Podle čekacích stavů sestupně. */
    PROFILER_PC_SORT_ADDR,        /**< Podle adresy vzestupně. */
} en_PROFILER_PC_SORT;

/** @brief Jeden řádek výsledku (adresa s aspoň jedním vykonáním). */
typedef struct st_PROFILER_PC_ROW {
    uint16_t pc;       /**< Adresa začátku instrukce. */
    uint64_t count;    /**< Počet vykonání. */
    uint64_t tstates;  /**< Součet T-stavů (včetně WAIT). */
    uint64_t wait;     /**< Součet čekacích stavů (WAIT). */
} st_PROFILER_PC_ROW;

/** @brief Souhrn měření. */
typedef struct st_PROFILER_PC_SUMMARY {
    bool active;               /**< Sbírá se. */
    uint64_t instructions;     /**< Vykonané instrukce celkem. */
    uint64_t tstates;          /**< T-stavy instrukcí celkem (včetně WAIT). */
    uint64_t wait;             /**< Čekací stavy celkem. */
    uint64_t halt_idle;        /**< T-stavy nečinného HALT. */
    uint64_t elapsed_tstates;  /**< Uplynulý emulovaný čas během sběru [T-stavy CPU]. */
    uint64_t other_tstates;    /**< elapsed - tstates - halt_idle (přijetí přerušení aj.); 0 když záporné. */
    unsigned unique_pcs;       /**< Počet adres s aspoň jedním vykonáním. */
} st_PROFILER_PC_SUMMARY;

/**
 * @brief Zapne nebo vypne sběr. Data se při vypnutí nemažou.
 * @param enable true = sbírat.
 * @pre Emu vlákno (čte `cpu->total_cycles`), nebo emu vlákno neběží.
 * @post Při zapnutí se začne měřit uplynulý čas od tohoto okamžiku.
 */
void profiler_pc_set_active(bool enable);

/**
 * @brief Vynuluje všechna data (i uplynulý čas); stav aktivity nemění.
 */
void profiler_pc_reset(void);

/**
 * @brief Háček po dokončení instrukce (volá hlavní smyčka jen při aktivním sběru).
 *
 * @param pc       Adresa začátku instrukce (instruction_addr).
 * @param tstates  T-stavy instrukce včetně WAIT (instruction_tstates).
 * @param wait     Čekací stavy instrukce (instruction_wait_tstates).
 * @pre Emu vlákno, g_profiler_pc_active != 0.
 */
void profiler_pc_on_instruction(uint16_t pc, uint32_t tstates, uint32_t wait);

/**
 * @brief Vrátí souhrn měření.
 * @param out Výstup.
 */
void profiler_pc_get_summary(st_PROFILER_PC_SUMMARY *out);

/**
 * @brief Vrátí nejvýše `max` řádků v rozsahu adres, seřazených podle `sort`.
 *
 * @param sort Řazení.
 * @param lo   Nejnižší adresa (včetně).
 * @param hi   Nejvyšší adresa (včetně); hi < lo = nic.
 * @param out  Výstupní pole (alespoň `max` prvků), nebo NULL = jen spočítat.
 * @param max  Kapacita `out`.
 * @return Počet adres s aspoň jedním vykonáním v rozsahu (může být > max;
 *         zapsáno je min(návrat, max)).
 */
size_t profiler_pc_get_rows(en_PROFILER_PC_SORT sort, uint16_t lo, uint16_t hi,
                            st_PROFILER_PC_ROW *out, size_t max);

/**
 * @brief Zapíše všechny adresy s aspoň jedním vykonáním do CSV.
 *
 * Sloupce `pc,count,tstates,wait,tstates_pct`, řazení podle `sort`, UTF-8,
 * konce řádků LF, desetinná tečka bez ohledu na locale.
 * tstates_pct = podíl T-stavů adresy na T-stavech všech instrukcí [%].
 *
 * @param path     Cílový soubor (přepíše se).
 * @param sort     Řazení.
 * @param err      Buffer na anglický popis chyby, nebo NULL.
 * @param err_size Velikost `err`.
 * @return Počet zapsaných řádků, nebo -1 při chybě zápisu.
 */
int profiler_pc_export_csv(const char *path, en_PROFILER_PC_SORT sort, char *err, size_t err_size);


#ifdef __cplusplus
}
#endif

#endif /* PROFILER_PC_H */
