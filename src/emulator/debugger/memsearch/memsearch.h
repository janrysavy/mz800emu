/**
 * @file memsearch.h
 * @brief Postupné hledání proměnných v paměti (cheat search, RAM search).
 *
 * Stavový vyhledávač: drží množinu kandidátních adres nad zvoleným
 * prostorem paměti a každý další filtr ji zužuje (rovná se, zvýšilo /
 * snížilo se o N, změnilo / nezměnilo se, BCD, řetězec číslic). Slouží
 * oknu Memory Search v GUI i MCP příkazům `memsearch_*`.
 *
 * Model:
 *   - **Prostor** je zřetězení spanů. Span je souvislý úsek regionu
 *     dbgapi (kind, sub_id, offset, length). Offset v prostoru (dále jen
 *     "offset") je index do zřetězení, memsearch_item() ho převede zpět
 *     na (kind, sub_id, offset v regionu). Klíč (kind, sub_id) je stabilní
 *     přes připojení a odpojení periferií, ID regionu ne.
 *   - **Vzorek** (st_MEMSEARCH_SAMPLE) je kopie celého prostoru v jednom
 *     okamžiku emulace a příznak připojení každého spanu. Pořizuje ho
 *     emu vlákno (memsearch_sample_fill(), příkaz dbgapi
 *     DBGAPI_CMD_MEMSEARCH_SAMPLE). Vzorky jsou po naplnění neměnné
 *     a sdílené (počítání odkazů), drží je sezení i historie undo.
 *   - **Kandidát** je počáteční offset hodnoty. Vícebajtová hodnota nesmí
 *     přesahovat hranici spanu. Kandidáti jsou bitmapa, 1 bit na offset.
 *   - Sezení drží tři vzorky: `first` (start hledání), `cur` (poslední)
 *     a `prev` (vzorek před `cur`). Filtr s novým vzorkem S porovná S
 *     s `cur` ("předchozí hodnota") nebo s `first`, potom `prev = cur`
 *     a `cur = S`. Filtr bez nového vzorku porovná `cur` s `prev`.
 *     memsearch_sample() posune vzorky bez filtrace.
 *   - Každý krok (start, vzorek, filtr, ruční vyřazení) jde vrátit
 *     memsearch_undo() a znovu provést memsearch_redo(). Krok vrací
 *     bitmapu i vzorky `prev` a `cur`.
 *
 * Kandidáti ve spanu, který je ve vzorku odpojený (MemExt nebo RAMdisk
 * odpojený během hledání), zůstávají filtrem beze změny a výpis je
 * označí (st_MEMSEARCH_ITEM::connected).
 *
 * Modul je čisté C bez globálního stavu emulátoru (kromě
 * memsearch_sample_fill() v memsearch_sample.c) a jde testovat nad
 * syntetickými vzorky.
 *
 * Vlákna: sezení nemá vlastní zámek. Volající zaručí, že se sezením
 * pracuje jedno vlákno naráz (GUI jedno vlákno UI, MCP přes
 * memsearch_registry_acquire()). Vzorek po naplnění smí číst více
 * vláken současně, počítání odkazů je atomické.
 *
 * Licence: GPLv3
 */

#ifndef MEMSEARCH_H
#define MEMSEARCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Výchozí strop velikosti prostoru (4 MiB).
 *
 * Stejný jako snapshot Memory Browseru. Větší prostor (RAMdisk STD má až
 * 16 MB) jde založit jen s výslovným povolením (@c allow_large), protože
 * sezení drží tři vzorky a historie undo další.
 */
#define MEMSEARCH_DEFAULT_MAX_SPACE (4u * 1024u * 1024u)

/**
 * @brief Tvrdý strop velikosti prostoru (32 MiB) i s povolením velkého prostoru.
 *
 * Pokryje plnou sestavu MZ-800 (RAMdisk STD 16 MB, oba PEZIK, MemExt,
 * RAM a VRAM).
 */
#define MEMSEARCH_HARD_MAX_SPACE (32u * 1024u * 1024u)

/** @brief Nejvyšší hloubka historie undo (počet kroků). */
#define MEMSEARCH_UNDO_MAX_DEPTH 32

/**
 * @brief Rozpočet paměti historie undo v bajtech (64 MiB).
 *
 * Hloubka historie se zmenší tak, aby (bitmapa + 2 vzorky) x hloubka
 * nepřesáhla rozpočet; vždy se drží aspoň jeden krok.
 */
#define MEMSEARCH_UNDO_BUDGET (64u * 1024u * 1024u)

/** @brief Nejvyšší šířka hodnoty v bajtech (řetězec 8 číslic). */
#define MEMSEARCH_MAX_WIDTH 8


/**
 * @brief Typ hledané hodnoty.
 */
typedef enum en_MEMSEARCH_TYPE {
    MEMSEARCH_TYPE_UINT = 0,    /**< Celé číslo bez znaménka, šířka 1-4 B. */
    MEMSEARCH_TYPE_SINT,        /**< Celé číslo se znaménkem (dvojkový doplněk), šířka 1-4 B. */
    MEMSEARCH_TYPE_BCD,         /**< Packed BCD, 2 číslice na bajt, šířka 1-4 B. Nibble nad 9 = neplatná hodnota. */
    MEMSEARCH_TYPE_DIGITS,      /**< Řetězec číslic, 1 číslice na bajt, šířka 1-8 B, kódování podle en_MEMSEARCH_DIGIT_ENC. */
    MEMSEARCH_TYPE_COUNT        /**< Sentinel. */
} en_MEMSEARCH_TYPE;


/**
 * @brief Kódování číslice v řetězci číslic (MEMSEARCH_TYPE_DIGITS).
 */
typedef enum en_MEMSEARCH_DIGIT_ENC {
    MEMSEARCH_DIGITS_RAW = 0,   /**< Holé hodnoty 00h-09h. */
    MEMSEARCH_DIGITS_ASCII,     /**< ASCII 30h-39h, mezera 20h. */
    MEMSEARCH_DIGITS_DISPLAY,   /**< Display kód Sharp MZ 20h-29h, mezera 00h. */
    MEMSEARCH_DIGITS_COUNT      /**< Sentinel. */
} en_MEMSEARCH_DIGIT_ENC;


/**
 * @brief Formát hodnoty: typ, šířka, pořadí bajtů a kódování.
 *
 * @invariant UINT, SINT a BCD mají šířku 1-4, DIGITS 1-8
 *            (MEMSEARCH_MAX_WIDTH), viz memsearch_fmt_validate().
 */
typedef struct st_MEMSEARCH_FMT {
    en_MEMSEARCH_TYPE      type;          /**< Typ hodnoty. */
    unsigned               width;         /**< Šířka v bajtech. */
    bool                   big_endian;    /**< UINT/SINT/BCD: nejvyšší řád na nejnižší adrese. DIGITS: nejvyšší řád první (pořadí tisku). */
    en_MEMSEARCH_DIGIT_ENC digit_enc;     /**< Kódování číslic (jen DIGITS). */
    bool                   leading_space; /**< DIGITS: mezery zleva čti jako nuly (ASCII 20h, display 00h; u RAW bez vlivu). Mezera za první číslicí je neplatná. */
    bool                   aligned;       /**< Kandidáti jen na offsetech dělitelných šířkou (od začátku spanu). */
} st_MEMSEARCH_FMT;


/**
 * @brief Operace filtru.
 *
 * "ref" je podle st_MEMSEARCH_FILTER::compare_to hodnota zadaná
 * (`value`), předchozí nebo první. Aritmetika je nad dekódovanými
 * hodnotami jako nad celými čísly (bez přetečení na šířku): u8 z 00h
 * "dec_by 1" na FFh neprojde.
 */
typedef enum en_MEMSEARCH_OP {
    MEMSEARCH_OP_EQ = 0,        /**< cur == ref */
    MEMSEARCH_OP_NE,            /**< cur != ref */
    MEMSEARCH_OP_LT,            /**< cur <  ref */
    MEMSEARCH_OP_LE,            /**< cur <= ref */
    MEMSEARCH_OP_GT,            /**< cur >  ref */
    MEMSEARCH_OP_GE,            /**< cur >= ref */
    MEMSEARCH_OP_BETWEEN,       /**< value <= cur <= value2 (jen compare_to VALUE). */
    MEMSEARCH_OP_INC,           /**< cur > ref (jen PREVIOUS / FIRST). */
    MEMSEARCH_OP_DEC,           /**< cur < ref (jen PREVIOUS / FIRST). */
    MEMSEARCH_OP_INC_BY,        /**< cur == ref + value (jen PREVIOUS / FIRST, value >= 0). */
    MEMSEARCH_OP_DEC_BY,        /**< cur == ref - value (jen PREVIOUS / FIRST, value >= 0). */
    MEMSEARCH_OP_CHANGED,       /**< cur != ref (jen PREVIOUS / FIRST). */
    MEMSEARCH_OP_UNCHANGED,     /**< cur == ref (jen PREVIOUS / FIRST). */
    MEMSEARCH_OP_CHANGED_BY,    /**< |cur - ref| == value (jen PREVIOUS / FIRST, value >= 0). */
    MEMSEARCH_OP_MASK_EQ,       /**< (bity cur & mask) == (bity ref & mask); u VALUE je ref = value. Jen UINT/SINT. */
    MEMSEARCH_OP_COUNT          /**< Sentinel. */
} en_MEMSEARCH_OP;


/**
 * @brief S čím filtr porovnává.
 */
typedef enum en_MEMSEARCH_CMP {
    MEMSEARCH_CMP_VALUE = 0,    /**< Se zadanou hodnotou `value`. */
    MEMSEARCH_CMP_PREVIOUS,     /**< S předchozím vzorkem. */
    MEMSEARCH_CMP_FIRST,        /**< Se vzorkem ze startu hledání. */
    MEMSEARCH_CMP_COUNT         /**< Sentinel. */
} en_MEMSEARCH_CMP;


/**
 * @brief Parametry jednoho filtru.
 *
 * @invariant Kombinace op / compare_to / value odpovídá
 *            memsearch_filter_validate().
 */
typedef struct st_MEMSEARCH_FILTER {
    en_MEMSEARCH_OP  op;          /**< Operace. */
    en_MEMSEARCH_CMP compare_to;  /**< Reference. */
    int64_t          value;       /**< Hodnota (VALUE), rozdíl (INC_BY, DEC_BY, CHANGED_BY) nebo dolní mez (BETWEEN). */
    int64_t          value2;      /**< Horní mez (BETWEEN). */
    uint32_t         mask;        /**< Maska bitů (MASK_EQ). */
    bool             negate;      /**< Obrátí výsledek. Neplatná hodnota (BCD, číslice) kandidáta vyřadí vždy. */
} st_MEMSEARCH_FILTER;


/**
 * @brief Jeden span prostoru: souvislý úsek regionu dbgapi.
 *
 * @invariant length > 0.
 */
typedef struct st_MEMSEARCH_SPAN {
    int      region_kind;   /**< en_REGION_KIND (dbgapi_regions.h). */
    int      sub_id;        /**< Disambiguator regionu (banka, plane, instance). */
    uint32_t offset;        /**< Začátek úseku v regionu. */
    uint32_t length;        /**< Délka úseku v bajtech. */
} st_MEMSEARCH_SPAN;


/**
 * @brief Vzorek prostoru v jednom okamžiku emulace.
 *
 * Vzniká přes memsearch_sample_new(), plní ho emu vlákno, potom je
 * neměnný. Uvolní se posledním memsearch_sample_unref().
 *
 * @invariant data má @c size bajtů, span_connected má @c span_count
 *            položek. Bajty odpojeného spanu jsou nulové.
 */
typedef struct st_MEMSEARCH_SAMPLE {
    int       refcount;        /**< Počet odkazů (atomicky přes g_atomic_int_*). */
    uint32_t  size;            /**< Velikost prostoru v bajtech. */
    uint8_t  *data;            /**< Kopie prostoru (vlastní vzorek). */
    unsigned  span_count;      /**< Počet spanů. */
    uint8_t  *span_connected;  /**< 1 = span byl při vzorku připojený a přečtený, 0 = ne. */
    uint64_t  frame;           /**< Číslo snímku emulace v okamžiku vzorku (g_gdg.total_elapsed.screens). */
} st_MEMSEARCH_SAMPLE;


/** @brief Neprůhledné sezení hledání. */
typedef struct st_MEMSEARCH_SESSION st_MEMSEARCH_SESSION;


/**
 * @brief Hodnota kandidáta v jednom vzorku.
 */
typedef struct st_MEMSEARCH_VALUE {
    bool    valid;      /**< false = neplatné BCD / číslice nebo vzorek chybí. */
    int64_t value;      /**< Dekódovaná hodnota (jen když valid). */
    uint8_t raw[MEMSEARCH_MAX_WIDTH]; /**< Bajty ze vzorku v pořadí adres (prvních `width`). */
} st_MEMSEARCH_VALUE;


/**
 * @brief Jedna položka výpisu kandidátů.
 */
typedef struct st_MEMSEARCH_ITEM {
    uint32_t           offset;         /**< Offset v prostoru. */
    unsigned           span_index;     /**< Index spanu. */
    int                region_kind;    /**< en_REGION_KIND spanu. */
    int                sub_id;         /**< sub_id spanu. */
    uint32_t           region_offset;  /**< Offset v regionu (span.offset + posun ve spanu). */
    bool               connected;      /**< Span připojený v posledním vzorku (`cur`). */
    st_MEMSEARCH_VALUE cur;            /**< Hodnota v posledním vzorku. */
    st_MEMSEARCH_VALUE prev;           /**< Hodnota v předchozím vzorku. */
    st_MEMSEARCH_VALUE first;          /**< Hodnota ve vzorku ze startu. */
} st_MEMSEARCH_ITEM;


/* ------------------------------------------------------------------ */
/* Vzorky                                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Alokuje prázdný vzorek (nulová data, všechny spany odpojené).
 *
 * @param size       Velikost prostoru v bajtech (> 0).
 * @param span_count Počet spanů (> 0).
 * @return Vzorek s refcount 1, nebo NULL při neplatných parametrech
 *         nebo nedostatku paměti.
 * @post Vlastní volající, uvolní přes memsearch_sample_unref().
 */
st_MEMSEARCH_SAMPLE *memsearch_sample_new(uint32_t size, unsigned span_count);

/**
 * @brief Přidá odkaz na vzorek.
 * @param s Vzorek (smí být NULL, pak nic).
 * @return @p s.
 * @note Thread-safe (atomické počítadlo).
 */
st_MEMSEARCH_SAMPLE *memsearch_sample_ref(st_MEMSEARCH_SAMPLE *s);

/**
 * @brief Odebere odkaz; poslední odkaz vzorek uvolní.
 * @param s Vzorek (smí být NULL, pak nic).
 * @note Thread-safe (atomické počítadlo).
 */
void memsearch_sample_unref(st_MEMSEARCH_SAMPLE *s);

/**
 * @brief Naplní vzorek z regionů dbgapi (emu vlákno).
 *
 * Obnoví výčet regionů (dbgapi_regions_refresh()), každý span dohledá
 * podle (kind, sub_id) a zkopíruje přes dbgapi_regions_read() (bez
 * vedlejšího efektu). Span, jehož region v sestavě není, je odpojený nebo
 * je kratší než úsek, má span_connected 0 a nulová data. Do @c frame
 * zapíše číslo snímku.
 *
 * Implementace je v memsearch_sample.c (jediná část modulu, která sahá
 * na stav emulátoru).
 *
 * @param spans Spany prostoru (stejné pořadí jako v sezení).
 * @param n     Počet spanů, musí se rovnat sample->span_count.
 * @param sample Vzorek z memsearch_sample_new() s velikostí rovnou součtu délek.
 * @return true = vzorek naplněný (i když jsou některé spany odpojené),
 *         false = neplatné parametry.
 * @pre EMU vlákno v bodě mezi instrukcemi (drain fronty dbgapi na hranici
 *      snímku nebo v pauze), aby byl vzorek konzistentní pro všechny spany.
 */
bool memsearch_sample_fill(const st_MEMSEARCH_SPAN *spans, unsigned n,
                           st_MEMSEARCH_SAMPLE *sample);


/* ------------------------------------------------------------------ */
/* Formát a filtr                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Ověří formát hodnoty.
 * @param fmt Formát.
 * @param[out] err Při chybě anglický popis (statický řetězec); smí být NULL.
 * @return true = formát je platný.
 */
bool memsearch_fmt_validate(const st_MEMSEARCH_FMT *fmt, const char **err);

/**
 * @brief Ověří filtr vůči formátu.
 *
 * Pravidla: BETWEEN jen s VALUE a value <= value2; INC, DEC, INC_BY,
 * DEC_BY, CHANGED, UNCHANGED a CHANGED_BY jen s PREVIOUS / FIRST;
 * rozdíl u INC_BY / DEC_BY / CHANGED_BY nezáporný; MASK_EQ jen u UINT /
 * SINT.
 *
 * @param f   Filtr.
 * @param fmt Formát sezení.
 * @param[out] err Při chybě anglický popis (statický řetězec); smí být NULL.
 * @return true = filtr je platný.
 */
bool memsearch_filter_validate(const st_MEMSEARCH_FILTER *f,
                               const st_MEMSEARCH_FMT *fmt, const char **err);

/**
 * @brief Dekóduje hodnotu z bajtů podle formátu.
 *
 * @param fmt   Formát.
 * @param bytes Ukazatel na @c fmt->width bajtů v pořadí adres.
 * @param[out] out Dekódovaná hodnota.
 * @return true = platná hodnota, false = neplatné BCD nebo číslice.
 */
bool memsearch_decode(const st_MEMSEARCH_FMT *fmt, const uint8_t *bytes,
                      int64_t *out);

/**
 * @brief Zakóduje hodnotu do bajtů podle formátu (opak memsearch_decode()).
 *
 * Používá se pro Freeze "na hodnotu" a pro testy. Řetězec číslic se
 * kóduje s nulami zleva (bez mezer).
 *
 * @param fmt   Formát.
 * @param value Hodnota.
 * @param[out] bytes @c fmt->width bajtů v pořadí adres.
 * @return false = hodnota se do formátu nevejde (záporná u BCD, DIGITS
 *         a UINT, mimo rozsah šířky).
 */
bool memsearch_encode(const st_MEMSEARCH_FMT *fmt, int64_t value,
                      uint8_t *bytes);


/* ------------------------------------------------------------------ */
/* Sezení                                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Založí sezení nad prostorem.
 *
 * Sezení zatím nemá vzorek ani kandidáty, další krok je memsearch_start().
 *
 * @param spans     Spany (zkopírují se).
 * @param n         Počet spanů (> 0).
 * @param fmt       Formát hodnoty (zkopíruje se).
 * @param max_space Strop velikosti prostoru v bajtech (typicky
 *                  MEMSEARCH_DEFAULT_MAX_SPACE, s povolením velkého prostoru
 *                  MEMSEARCH_HARD_MAX_SPACE; víc se nepovolí nikdy).
 * @param[out] err  Při chybě anglický popis (statický řetězec); smí být NULL.
 * @return Sezení, nebo NULL (neplatný formát, prázdný nebo příliš velký
 *         prostor, nedostatek paměti).
 * @post Vlastní volající, uvolní přes memsearch_session_free().
 */
st_MEMSEARCH_SESSION *memsearch_session_new(const st_MEMSEARCH_SPAN *spans,
                                            unsigned n,
                                            const st_MEMSEARCH_FMT *fmt,
                                            uint32_t max_space,
                                            const char **err);

/**
 * @brief Uvolní sezení, jeho bitmapy, historii a odkazy na vzorky.
 * @param s Sezení (smí být NULL).
 */
void memsearch_session_free(st_MEMSEARCH_SESSION *s);

/** @brief Velikost prostoru v bajtech. */
uint32_t memsearch_space_size(const st_MEMSEARCH_SESSION *s);

/** @brief Počet spanů. */
unsigned memsearch_span_count(const st_MEMSEARCH_SESSION *s);

/** @brief Span podle indexu (NULL mimo rozsah). Ukazatel platí do memsearch_session_free(). */
const st_MEMSEARCH_SPAN *memsearch_span(const st_MEMSEARCH_SESSION *s,
                                        unsigned idx);

/** @brief Formát sezení. Ukazatel platí do memsearch_session_free(). */
const st_MEMSEARCH_FMT *memsearch_fmt(const st_MEMSEARCH_SESSION *s);

/** @brief Počet kandidátů. */
uint64_t memsearch_candidate_count(const st_MEMSEARCH_SESSION *s);

/** @brief Počet provedených kroků (start = 1; undo snižuje, redo zvyšuje). */
unsigned memsearch_step(const st_MEMSEARCH_SESSION *s);

/** @brief true = sezení má vzorek (proběhl memsearch_start()). */
bool memsearch_started(const st_MEMSEARCH_SESSION *s);

/** @brief Číslo snímku posledního vzorku (0 bez vzorku). */
uint64_t memsearch_sample_frame(const st_MEMSEARCH_SESSION *s);

/** @brief Počet kroků, které jde vrátit. */
unsigned memsearch_undo_depth(const st_MEMSEARCH_SESSION *s);

/** @brief Počet kroků, které jde znovu provést. */
unsigned memsearch_redo_depth(const st_MEMSEARCH_SESSION *s);

/**
 * @brief Start hledání: první vzorek, všichni jsou kandidáti.
 *
 * Kandidáti jsou všechny offsety, ze kterých se hodnota vejde do spanu
 * (a u @c aligned leží na násobku šířky). Volitelný první filtr smí
 * porovnávat jen se zadanou hodnotou (compare_to VALUE).
 *
 * Opakované volání začne hledání znovu (předchozí stav jde vrátit undo).
 *
 * @param s       Sezení.
 * @param sample  Vzorek (velikost a počet spanů jako sezení); sezení si
 *                přidá odkaz.
 * @param initial První filtr, nebo NULL.
 * @param[out] err Při chybě anglický popis (statický řetězec); smí být NULL.
 * @return true = hotovo, false = neplatný vzorek nebo filtr (sezení beze
 *         změny).
 * @post first = prev = cur = @p sample; step se zvýší.
 */
bool memsearch_start(st_MEMSEARCH_SESSION *s, st_MEMSEARCH_SAMPLE *sample,
                     const st_MEMSEARCH_FILTER *initial, const char **err);

/**
 * @brief Nový vzorek bez filtrace (MAME `cheatnext all`).
 *
 * @param s      Sezení (po memsearch_start()).
 * @param sample Vzorek; sezení si přidá odkaz.
 * @param[out] err Při chybě anglický popis; smí být NULL.
 * @return true = hotovo.
 * @post prev = dřívější cur, cur = @p sample, kandidáti beze změny, krok jde vrátit.
 */
bool memsearch_sample(st_MEMSEARCH_SESSION *s, st_MEMSEARCH_SAMPLE *sample,
                      const char **err);

/**
 * @brief Filtr: vyřadí kandidáty, kteří podmínku nesplňují.
 *
 * S novým vzorkem S se hodnota z S porovná s `cur` (PREVIOUS), s `first`
 * (FIRST) nebo s `value`; potom prev = cur a cur = S. Bez vzorku
 * (@p sample NULL) se porovná `cur` s `prev`. Kandidáti ve spanu
 * odpojeném v porovnávaném nebo referenčním vzorku zůstávají beze změny.
 *
 * @param s       Sezení (po memsearch_start()).
 * @param sample  Nový vzorek, nebo NULL.
 * @param f       Filtr (memsearch_filter_validate()).
 * @param[out] removed Počet vyřazených kandidátů; smí být NULL.
 * @param[out] err Při chybě anglický popis; smí být NULL.
 * @return true = hotovo (i když se nic nevyřadilo), false = neplatný
 *         filtr nebo vzorek (sezení beze změny).
 */
bool memsearch_filter(st_MEMSEARCH_SESSION *s, st_MEMSEARCH_SAMPLE *sample,
                      const st_MEMSEARCH_FILTER *f, uint64_t *removed,
                      const char **err);

/**
 * @brief Vyhodnotí filtr pro jednoho kandidáta bez změny sezení (náhled).
 *
 * Stejná sémantika jako memsearch_filter() bez vzorku (cur proti prev).
 *
 * @param s      Sezení.
 * @param offset Offset v prostoru.
 * @param f      Filtr (platný).
 * @return 1 = kandidát by zůstal, 0 = byl by vyřazen, -1 = offset není
 *         kandidát nebo sezení nemá vzorek.
 */
int memsearch_eval(const st_MEMSEARCH_SESSION *s, uint32_t offset,
                   const st_MEMSEARCH_FILTER *f);

/**
 * @brief Ručně vyřadí kandidáty (FCEUX "Eliminate").
 *
 * Offsety, které kandidáti nejsou, se ignorují. Krok jde vrátit undo
 * (i když se nic nevyřadilo, krok vznikne jen při aspoň jednom vyřazení).
 *
 * @param s       Sezení.
 * @param offsets Offsety v prostoru.
 * @param n       Počet offsetů.
 * @return Počet skutečně vyřazených kandidátů.
 */
uint64_t memsearch_eliminate(st_MEMSEARCH_SESSION *s, const uint32_t *offsets,
                             unsigned n);

/**
 * @brief Vrátí poslední krok (bitmapu i vzorky prev / cur).
 * @return false = není co vrátit.
 */
bool memsearch_undo(st_MEMSEARCH_SESSION *s);

/**
 * @brief Znovu provede vrácený krok.
 * @return false = není co znovu provést.
 */
bool memsearch_redo(st_MEMSEARCH_SESSION *s);

/**
 * @brief true = offset je kandidát.
 */
bool memsearch_is_candidate(const st_MEMSEARCH_SESSION *s, uint32_t offset);

/**
 * @brief N-tý kandidát v pořadí offsetů (stránkování).
 *
 * Při počtu kandidátů do 1 Mi si sezení postaví index (platí do další
 * změny kandidátů), jinak prochází bitmapu.
 *
 * @param s      Sezení (logicky const; index je interní cache, proto ne const).
 * @param n      Pořadí kandidáta (0-based).
 * @param[out] out_offset Offset kandidáta.
 * @return false = n >= počet kandidátů.
 */
bool memsearch_nth_candidate(st_MEMSEARCH_SESSION *s, uint64_t n,
                             uint32_t *out_offset);

/**
 * @brief Popis kandidáta (nebo libovolného offsetu) pro výpis.
 *
 * @param s      Sezení.
 * @param offset Offset v prostoru.
 * @param[out] out Položka.
 * @return false = offset mimo prostor nebo sezení bez vzorku.
 */
bool memsearch_item(const st_MEMSEARCH_SESSION *s, uint32_t offset,
                    st_MEMSEARCH_ITEM *out);

/**
 * @brief Převede (kind, sub_id, offset v regionu) na offset v prostoru.
 *
 * @return true = bajt leží v některém spanu.
 */
bool memsearch_find_offset(const st_MEMSEARCH_SESSION *s, int region_kind,
                           int sub_id, uint32_t region_offset,
                           uint32_t *out_offset);


/* ------------------------------------------------------------------ */
/* Textové názvy (MCP, INI, docs)                                      */
/* ------------------------------------------------------------------ */

/** @brief Stabilní název operace ("eq", "dec_by", ...), NULL mimo rozsah. */
const char *memsearch_op_name(en_MEMSEARCH_OP op);

/** @brief Operace podle názvu; -1 = neznámý název. */
int memsearch_op_from_name(const char *name);

/** @brief Stabilní název reference ("value", "previous", "first"). */
const char *memsearch_cmp_name(en_MEMSEARCH_CMP c);

/** @brief Reference podle názvu; -1 = neznámý název. */
int memsearch_cmp_from_name(const char *name);

/** @brief Stabilní název kódování číslic ("raw", "ascii", "display"). */
const char *memsearch_digit_enc_name(en_MEMSEARCH_DIGIT_ENC e);

/** @brief Kódování podle názvu; -1 = neznámý název. */
int memsearch_digit_enc_from_name(const char *name);

/**
 * @brief Formát podle názvu typu.
 *
 * Názvy: "u8", "s8", "u16", "s16", "u24", "s24", "u32", "s32" (šířka
 * podle názvu), "bcd" a "digits" (šířku dodá volající).
 *
 * @param name Název typu.
 * @param[out] type Typ.
 * @param[out] width Šířka z názvu, 0 u "bcd" a "digits".
 * @return false = neznámý název.
 */
bool memsearch_type_from_name(const char *name, en_MEMSEARCH_TYPE *type,
                              unsigned *width);

/* ------------------------------------------------------------------ */
/* Předvolby prostoru                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Předvolba prostoru (sada druhů regionů).
 *
 * Do prostoru jdou jen připojené regiony daných druhů, celé, v pořadí
 * výčtu. ROM, CG-ROM, FLASH a PROHIBITED shadow nejsou nikdy (ROM regiony
 * mají writable kvůli ladicím patchům, proto se vybírá podle druhu).
 */
typedef enum en_MEMSEARCH_PRESET {
    MEMSEARCH_PRESET_RAM = 0,       /**< User RAM 64 KB (REGION_KIND_RAM). Výchozí. */
    MEMSEARCH_PRESET_LOGICAL,       /**< Pohled CPU 64 KB (REGION_KIND_LOGICAL). */
    MEMSEARCH_PRESET_VRAM,          /**< VRAM 700 char/attr a VRAM plane I-IV. */
    MEMSEARCH_PRESET_MEMEXT,        /**< RAM banky MemExt. */
    MEMSEARCH_PRESET_RAMDISK,       /**< Banky RAMdisku STD a PEZIK. */
    MEMSEARCH_PRESET_ALL_WRITABLE,  /**< RAM, VRAM, CG-RAM, PCG, MemExt RAM, RAMdisky (bez Logical). */
    MEMSEARCH_PRESET_COUNT          /**< Sentinel. */
} en_MEMSEARCH_PRESET;

/** @brief Stabilní název předvolby ("ram", "logical", ...), NULL mimo rozsah. */
const char *memsearch_preset_name(en_MEMSEARCH_PRESET p);

/** @brief Předvolba podle názvu; -1 = neznámý název. */
int memsearch_preset_from_name(const char *name);

/**
 * @brief Sestaví spany předvolby z výčtu regionů.
 *
 * @param p       Předvolba.
 * @param regs    Výčet regionů (dbgapi_regions_enumerate_dup() a spol.).
 *                Typ je st_REGION_DESC z dbgapi_regions.h, tady jako
 *                neprůhledný ukazatel, aby hlavička nezávisela na dbgapi.
 * @param count   Počet regionů.
 * @param[out] out Spany (alokuje volající, aspoň @p count položek).
 * @return Počet zapsaných spanů (0 = předvolba v sestavě nic nemá).
 */
unsigned memsearch_spans_from_preset(en_MEMSEARCH_PRESET p, const void *regs,
                                     int count, st_MEMSEARCH_SPAN *out);

/* ------------------------------------------------------------------ */
/* Převod kandidáta na adresu Z80 a write breakpoint                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Zóna write breakpointu pro kandidáta (podmnožina en_BP_ZONE).
 *
 * Názvy memsearch_bpzone_name() odpovídají kanonickým názvům zón BP
 * ("RAM", "VRAM_FB", ...), modul ale nezávisí na breakpoints.h.
 */
typedef enum en_MEMSEARCH_BPZONE {
    MEMSEARCH_BPZONE_NONE = 0,     /**< Write breakpoint buňku nezachytí (VRAM plane, CG-RAM, PCG, RAMdisk, nenamapovaný Luftner). */
    MEMSEARCH_BPZONE_CPU_VIEW,     /**< CPU adresa podle aktuálního mapování (Logical, namapovaný kus MemExt Luftner). */
    MEMSEARCH_BPZONE_RAM,          /**< User RAM (nechytá zápisy do ROM a VRAM na stejné adrese). */
    MEMSEARCH_BPZONE_VRAM_FB,      /**< VRAM 700 char/attr v okně 0D000h / 0D800h. */
    MEMSEARCH_BPZONE_MMEXT_BANK,   /**< Banka MemExt PEHU, adresa = offset v 8 KB bance (bp_addr_space bank_offset). */
} en_MEMSEARCH_BPZONE;

/**
 * @brief Stav MemExt potřebný pro převod kandidáta (z DBGAPI_CMD_GET_MEMEXT_INFO).
 */
typedef struct st_MEMSEARCH_MEMEXT {
    bool     connected;    /**< MemExt připojený. */
    bool     pehu;         /**< Typ PEHU (jinak Luftner). */
    bool     map_valid;    /**< @c map je naplněná. */
    uint32_t map[16];      /**< Raw kus namapovaný do každého 4 KB okna CPU. */
} st_MEMSEARCH_MEMEXT;

/**
 * @brief Kam kandidát patří z pohledu CPU a jakým breakpointem ho hlídat.
 *
 * @invariant bp_zone == MEMSEARCH_BPZONE_NONE znamená, že ostatní pole bp_*
 *            nemají význam; bp_bank >= 0 jen u MMEXT_BANK.
 */
typedef struct st_MEMSEARCH_TARGET {
    bool                has_z80;      /**< Buňka je teď (nebo trvale) vidět na CPU adrese @c z80_addr. */
    uint16_t            z80_addr;     /**< CPU adresa (pro Watch a výpis). */
    en_MEMSEARCH_BPZONE bp_zone;      /**< Zóna write breakpointu. */
    uint16_t            bp_addr;      /**< Adresa BP (u MMEXT_BANK offset v bance 0000h-1FFFh). */
    uint16_t            bp_addr_end;  /**< Poslední bajt hodnoty (bp_addr + šířka - 1). */
    int                 bp_bank;      /**< Banka PEHU (raw kus / 2), jinak -1. */
} st_MEMSEARCH_TARGET;

/**
 * @brief Převede buňku kandidáta na CPU adresu a parametry write BP.
 *
 * - User RAM: adresa = offset, zóna RAM.
 * - Logical: adresa = offset, zóna CPU_VIEW.
 * - VRAM 700 char / attr: 0D000h / 0D800h + offset, zóna VRAM_FB.
 * - MemExt RAM: CPU adresa okna, kam je kus právě namapovaný (jinak bez
 *   adresy). PEHU: BP v zóně MMEXT_BANK, banka = raw kus / 2, offset =
 *   (raw kus mod 2) * 1000h + offset - platí bez ohledu na mapování
 *   (memext_map_pwrite: banka b = kusy 2b a 2b + 1). Luftner: CPU_VIEW,
 *   jen když je kus namapovaný.
 * - Ostatní (VRAM plane, CG-RAM, PCG, RAMdisk): bez adresy a bez BP.
 *
 * @param region_kind   en_REGION_KIND buňky.
 * @param sub_id        sub_id regionu.
 * @param region_offset Offset v regionu.
 * @param width         Šířka hodnoty (1-8).
 * @param mx            Stav MemExt (smí být NULL = nezjištěn).
 * @param[out] out      Výsledek.
 * @return true = buňka má CPU adresu nebo BP.
 */
bool memsearch_target(int region_kind, int sub_id, uint32_t region_offset,
                      unsigned width, const st_MEMSEARCH_MEMEXT *mx,
                      st_MEMSEARCH_TARGET *out);

/** @brief Kanonický název zóny BP ("RAM", "VRAM_FB", "CPU_VIEW", "MMEXT_BANK"), NULL u NONE. */
const char *memsearch_bpzone_name(en_MEMSEARCH_BPZONE z);

#ifdef __cplusplus
}
#endif

#endif /* MEMSEARCH_H */
