/**
 * @file   ramlog.h
 * @brief  Záznam zápisů do paměti (ramlog): ring záznamů, mapa posledního
 *         zapisovatele, filtr rozsahů a dotazy "kdo zapsal X".
 *
 * Jádro modulu je čisté C bez stavu emulátoru: záznamy mu předávají
 * platformní háčky zápisu (`mzarch/mz<N>/memory/mz<N>_ramlog.c`) a zápisy
 * mimo CPU (debugger, freeze, CMT hack, načtení bloku).
 *
 * @par Fyzická buňka
 * Klíčem je fyzická buňka (prostor + offset), CPU adresa se ukládá vedle.
 * Prostory (en_RAMLOG_SPACE) jsou společné pro všechny platformy; jejich
 * velikost dá platforma při startu (ramlog_start()). Grafická VRAM MZ-800
 * má offset `plán * 2000h + fyzická adresa v plánu` (stejně jako fyzický
 * region VRAM v mhmap); jeden CPU zápis do ní dá záznam za každý zapsaný
 * plán.
 *
 * @par Ring
 * Záznamy mají pořadové číslo `seq` od 0 (začátek záznamu, ramlog_clear()
 * nebo ramlog_start() začne nový segment). Režim `ring` přepisuje nejstarší
 * (first_seq roste), režim `fill` drží prvních `capacity` záznamů a další
 * jen počítá (`dropped`; dostanou seq >= next_seq, v ringu nejsou).
 *
 * @par Mapa posledního zapisovatele
 * Jedna položka (8 B) na fyzickou buňku RAM, VRAM, PCG a MemExt (MemExt se
 * alokuje až při prvním zápisu do něj). Aktualizuje se při každém
 * zaznamenaném zápisu, i když ring v režimu `fill` už nic nepřijímá, takže
 * "kdo naposledy zapsal X" odpoví i po přetečení ringu.
 *
 * @par Vlákna
 * Záznam i dotazy volá emu vlákno (háčky zápisu, drain dbgapi). Modul nemá
 * zámky; čtení z jiného vlákna jde přes kopii v drainu dbgapi.
 *
 * @par Licence: GPLv3
 */

#ifndef RAMLOG_H
#define RAMLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fyzický prostor buňky. */
typedef enum en_RAMLOG_SPACE {
    RAMLOG_SPACE_RAM = 0,  /**< Hlavní RAM (64 KB). */
    RAMLOG_SPACE_VRAM,     /**< VRAM: MZ-800 32 KB (4 plány po 8 KB, CG-RAM a VRAM 700 v plánu I), MZ-700 a MZ-1500 4 KB. */
    RAMLOG_SPACE_PCG,      /**< MZ-1500: PCG banky 1-3 za sebou (3x 8 KB). */
    RAMLOG_SPACE_MEMEXT,   /**< Rozšíření paměti (MemExt), 512 KB. */
    RAMLOG_SPACE_ROM,      /**< Zápis pod ROM / CG-ROM (potlačený, offset = CPU adresa); bez mapy. */
    RAMLOG_SPACE_COUNT     /**< Počet prostorů (není platná hodnota). */
} en_RAMLOG_SPACE;

/** @brief Zdroj zápisu. */
typedef enum en_RAMLOG_SRC {
    RAMLOG_SRC_INSN = 0,   /**< Instrukce CPU (insn_pc = adresa jejího začátku). */
    RAMLOG_SRC_INT_ACK,    /**< Push návratové adresy při přijetí INT (insn_pc = návratová adresa). */
    RAMLOG_SRC_NMI_ACK,    /**< Push návratové adresy při přijetí NMI. */
    RAMLOG_SRC_DEBUGGER,   /**< Zápis debuggerem (Memory Browser, MCP mem_write, region_write). */
    RAMLOG_SRC_POKE,       /**< Akce `poke` breakpointu. */
    RAMLOG_SRC_FREEZE,     /**< Freeze Bytes. */
    RAMLOG_SRC_CMT_HACK,   /**< Rychlé načtení z kazety (CMT hack). */
    RAMLOG_SRC_LOAD,       /**< Načtení bloku (bootstrap, --run-mzf, MCP media_load_mzf). */
    RAMLOG_SRC_COUNT       /**< Počet zdrojů (není platná hodnota). */
} en_RAMLOG_SRC;

/** @brief Příznak záznamu: zápis hodnotu buňky nezměnil (old == new). */
#define RAMLOG_FLAG_UNCHANGED  0x01u
/** @brief Příznak záznamu: zápis se zahodil (pod ROM, grafická VRAM bez zapsaného plánu). */
#define RAMLOG_FLAG_SUPPRESSED 0x02u
/** @brief Příznak záznamu: souhrnný záznam bloku (count bajtů od offset / cpu_addr, old/new = 0). */
#define RAMLOG_FLAG_SUMMARY    0x04u
/** @brief Příznak záznamu: plane platí (zápis do plánu grafické VRAM MZ-800). */
#define RAMLOG_FLAG_PLANE      0x08u
/** @brief Příznak záznamu: zápis nemá CPU adresu (MCP region_write do RAM regionu). */
#define RAMLOG_FLAG_NO_CPU     0x10u
/**
 * @brief Příznak záznamu: souhrn bez přesných buněk (načtení bloku do grafické
 *        VRAM MZ-800: offset = CPU adresa & 3FFFh); mapa se neaktualizuje.
 */
#define RAMLOG_FLAG_NO_MAP     0x20u

/**
 * @brief Záznam zápisu v ringu (32 B; rozložení čte i trace_reader, kanál `ramlog`).
 *
 * Paměť hostitele (little endian na podporovaných hostitelích); stejné bajty
 * vrací čtení `raw_b64`.
 */
typedef struct st_RAMLOG_REC {
    uint64_t t;           /**< Čas začátku zapisující instrukce [takty GDG od startu emulace]. */
    uint32_t frame;       /**< Snímek (total_elapsed.screens). */
    uint16_t insn_pc;     /**< Adresa začátku zapisující instrukce (u INT_ACK / NMI_ACK návratová adresa). */
    uint16_t cpu_addr;    /**< CPU adresa zápisu (u souhrnu začátek bloku). */
    uint32_t offset;      /**< Offset buňky v prostoru. */
    uint8_t space;        /**< en_RAMLOG_SPACE. */
    uint8_t old_value;    /**< Hodnota buňky před zápisem. */
    uint8_t new_value;    /**< Hodnota buňky po zápisu (u potlačeného zápisu zapisovaná hodnota). */
    uint8_t src;          /**< en_RAMLOG_SRC. */
    uint8_t tin;          /**< T-stav uvnitř instrukce (op_tstate, přibližně). */
    uint8_t flags;        /**< RAMLOG_FLAG_*. */
    uint8_t plane;        /**< Plán grafické VRAM 0-3 (jen s RAMLOG_FLAG_PLANE). */
    uint8_t reserved0;    /**< 0. */
    uint16_t count;       /**< Souhrn: počet bajtů bloku (0 = 65536); jinak 1. */
    uint16_t reserved1;   /**< 0. */
} st_RAMLOG_REC;

/** @brief Položka mapy posledního zapisovatele (8 B). */
typedef struct st_RAMLOG_MAPENT {
    uint32_t seq1;        /**< seq + 1 posledního zápisu (dolních 32 bitů), 0 = buňku nikdo nezapsal. */
    uint16_t insn_pc;     /**< insn_pc posledního zápisu. */
    uint8_t src;          /**< en_RAMLOG_SRC posledního zápisu. */
    uint8_t flags;        /**< RAMLOG_FLAG_* posledního zápisu. */
} st_RAMLOG_MAPENT;

/** @brief Režim ringu. */
typedef enum en_RAMLOG_MODE {
    RAMLOG_MODE_RING = 0,  /**< Přepisuje nejstarší záznamy. */
    RAMLOG_MODE_FILL       /**< Drží prvních `capacity` záznamů, další počítá jako `dropped`. */
} en_RAMLOG_MODE;

/** @brief Nejvýš rozsahů filtru. */
#define RAMLOG_MAX_RANGES 16u
/** @brief Výchozí kapacita ringu [záznamy] (32 MiB). */
#define RAMLOG_DEFAULT_CAPACITY (1024u * 1024u)
/** @brief Nejmenší kapacita ringu. */
#define RAMLOG_MIN_CAPACITY 16u
/** @brief Největší kapacita ringu (512 MiB). */
#define RAMLOG_MAX_CAPACITY (16u * 1024u * 1024u)

/** @brief Rozsah filtru: CPU adresy, nebo fyzický prostor. */
typedef struct st_RAMLOG_RANGE {
    bool phys;            /**< false = CPU adresy [from, to], true = prostor `space` [from, to]. */
    uint8_t space;        /**< en_RAMLOG_SPACE (jen phys). */
    uint32_t from;        /**< První adresa / offset. */
    uint32_t to;          /**< Poslední adresa / offset (>= from). */
} st_RAMLOG_RANGE;

/** @brief Parametry startu záznamu. */
typedef struct st_RAMLOG_CONFIG {
    en_RAMLOG_MODE mode;  /**< Režim ringu. */
    uint32_t capacity;    /**< Kapacita ringu (RAMLOG_MIN_CAPACITY..RAMLOG_MAX_CAPACITY). */
    unsigned n_ranges;    /**< Počet rozsahů filtru; 0 = celá paměť. */
    st_RAMLOG_RANGE ranges[RAMLOG_MAX_RANGES]; /**< Rozsahy filtru (OR). */
} st_RAMLOG_CONFIG;

/** @brief Stav záznamu. */
typedef struct st_RAMLOG_STATUS {
    bool active;          /**< Zaznamenává se. */
    bool allocated;       /**< Ring a mapa existují (záznam proběhl a nebyl uvolněn). */
    unsigned segment;     /**< Pořadí záznamu (zvýší ramlog_start() a ramlog_clear()); 0 = žádný. */
    en_RAMLOG_MODE mode;  /**< Režim ringu. */
    uint32_t capacity;    /**< Kapacita ringu. */
    uint64_t first_seq;   /**< Nejstarší záznam v ringu. */
    uint64_t next_seq;    /**< Příští záznam ringu (= záznamy v ringu od začátku). */
    uint64_t total;       /**< Všechny zaznamenané zápisy (i zahozené v režimu fill). */
    uint64_t dropped;     /**< Zápisy, které ring v režimu fill nepřijal. */
    uint64_t overwritten; /**< Záznamy přepsané v režimu ring (= first_seq). */
    unsigned n_ranges;    /**< Počet rozsahů filtru. */
    st_RAMLOG_RANGE ranges[RAMLOG_MAX_RANGES]; /**< Rozsahy filtru. */
    uint64_t memory_bytes; /**< Alokovaná paměť (ring + mapa). */
} st_RAMLOG_STATUS;

/** @brief Jeden zapisovatel v souhrnu ramlog_writers(). */
typedef struct st_RAMLOG_WRITER {
    uint16_t insn_pc;     /**< Adresa zapisující instrukce. */
    uint8_t src;          /**< Zdroj (u různých zdrojů na stejné adrese samostatné položky). */
    uint64_t count;       /**< Počet zápisů. */
    uint64_t first_seq;   /**< První zápis. */
    uint64_t last_seq;    /**< Poslední zápis. */
} st_RAMLOG_WRITER;

/**
 * @brief Zaznamenává se? (příznak pro háčky; mimo záznam stojí jedno čtení).
 *
 * Nastavují ho jen ramlog_start() / ramlog_stop() / ramlog_free() v emu vlákně.
 */
extern int g_ramlog_active;

/**
 * @brief Zdroj zápisu pro háčky (výchozí RAMLOG_SRC_INSN).
 *
 * Volající zápisu mimo CPU ho nastaví kolem zápisu (ramlog_source_push() /
 * ramlog_source_pop()); přijetí přerušení kolem z80_process_interrupt().
 */
extern uint8_t g_ramlog_src;

/** @brief insn_pc pro zdroje mimo instrukci (návratová adresa u přijetí přerušení, PC u CMT hacku). */
extern uint16_t g_ramlog_src_pc;

/**
 * @brief Nastaví zdroj zápisu a vrátí předchozí (pro ramlog_source_pop()).
 * @param src Zdroj.
 * @param pc  insn_pc pro záznamy s tímto zdrojem (u INSN se nepoužije).
 * @return Předchozí stav (zdroj v dolních 8 bitech, pc v horních 16).
 */
uint32_t ramlog_source_push(en_RAMLOG_SRC src, uint16_t pc);

/**
 * @brief Obnoví zdroj zápisu uložený ramlog_source_push().
 * @param saved Návratová hodnota ramlog_source_push().
 */
void ramlog_source_pop(uint32_t saved);

/**
 * @brief Spustí záznam: uvolní předchozí ring a mapu, alokuje nové, nový segment.
 *
 * @param cfg        Parametry (kapacita se omezí na povolený rozsah).
 * @param space_size Velikost prostorů [buňky] (0 = prostor platforma nemá);
 *                   RAMLOG_SPACE_ROM se nepoužije, MemExt se alokuje až při
 *                   prvním zápisu.
 * @return false = nedostatek paměti (záznam neběží, nic není alokované).
 * @post g_ramlog_active == 1 při úspěchu.
 */
bool ramlog_start(const st_RAMLOG_CONFIG *cfg, const uint32_t space_size[RAMLOG_SPACE_COUNT]);

/**
 * @brief Zastaví záznam; ring a mapa zůstanou čitelné do dalšího startu nebo ramlog_free().
 */
void ramlog_stop(void);

/**
 * @brief Vyprázdní ring a mapu (nový segment); běžící záznam pokračuje.
 *
 * Volá ho i reset emulátoru a načtení snapshotu (záznamy se týkají jiného stavu).
 */
void ramlog_clear(void);

/**
 * @brief Uvolní ring a mapu (konec emulátoru).
 */
void ramlog_free(void);

/**
 * @brief Patří zápis do filtru?
 *
 * @param cpu_addr CPU adresa (nepoužije se s has_cpu = false).
 * @param has_cpu  Zápis má CPU adresu.
 * @param space    Prostor buňky.
 * @param offset   Offset buňky.
 * @return true = zaznamenat.
 */
bool ramlog_match(uint16_t cpu_addr, bool has_cpu, en_RAMLOG_SPACE space, uint32_t offset);

/**
 * @brief Může zápis na CPU stránku 4 KB patřit do filtru? (pro masku RAM fast-path)
 *
 * Konzervativní: s fyzickým rozsahem v prostoru RAM nebo MemExt vrátí true
 * pro každou stránku (platforma pak zápisy na stránku pošle do callbacku).
 *
 * @param page Stránka 0-15.
 * @return true = stránku z fast-path zápisu vyřadit.
 */
bool ramlog_page_watched(unsigned page);

/**
 * @brief Zaznamená zápis (ring + mapa); seq dostane od modulu.
 *
 * @param r Záznam (bez seq); u souhrnu (RAMLOG_FLAG_SUMMARY) se mapa
 *          aktualizuje pro všech `count` buněk od `offset`.
 * @pre g_ramlog_active.
 */
void ramlog_record(const st_RAMLOG_REC *r);

/**
 * @brief Poslední zapisovatel buňky z mapy.
 *
 * @param space  Prostor.
 * @param offset Offset.
 * @param out    Výstup.
 * @return false = buňku od začátku segmentu nikdo nezapsal (nebo mimo prostor).
 */
bool ramlog_lastwriter(en_RAMLOG_SPACE space, uint32_t offset, st_RAMLOG_MAPENT *out);

/**
 * @brief Stav záznamu.
 * @param out Výstup.
 */
void ramlog_get_status(st_RAMLOG_STATUS *out);

/**
 * @brief Záznam ringu podle seq.
 *
 * @param seq Pořadové číslo.
 * @param out Výstup.
 * @return false = seq není v ringu (přepsaný, zahozený nebo ještě nevznikl).
 */
bool ramlog_get(uint64_t seq, st_RAMLOG_REC *out);

/**
 * @brief Nejnovější záznamy ringu (od nejnovějšího).
 *
 * @param out_seq Výstup seq (smí být NULL).
 * @param out     Výstup záznamů.
 * @param max     Kapacita výstupu.
 * @param skip_unchanged Vynechat záznamy s RAMLOG_FLAG_UNCHANGED / SUPPRESSED.
 * @param scan_max Nejvýš prošlých záznamů (omezí čas při mnoha skrytých); 0 = celý ring.
 * @return Počet zapsaných (<= max).
 */
size_t ramlog_recent(uint64_t *out_seq, st_RAMLOG_REC *out, size_t max,
                     bool skip_unchanged, uint64_t scan_max);

/**
 * @brief Zápisy do buňky od nejnovějšího (průchod ringem odzadu).
 *
 * @param space  Prostor.
 * @param offset Offset (souhrnné záznamy se započtou, pokud buňku pokrývají).
 * @param out_seq Výstup seq nalezených záznamů (smí být NULL).
 * @param out    Výstup záznamů.
 * @param max    Kapacita výstupu.
 * @param skip_unchanged Vynechat záznamy s RAMLOG_FLAG_UNCHANGED / SUPPRESSED.
 * @return Počet nalezených (<= max).
 */
size_t ramlog_who_wrote(en_RAMLOG_SPACE space, uint32_t offset, uint64_t *out_seq,
                        st_RAMLOG_REC *out, size_t max, bool skip_unchanged);

/**
 * @brief Souhrn zapisovatelů rozsahu buněk (odlišné insn_pc + zdroj s počty).
 *
 * @param space   Prostor.
 * @param from    První offset.
 * @param to      Poslední offset.
 * @param out     Výstup, seřazený podle počtu sestupně.
 * @param max     Kapacita výstupu.
 * @param total   Výstup: počet odlišných zapisovatelů (i těch, co se nevešli), smí být NULL.
 * @param skip_unchanged Vynechat záznamy s RAMLOG_FLAG_UNCHANGED / SUPPRESSED.
 * @return Počet zapsaných položek.
 */
size_t ramlog_writers(en_RAMLOG_SPACE space, uint32_t from, uint32_t to,
                      st_RAMLOG_WRITER *out, size_t max, size_t *total, bool skip_unchanged);

/**
 * @brief Souhrn zapisovatelů podle CPU adresy záznamu (adresa v okamžiku zápisu).
 *
 * Jako ramlog_writers(), ale vybírá záznamy podle cpu_addr (souhrn bloku
 * pokrývá count adres); záznamy bez CPU adresy (RAMLOG_FLAG_NO_CPU) vynechá.
 *
 * @param from  První CPU adresa.
 * @param to    Poslední CPU adresa.
 * @param out   Výstup, seřazený podle počtu sestupně.
 * @param max   Kapacita výstupu.
 * @param total Výstup: počet odlišných zapisovatelů, smí být NULL.
 * @param skip_unchanged Vynechat záznamy s RAMLOG_FLAG_UNCHANGED / SUPPRESSED.
 * @return Počet zapsaných položek.
 */
size_t ramlog_writers_cpu(uint16_t from, uint16_t to,
                          st_RAMLOG_WRITER *out, size_t max, size_t *total, bool skip_unchanged);

/**
 * @brief Zkopíruje souvislý úsek ringu (pro čtení z jiného vlákna přes dbgapi).
 *
 * @param from_seq  První požadovaný záznam (níž než first_seq = od first_seq).
 * @param count     Nejvýš záznamů.
 * @param dst       Výstup (count * sizeof(st_RAMLOG_REC) bajtů).
 * @param out_seq   Výstup: seq prvního zkopírovaného.
 * @return Počet zkopírovaných záznamů.
 */
uint64_t ramlog_copy(uint64_t from_seq, uint64_t count, st_RAMLOG_REC *dst, uint64_t *out_seq);

/** @brief Jméno prostoru ("ram", "vram", "pcg", "memext", "rom"), "?" mimo rozsah. */
const char *ramlog_space_name(unsigned space);

/** @brief Jméno zdroje ("insn", "int_ack", ...), "?" mimo rozsah. */
const char *ramlog_src_name(unsigned src);

/**
 * @brief Prostor podle jména.
 * @return Prostor, nebo -1.
 */
int ramlog_space_from_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* RAMLOG_H */
