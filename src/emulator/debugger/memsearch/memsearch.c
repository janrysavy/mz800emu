/**
 * @file memsearch.c
 * @brief Jádro postupného hledání proměnných (viz memsearch.h).
 *
 * Čisté C nad vzorky paměti: dekódování hodnot, filtry, bitmapa
 * kandidátů, historie undo/redo a výpis. Stav emulátoru čte jen
 * memsearch_sample_fill() v memsearch_sample.c.
 *
 * Licence: GPLv3
 */

#include "memsearch.h"

#include <glib.h>
#include <string.h>

#include "../dbgapi_regions.h"


/** @brief Nejvyšší počet kandidátů, pro který se staví index pro memsearch_nth_candidate(). */
#define MEMSEARCH_INDEX_MAX ( 1u << 20 )


/**
 * @brief Stav hledání, který vrací undo a redo.
 *
 * @invariant started == false znamená cand == NULL a žádné vzorky.
 *            Jinak cand má session->words slov a first/prev/cur jsou
 *            vzorky s velikostí a počtem spanů sezení (vlastní odkaz).
 */
typedef struct st_MEMSEARCH_STATE {
    bool                 started;     /**< Proběhl start hledání. */
    uint64_t            *cand;        /**< Bitmapa kandidátů (vlastní). */
    uint64_t             cand_count;  /**< Počet nastavených bitů. */
    st_MEMSEARCH_SAMPLE *first;       /**< Vzorek ze startu (odkaz). */
    st_MEMSEARCH_SAMPLE *prev;        /**< Vzorek před cur (odkaz). */
    st_MEMSEARCH_SAMPLE *cur;         /**< Poslední vzorek (odkaz). */
    unsigned             step;        /**< Počet kroků. */
} st_MEMSEARCH_STATE;


/**
 * @brief Sezení hledání (neprůhledné, viz memsearch.h).
 *
 * @invariant span_base[i] je offset začátku spanu i v prostoru,
 *            span_base[span_count] == size. undo_n, redo_n <= undo_cap.
 *            index platí jen při index_valid (po změně kandidátů se
 *            zneplatní).
 */
struct st_MEMSEARCH_SESSION {
    st_MEMSEARCH_SPAN  *spans;        /**< Kopie spanů. */
    unsigned            span_count;   /**< Počet spanů. */
    uint32_t           *span_base;    /**< Prefixové součty délek (span_count + 1 položek). */
    uint32_t            size;         /**< Velikost prostoru. */
    st_MEMSEARCH_FMT    fmt;          /**< Formát hodnoty. */
    size_t              words;        /**< Počet 64bitových slov bitmapy. */
    st_MEMSEARCH_STATE  st;           /**< Aktuální stav. */
    st_MEMSEARCH_STATE  undo[MEMSEARCH_UNDO_MAX_DEPTH]; /**< Historie undo (nejstarší na indexu 0). */
    unsigned            undo_n;       /**< Počet kroků v undo. */
    st_MEMSEARCH_STATE  redo[MEMSEARCH_UNDO_MAX_DEPTH]; /**< Zásobník redo (poslední vrácený nahoře). */
    unsigned            redo_n;       /**< Počet kroků v redo. */
    unsigned            undo_cap;     /**< Hloubka historie podle rozpočtu (1..MEMSEARCH_UNDO_MAX_DEPTH). */
    uint32_t           *index;        /**< Offsety kandidátů v pořadí (cache). */
    uint64_t            index_n;      /**< Počet položek indexu. */
    bool                index_valid;  /**< Index odpovídá aktuálním kandidátům. */
};


/* ------------------------------------------------------------------ */
/* Vzorky                                                              */
/* ------------------------------------------------------------------ */

st_MEMSEARCH_SAMPLE *memsearch_sample_new(uint32_t size, unsigned span_count)
{
    if ( size == 0 || span_count == 0 ) return NULL;
    st_MEMSEARCH_SAMPLE *s = g_try_new0 ( st_MEMSEARCH_SAMPLE, 1 );
    if ( !s ) return NULL;
    s->data = g_try_malloc0 ( size );
    s->span_connected = g_try_malloc0 ( span_count );
    if ( !s->data || !s->span_connected ) {
        g_free ( s->data );
        g_free ( s->span_connected );
        g_free ( s );
        return NULL;
    };
    s->refcount = 1;
    s->size = size;
    s->span_count = span_count;
    return s;
}


st_MEMSEARCH_SAMPLE *memsearch_sample_ref(st_MEMSEARCH_SAMPLE *s)
{
    if ( s ) g_atomic_int_inc ( &s->refcount );
    return s;
}


void memsearch_sample_unref(st_MEMSEARCH_SAMPLE *s)
{
    if ( !s ) return;
    if ( g_atomic_int_dec_and_test ( &s->refcount ) ) {
        g_free ( s->data );
        g_free ( s->span_connected );
        g_free ( s );
    };
}


/* ------------------------------------------------------------------ */
/* Formát, dekódování a kódování                                       */
/* ------------------------------------------------------------------ */

bool memsearch_fmt_validate(const st_MEMSEARCH_FMT *fmt, const char **err)
{
    const char *e = NULL;
    if ( !fmt ) {
        e = "missing value format";
    } else if ( (unsigned) fmt->type >= MEMSEARCH_TYPE_COUNT ) {
        e = "unknown value type";
    } else if ( fmt->type == MEMSEARCH_TYPE_DIGITS ) {
        if ( fmt->width < 1 || fmt->width > MEMSEARCH_MAX_WIDTH )
            e = "digits width must be 1-8 bytes";
        else if ( (unsigned) fmt->digit_enc >= MEMSEARCH_DIGITS_COUNT )
            e = "unknown digit encoding";
    } else if ( fmt->width < 1 || fmt->width > 4 ) {
        e = "integer and BCD width must be 1-4 bytes";
    };
    if ( err ) *err = e;
    return e == NULL;
}


/**
 * @brief Bajt s pořadím `rank` od nejvyššího řádu (0 = nejvyšší).
 * @param fmt   Formát (určuje pořadí bajtů).
 * @param bytes Bajty v pořadí adres.
 * @param rank  0..width-1.
 * @return Bajt.
 */
static inline uint8_t ms_byte_by_rank(const st_MEMSEARCH_FMT *fmt,
                                      const uint8_t *bytes, unsigned rank)
{
    return fmt->big_endian ? bytes[rank] : bytes[fmt->width - 1 - rank];
}


/**
 * @brief Surové bity celého čísla (bez znaménkového rozšíření).
 * @param fmt   Formát UINT / SINT.
 * @param bytes Bajty v pořadí adres.
 * @return Hodnota bitů 0..2^(8*width)-1.
 */
static inline uint64_t ms_raw_bits(const st_MEMSEARCH_FMT *fmt,
                                   const uint8_t *bytes)
{
    uint64_t v = 0;
    for ( unsigned r = 0; r < fmt->width; r++ ) {
        v = ( v << 8 ) | ms_byte_by_rank ( fmt, bytes, r );
    };
    return v;
}


/**
 * @brief Kód mezery v kódování číslic, -1 když kódování mezeru nemá.
 * @param enc Kódování.
 * @return 20h (ASCII), 00h (display), -1 (holé číslice).
 */
static inline int ms_space_code(en_MEMSEARCH_DIGIT_ENC enc)
{
    switch ( enc ) {
        case MEMSEARCH_DIGITS_ASCII:   return 0x20;
        case MEMSEARCH_DIGITS_DISPLAY: return 0x00;
        default:                       return -1;
    };
}


/**
 * @brief Kód číslice 0 v kódování číslic.
 * @param enc Kódování.
 * @return 00h (holé), 30h (ASCII), 20h (display).
 */
static inline uint8_t ms_digit_base(en_MEMSEARCH_DIGIT_ENC enc)
{
    switch ( enc ) {
        case MEMSEARCH_DIGITS_ASCII:   return 0x30;
        case MEMSEARCH_DIGITS_DISPLAY: return 0x20;
        default:                       return 0x00;
    };
}


bool memsearch_decode(const st_MEMSEARCH_FMT *fmt, const uint8_t *bytes,
                      int64_t *out)
{
    int64_t v = 0;
    switch ( fmt->type ) {
        case MEMSEARCH_TYPE_UINT:
            v = (int64_t) ms_raw_bits ( fmt, bytes );
            break;

        case MEMSEARCH_TYPE_SINT: {
            uint64_t bits = ms_raw_bits ( fmt, bytes );
            unsigned nbits = fmt->width * 8;
            uint64_t sign = (uint64_t) 1 << ( nbits - 1 );
            v = ( bits & sign ) ? (int64_t) bits - (int64_t) ( (uint64_t) 1 << nbits )
                                : (int64_t) bits;
            break;
        }

        case MEMSEARCH_TYPE_BCD:
            for ( unsigned r = 0; r < fmt->width; r++ ) {
                uint8_t b = ms_byte_by_rank ( fmt, bytes, r );
                uint8_t hi = b >> 4, lo = b & 0x0F;
                if ( hi > 9 || lo > 9 ) return false;
                v = v * 100 + hi * 10 + lo;
            };
            break;

        case MEMSEARCH_TYPE_DIGITS: {
            uint8_t base = ms_digit_base ( fmt->digit_enc );
            int space = fmt->leading_space ? ms_space_code ( fmt->digit_enc ) : -1;
            bool leading = true;
            for ( unsigned r = 0; r < fmt->width; r++ ) {
                uint8_t b = ms_byte_by_rank ( fmt, bytes, r );
                if ( b >= base && b <= base + 9 ) {
                    v = v * 10 + ( b - base );
                    leading = false;
                } else if ( leading && space >= 0 && b == (uint8_t) space ) {
                    /* Mezera zleva = nula, hodnota se nemění. */
                } else {
                    return false;
                };
            };
            break;
        }

        default:
            return false;
    };
    *out = v;
    return true;
}


bool memsearch_encode(const st_MEMSEARCH_FMT *fmt, int64_t value,
                      uint8_t *bytes)
{
    uint8_t ms[MEMSEARCH_MAX_WIDTH]; /* od nejvyššího řádu */
    unsigned w = fmt->width;
    switch ( fmt->type ) {
        case MEMSEARCH_TYPE_UINT:
        case MEMSEARCH_TYPE_SINT: {
            unsigned nbits = w * 8;
            int64_t lo, hi;
            if ( fmt->type == MEMSEARCH_TYPE_UINT ) {
                lo = 0;
                hi = (int64_t) ( ( (uint64_t) 1 << nbits ) - 1 );
            } else {
                lo = -(int64_t) ( (uint64_t) 1 << ( nbits - 1 ) );
                hi = (int64_t) ( ( (uint64_t) 1 << ( nbits - 1 ) ) - 1 );
            };
            if ( value < lo || value > hi ) return false;
            uint64_t bits = (uint64_t) value;
            for ( unsigned r = 0; r < w; r++ ) {
                ms[w - 1 - r] = (uint8_t) ( bits >> ( 8 * r ) );
            };
            break;
        }

        case MEMSEARCH_TYPE_BCD: {
            if ( value < 0 ) return false;
            uint64_t v = (uint64_t) value;
            for ( int r = (int) w - 1; r >= 0; r-- ) {
                uint8_t lo = (uint8_t) ( v % 10 ); v /= 10;
                uint8_t hi = (uint8_t) ( v % 10 ); v /= 10;
                ms[r] = (uint8_t) ( ( hi << 4 ) | lo );
            };
            if ( v != 0 ) return false;
            break;
        }

        case MEMSEARCH_TYPE_DIGITS: {
            if ( value < 0 ) return false;
            uint64_t v = (uint64_t) value;
            uint8_t base = ms_digit_base ( fmt->digit_enc );
            for ( int r = (int) w - 1; r >= 0; r-- ) {
                ms[r] = (uint8_t) ( base + v % 10 );
                v /= 10;
            };
            if ( v != 0 ) return false;
            break;
        }

        default:
            return false;
    };
    for ( unsigned r = 0; r < w; r++ ) {
        if ( fmt->big_endian ) bytes[r] = ms[r];
        else bytes[w - 1 - r] = ms[r];
    };
    return true;
}


/* ------------------------------------------------------------------ */
/* Filtr                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief true = operace porovnává se vzorkem (PREVIOUS / FIRST povinně).
 * @param op Operace.
 */
static bool ms_op_needs_ref(en_MEMSEARCH_OP op)
{
    switch ( op ) {
        case MEMSEARCH_OP_INC:
        case MEMSEARCH_OP_DEC:
        case MEMSEARCH_OP_INC_BY:
        case MEMSEARCH_OP_DEC_BY:
        case MEMSEARCH_OP_CHANGED:
        case MEMSEARCH_OP_UNCHANGED:
        case MEMSEARCH_OP_CHANGED_BY:
            return true;
        default:
            return false;
    };
}


bool memsearch_filter_validate(const st_MEMSEARCH_FILTER *f,
                               const st_MEMSEARCH_FMT *fmt, const char **err)
{
    const char *e = NULL;
    if ( !f || !fmt ) {
        e = "missing filter";
    } else if ( (unsigned) f->op >= MEMSEARCH_OP_COUNT ) {
        e = "unknown filter operation";
    } else if ( (unsigned) f->compare_to >= MEMSEARCH_CMP_COUNT ) {
        e = "unknown compare_to";
    } else if ( ms_op_needs_ref ( f->op ) && f->compare_to == MEMSEARCH_CMP_VALUE ) {
        e = "this operation compares with a sample: use compare_to previous or first";
    } else if ( f->op == MEMSEARCH_OP_BETWEEN && f->compare_to != MEMSEARCH_CMP_VALUE ) {
        e = "between compares with values: use compare_to value";
    } else if ( f->op == MEMSEARCH_OP_BETWEEN && f->value > f->value2 ) {
        e = "between needs value <= value2";
    } else if ( ( f->op == MEMSEARCH_OP_INC_BY || f->op == MEMSEARCH_OP_DEC_BY
                  || f->op == MEMSEARCH_OP_CHANGED_BY ) && f->value < 0 ) {
        e = "the difference must not be negative";
    } else if ( f->op == MEMSEARCH_OP_MASK_EQ
                && fmt->type != MEMSEARCH_TYPE_UINT && fmt->type != MEMSEARCH_TYPE_SINT ) {
        e = "mask_eq needs an integer type";
    };
    if ( err ) *err = e;
    return e == NULL;
}


/**
 * @brief Vyhodnotí filtr pro jednu hodnotu.
 *
 * @param fmt     Formát.
 * @param f       Filtr (platný).
 * @param cmp     Bajty porovnávané hodnoty.
 * @param ref     Bajty referenční hodnoty (NULL u compare_to VALUE).
 * @return true = kandidát zůstává.
 */
static bool ms_eval_bytes(const st_MEMSEARCH_FMT *fmt,
                          const st_MEMSEARCH_FILTER *f,
                          const uint8_t *cmp, const uint8_t *ref)
{
    int64_t c, r = 0;
    if ( !memsearch_decode ( fmt, cmp, &c ) ) return false;
    if ( f->compare_to == MEMSEARCH_CMP_VALUE ) {
        r = f->value;
    } else if ( !memsearch_decode ( fmt, ref, &r ) ) {
        return false;
    };

    bool res;
    switch ( f->op ) {
        case MEMSEARCH_OP_EQ:         res = ( c == r ); break;
        case MEMSEARCH_OP_NE:         res = ( c != r ); break;
        case MEMSEARCH_OP_LT:         res = ( c <  r ); break;
        case MEMSEARCH_OP_LE:         res = ( c <= r ); break;
        case MEMSEARCH_OP_GT:         res = ( c >  r ); break;
        case MEMSEARCH_OP_GE:         res = ( c >= r ); break;
        case MEMSEARCH_OP_BETWEEN:    res = ( c >= f->value && c <= f->value2 ); break;
        case MEMSEARCH_OP_INC:        res = ( c >  r ); break;
        case MEMSEARCH_OP_DEC:        res = ( c <  r ); break;
        case MEMSEARCH_OP_INC_BY:     res = ( c == r + f->value ); break;
        case MEMSEARCH_OP_DEC_BY:     res = ( c == r - f->value ); break;
        case MEMSEARCH_OP_CHANGED:    res = ( c != r ); break;
        case MEMSEARCH_OP_UNCHANGED:  res = ( c == r ); break;
        case MEMSEARCH_OP_CHANGED_BY: res = ( c == r + f->value || c == r - f->value ); break;
        case MEMSEARCH_OP_MASK_EQ: {
            uint64_t cb = ms_raw_bits ( fmt, cmp );
            uint64_t rb = ( f->compare_to == MEMSEARCH_CMP_VALUE )
                          ? (uint64_t) f->value : ms_raw_bits ( fmt, ref );
            res = ( ( cb ^ rb ) & f->mask ) == 0;
            break;
        }
        default:
            res = false;
            break;
    };
    return f->negate ? !res : res;
}


/* ------------------------------------------------------------------ */
/* Stav a historie                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Uvolní stav (bitmapu a odkazy na vzorky) a vynuluje ho.
 * @param st Stav.
 */
static void ms_state_clear(st_MEMSEARCH_STATE *st)
{
    g_free ( st->cand );
    memsearch_sample_unref ( st->first );
    memsearch_sample_unref ( st->prev );
    memsearch_sample_unref ( st->cur );
    memset ( st, 0, sizeof ( *st ) );
}


/**
 * @brief Hluboká kopie stavu (bitmapa se zkopíruje, vzorky dostanou odkaz).
 * @param s   Sezení (počet slov bitmapy).
 * @param src Zdroj.
 * @param[out] dst Cíl (přepíše se bez uvolnění).
 * @return false = nedostatek paměti (dst vynulovaný).
 */
static bool ms_state_copy(const st_MEMSEARCH_SESSION *s,
                          const st_MEMSEARCH_STATE *src,
                          st_MEMSEARCH_STATE *dst)
{
    *dst = *src;
    dst->cand = NULL;
    if ( src->cand ) {
        dst->cand = g_try_malloc ( s->words * sizeof ( uint64_t ) );
        if ( !dst->cand ) {
            memset ( dst, 0, sizeof ( *dst ) );
            return false;
        };
        memcpy ( dst->cand, src->cand, s->words * sizeof ( uint64_t ) );
    };
    memsearch_sample_ref ( dst->first );
    memsearch_sample_ref ( dst->prev );
    memsearch_sample_ref ( dst->cur );
    return true;
}


/**
 * @brief Zahodí zásobník redo.
 * @param s Sezení.
 */
static void ms_redo_clear(st_MEMSEARCH_SESSION *s)
{
    for ( unsigned i = 0; i < s->redo_n; i++ ) ms_state_clear ( &s->redo[i] );
    s->redo_n = 0;
}


/**
 * @brief Uloží aktuální stav do historie undo před změnou (a zahodí redo).
 *
 * Při plné historii se zahodí nejstarší krok.
 *
 * @param s Sezení.
 * @return false = nedostatek paměti (sezení beze změny).
 */
static bool ms_push_undo(st_MEMSEARCH_SESSION *s)
{
    st_MEMSEARCH_STATE copy;
    if ( !ms_state_copy ( s, &s->st, &copy ) ) return false;
    if ( s->undo_n == s->undo_cap ) {
        ms_state_clear ( &s->undo[0] );
        memmove ( &s->undo[0], &s->undo[1],
                  ( s->undo_n - 1 ) * sizeof ( s->undo[0] ) );
        s->undo_n--;
    };
    s->undo[s->undo_n++] = copy;
    ms_redo_clear ( s );
    return true;
}


/** @brief Zneplatní index kandidátů. */
static inline void ms_index_invalidate(st_MEMSEARCH_SESSION *s)
{
    s->index_valid = false;
}


/* ------------------------------------------------------------------ */
/* Sezení                                                              */
/* ------------------------------------------------------------------ */

st_MEMSEARCH_SESSION *memsearch_session_new(const st_MEMSEARCH_SPAN *spans,
                                            unsigned n,
                                            const st_MEMSEARCH_FMT *fmt,
                                            uint32_t max_space,
                                            const char **err)
{
    const char *e = NULL;
    if ( err ) *err = NULL;
    if ( !memsearch_fmt_validate ( fmt, &e ) ) {
        if ( err ) *err = e;
        return NULL;
    };
    if ( !spans || n == 0 ) {
        if ( err ) *err = "empty search space";
        return NULL;
    };
    if ( max_space > MEMSEARCH_HARD_MAX_SPACE ) max_space = MEMSEARCH_HARD_MAX_SPACE;

    uint64_t total = 0;
    for ( unsigned i = 0; i < n; i++ ) {
        if ( spans[i].length == 0 ) {
            if ( err ) *err = "span of zero length";
            return NULL;
        };
        total += spans[i].length;
    };
    if ( total > max_space ) {
        if ( err ) *err = "search space is larger than the limit (allow_large raises it to 32 MiB)";
        return NULL;
    };

    st_MEMSEARCH_SESSION *s = g_try_new0 ( st_MEMSEARCH_SESSION, 1 );
    if ( !s ) goto oom;
    s->spans = g_try_new ( st_MEMSEARCH_SPAN, n );
    s->span_base = g_try_new ( uint32_t, n + 1 );
    if ( !s->spans || !s->span_base ) goto oom;
    memcpy ( s->spans, spans, n * sizeof ( *spans ) );
    s->span_count = n;
    s->span_base[0] = 0;
    for ( unsigned i = 0; i < n; i++ ) {
        s->span_base[i + 1] = s->span_base[i] + spans[i].length;
    };
    s->size = (uint32_t) total;
    s->fmt = *fmt;
    s->words = ( (size_t) s->size + 63 ) / 64;

    /* Hloubka historie podle rozpočtu: bitmapa + 2 vzorky na krok. */
    uint64_t per_step = (uint64_t) s->words * 8 + 2u * (uint64_t) s->size;
    uint64_t cap = MEMSEARCH_UNDO_BUDGET / ( per_step ? per_step : 1 );
    if ( cap < 1 ) cap = 1;
    if ( cap > MEMSEARCH_UNDO_MAX_DEPTH ) cap = MEMSEARCH_UNDO_MAX_DEPTH;
    s->undo_cap = (unsigned) cap;
    return s;

oom:
    if ( s ) {
        g_free ( s->spans );
        g_free ( s->span_base );
        g_free ( s );
    };
    if ( err ) *err = "out of memory";
    return NULL;
}


void memsearch_session_free(st_MEMSEARCH_SESSION *s)
{
    if ( !s ) return;
    ms_state_clear ( &s->st );
    for ( unsigned i = 0; i < s->undo_n; i++ ) ms_state_clear ( &s->undo[i] );
    ms_redo_clear ( s );
    g_free ( s->index );
    g_free ( s->spans );
    g_free ( s->span_base );
    g_free ( s );
}


uint32_t memsearch_space_size(const st_MEMSEARCH_SESSION *s) { return s ? s->size : 0; }
unsigned memsearch_span_count(const st_MEMSEARCH_SESSION *s) { return s ? s->span_count : 0; }

const st_MEMSEARCH_SPAN *memsearch_span(const st_MEMSEARCH_SESSION *s,
                                        unsigned idx)
{
    return ( s && idx < s->span_count ) ? &s->spans[idx] : NULL;
}

const st_MEMSEARCH_FMT *memsearch_fmt(const st_MEMSEARCH_SESSION *s) { return s ? &s->fmt : NULL; }
uint64_t memsearch_candidate_count(const st_MEMSEARCH_SESSION *s) { return s ? s->st.cand_count : 0; }
unsigned memsearch_step(const st_MEMSEARCH_SESSION *s) { return s ? s->st.step : 0; }
bool memsearch_started(const st_MEMSEARCH_SESSION *s) { return s && s->st.started; }
uint64_t memsearch_sample_frame(const st_MEMSEARCH_SESSION *s) { return ( s && s->st.cur ) ? s->st.cur->frame : 0; }
unsigned memsearch_undo_depth(const st_MEMSEARCH_SESSION *s) { return s ? s->undo_n : 0; }
unsigned memsearch_redo_depth(const st_MEMSEARCH_SESSION *s) { return s ? s->redo_n : 0; }


/**
 * @brief Ověří, že vzorek patří k prostoru sezení.
 * @param s      Sezení.
 * @param sample Vzorek.
 * @param[out] err Popis chyby.
 * @return true = velikost i počet spanů sedí.
 */
static bool ms_sample_ok(const st_MEMSEARCH_SESSION *s,
                         const st_MEMSEARCH_SAMPLE *sample, const char **err)
{
    if ( !sample || sample->size != s->size || sample->span_count != s->span_count ) {
        if ( err ) *err = "sample does not match the search space";
        return false;
    };
    return true;
}


/**
 * @brief Nastaví bity všech platných startovních offsetů.
 * @param s    Sezení.
 * @param cand Bitmapa (s->words slov, předem vynulovaná).
 * @return Počet nastavených bitů.
 */
static uint64_t ms_fill_all(const st_MEMSEARCH_SESSION *s, uint64_t *cand)
{
    uint64_t count = 0;
    unsigned w = s->fmt.width;
    for ( unsigned i = 0; i < s->span_count; i++ ) {
        uint32_t len = s->spans[i].length;
        if ( len < w ) continue;
        uint32_t base = s->span_base[i];
        uint32_t step = s->fmt.aligned ? w : 1;
        for ( uint32_t o = 0; o + w <= len; o += step ) {
            uint32_t bit = base + o;
            cand[bit >> 6] |= (uint64_t) 1 << ( bit & 63 );
            count++;
        };
    };
    return count;
}


/**
 * @brief Projde kandidáty a vyřadí ty, které filtr nesplní.
 *
 * Kandidáti ve spanu, který je v @p cmp nebo v referenčním vzorku
 * odpojený, zůstávají.
 *
 * @param s   Sezení (mění se s->st.cand a cand_count).
 * @param f   Filtr (platný).
 * @param cmp Porovnávaný vzorek.
 * @param ref Referenční vzorek (PREVIOUS / FIRST), u VALUE se nepoužije.
 * @return Počet vyřazených kandidátů.
 */
static uint64_t ms_apply(st_MEMSEARCH_SESSION *s, const st_MEMSEARCH_FILTER *f,
                         const st_MEMSEARCH_SAMPLE *cmp,
                         const st_MEMSEARCH_SAMPLE *ref)
{
    uint64_t removed = 0;
    bool use_ref = ( f->compare_to != MEMSEARCH_CMP_VALUE );
    for ( unsigned i = 0; i < s->span_count; i++ ) {
        if ( !cmp->span_connected[i] ) continue;
        if ( use_ref && !ref->span_connected[i] ) continue;
        uint32_t lo = s->span_base[i];
        uint32_t hi = s->span_base[i + 1];
        uint32_t bit = lo;
        while ( bit < hi ) {
            size_t wi = bit >> 6;
            uint64_t word = s->st.cand[wi] >> ( bit & 63 );
            if ( word == 0 ) {
                bit = (uint32_t) ( ( wi + 1 ) << 6 );
                continue;
            };
            bit += (uint32_t) __builtin_ctzll ( word );
            if ( bit >= hi ) break;
            const uint8_t *rb = use_ref ? &ref->data[bit] : NULL;
            if ( !ms_eval_bytes ( &s->fmt, f, &cmp->data[bit], rb ) ) {
                s->st.cand[bit >> 6] &= ~( (uint64_t) 1 << ( bit & 63 ) );
                removed++;
            };
            bit++;
        };
    };
    s->st.cand_count -= removed;
    return removed;
}


bool memsearch_start(st_MEMSEARCH_SESSION *s, st_MEMSEARCH_SAMPLE *sample,
                     const st_MEMSEARCH_FILTER *initial, const char **err)
{
    const char *e = NULL;
    if ( err ) *err = NULL;
    if ( !s ) return false;
    if ( !ms_sample_ok ( s, sample, err ) ) return false;
    if ( initial ) {
        if ( !memsearch_filter_validate ( initial, &s->fmt, &e ) ) {
            if ( err ) *err = e;
            return false;
        };
        if ( initial->compare_to != MEMSEARCH_CMP_VALUE ) {
            if ( err ) *err = "the first filter can only compare with a value";
            return false;
        };
    };

    uint64_t *cand = g_try_malloc0 ( s->words * sizeof ( uint64_t ) );
    if ( !cand || !ms_push_undo ( s ) ) {
        g_free ( cand );
        if ( err ) *err = "out of memory";
        return false;
    };

    unsigned step = s->st.step;
    ms_state_clear ( &s->st );
    s->st.started = true;
    s->st.cand = cand;
    s->st.cand_count = ms_fill_all ( s, cand );
    s->st.first = memsearch_sample_ref ( sample );
    s->st.prev = memsearch_sample_ref ( sample );
    s->st.cur = memsearch_sample_ref ( sample );
    s->st.step = step + 1;
    if ( initial ) (void) ms_apply ( s, initial, s->st.cur, NULL );
    ms_index_invalidate ( s );
    return true;
}


bool memsearch_sample(st_MEMSEARCH_SESSION *s, st_MEMSEARCH_SAMPLE *sample,
                      const char **err)
{
    if ( err ) *err = NULL;
    if ( !s || !s->st.started ) {
        if ( err ) *err = "search not started";
        return false;
    };
    if ( !ms_sample_ok ( s, sample, err ) ) return false;
    if ( !ms_push_undo ( s ) ) {
        if ( err ) *err = "out of memory";
        return false;
    };
    memsearch_sample_unref ( s->st.prev );
    s->st.prev = s->st.cur;
    s->st.cur = memsearch_sample_ref ( sample );
    s->st.step++;
    return true;
}


bool memsearch_filter(st_MEMSEARCH_SESSION *s, st_MEMSEARCH_SAMPLE *sample,
                      const st_MEMSEARCH_FILTER *f, uint64_t *removed,
                      const char **err)
{
    const char *e = NULL;
    if ( err ) *err = NULL;
    if ( removed ) *removed = 0;
    if ( !s || !s->st.started ) {
        if ( err ) *err = "search not started";
        return false;
    };
    if ( !memsearch_filter_validate ( f, &s->fmt, &e ) ) {
        if ( err ) *err = e;
        return false;
    };
    if ( sample && !ms_sample_ok ( s, sample, err ) ) return false;
    if ( !ms_push_undo ( s ) ) {
        if ( err ) *err = "out of memory";
        return false;
    };

    const st_MEMSEARCH_SAMPLE *cmp = sample ? sample : s->st.cur;
    const st_MEMSEARCH_SAMPLE *ref;
    if ( f->compare_to == MEMSEARCH_CMP_FIRST ) ref = s->st.first;
    else ref = sample ? s->st.cur : s->st.prev;

    uint64_t r = ms_apply ( s, f, cmp, ref );
    if ( sample ) {
        memsearch_sample_unref ( s->st.prev );
        s->st.prev = s->st.cur;
        s->st.cur = memsearch_sample_ref ( sample );
    };
    s->st.step++;
    ms_index_invalidate ( s );
    if ( removed ) *removed = r;
    return true;
}


int memsearch_eval(const st_MEMSEARCH_SESSION *s, uint32_t offset,
                   const st_MEMSEARCH_FILTER *f)
{
    if ( !memsearch_is_candidate ( s, offset ) || !f ) return -1;
    st_MEMSEARCH_ITEM it;
    if ( !memsearch_item ( s, offset, &it ) ) return -1;
    if ( !s->st.cur->span_connected[it.span_index] ) return 1;
    const st_MEMSEARCH_SAMPLE *ref = ( f->compare_to == MEMSEARCH_CMP_FIRST )
                                     ? s->st.first : s->st.prev;
    if ( f->compare_to != MEMSEARCH_CMP_VALUE && !ref->span_connected[it.span_index] ) return 1;
    const uint8_t *rb = ( f->compare_to != MEMSEARCH_CMP_VALUE ) ? &ref->data[offset] : NULL;
    return ms_eval_bytes ( &s->fmt, f, &s->st.cur->data[offset], rb ) ? 1 : 0;
}


uint64_t memsearch_eliminate(st_MEMSEARCH_SESSION *s, const uint32_t *offsets,
                             unsigned n)
{
    if ( !s || !s->st.started || !offsets ) return 0;
    bool any = false;
    for ( unsigned i = 0; i < n && !any; i++ ) {
        if ( memsearch_is_candidate ( s, offsets[i] ) ) any = true;
    };
    if ( !any || !ms_push_undo ( s ) ) return 0;

    uint64_t removed = 0;
    for ( unsigned i = 0; i < n; i++ ) {
        uint32_t o = offsets[i];
        if ( memsearch_is_candidate ( s, o ) ) {
            s->st.cand[o >> 6] &= ~( (uint64_t) 1 << ( o & 63 ) );
            removed++;
        };
    };
    s->st.cand_count -= removed;
    s->st.step++;
    ms_index_invalidate ( s );
    return removed;
}


bool memsearch_undo(st_MEMSEARCH_SESSION *s)
{
    if ( !s || s->undo_n == 0 || s->redo_n >= MEMSEARCH_UNDO_MAX_DEPTH ) return false;
    s->redo[s->redo_n++] = s->st;
    s->st = s->undo[--s->undo_n];
    memset ( &s->undo[s->undo_n], 0, sizeof ( s->undo[0] ) );
    ms_index_invalidate ( s );
    return true;
}


bool memsearch_redo(st_MEMSEARCH_SESSION *s)
{
    if ( !s || s->redo_n == 0 ) return false;
    if ( s->undo_n == s->undo_cap ) {
        ms_state_clear ( &s->undo[0] );
        memmove ( &s->undo[0], &s->undo[1],
                  ( s->undo_n - 1 ) * sizeof ( s->undo[0] ) );
        s->undo_n--;
    };
    s->undo[s->undo_n++] = s->st;
    s->st = s->redo[--s->redo_n];
    memset ( &s->redo[s->redo_n], 0, sizeof ( s->redo[0] ) );
    ms_index_invalidate ( s );
    return true;
}


bool memsearch_is_candidate(const st_MEMSEARCH_SESSION *s, uint32_t offset)
{
    if ( !s || !s->st.started || offset >= s->size ) return false;
    return ( s->st.cand[offset >> 6] >> ( offset & 63 ) ) & 1u;
}


/**
 * @brief Najde offset prvního kandidáta >= @p from.
 * @param s    Sezení (started).
 * @param from Počáteční offset.
 * @param[out] out Offset kandidáta.
 * @return false = žádný další kandidát.
 */
static bool ms_next_candidate(const st_MEMSEARCH_SESSION *s, uint32_t from,
                              uint32_t *out)
{
    uint32_t bit = from;
    while ( bit < s->size ) {
        size_t wi = bit >> 6;
        uint64_t word = s->st.cand[wi] >> ( bit & 63 );
        if ( word == 0 ) {
            bit = (uint32_t) ( ( wi + 1 ) << 6 );
            continue;
        };
        bit += (uint32_t) __builtin_ctzll ( word );
        if ( bit >= s->size ) return false;
        *out = bit;
        return true;
    };
    return false;
}


bool memsearch_nth_candidate(st_MEMSEARCH_SESSION *s, uint64_t n,
                             uint32_t *out_offset)
{
    if ( !s || !s->st.started || n >= s->st.cand_count || !out_offset ) return false;

    if ( s->st.cand_count <= MEMSEARCH_INDEX_MAX ) {
        if ( !s->index_valid ) {
            g_free ( s->index );
            s->index = g_try_new ( uint32_t, s->st.cand_count ? s->st.cand_count : 1 );
            s->index_n = 0;
            if ( s->index ) {
                uint32_t o = 0;
                while ( s->index_n < s->st.cand_count && ms_next_candidate ( s, o, &o ) ) {
                    s->index[s->index_n++] = o;
                    o++;
                };
                s->index_valid = true;
            };
        };
        if ( s->index_valid ) {
            *out_offset = s->index[n];
            return true;
        };
    };

    /* Bez indexu: přeskočit celá slova podle počtu bitů. */
    uint64_t left = n;
    for ( size_t wi = 0; wi < s->words; wi++ ) {
        uint64_t word = s->st.cand[wi];
        unsigned pc = (unsigned) __builtin_popcountll ( word );
        if ( left >= pc ) {
            left -= pc;
            continue;
        };
        while ( left-- ) word &= word - 1;
        *out_offset = (uint32_t) ( ( wi << 6 ) + (size_t) __builtin_ctzll ( word ) );
        return true;
    };
    return false;
}


/**
 * @brief Index spanu, ve kterém leží offset (půlení intervalu).
 * @param s      Sezení.
 * @param offset Offset < s->size.
 * @return Index spanu.
 */
static unsigned ms_span_of(const st_MEMSEARCH_SESSION *s, uint32_t offset)
{
    unsigned lo = 0, hi = s->span_count;
    while ( hi - lo > 1 ) {
        unsigned mid = ( lo + hi ) / 2;
        if ( s->span_base[mid] <= offset ) lo = mid;
        else hi = mid;
    };
    return lo;
}


/**
 * @brief Hodnota kandidáta ze vzorku.
 * @param s      Sezení.
 * @param sample Vzorek (smí být NULL).
 * @param span   Index spanu.
 * @param offset Offset v prostoru.
 * @param[out] out Hodnota.
 */
static void ms_value(const st_MEMSEARCH_SESSION *s,
                     const st_MEMSEARCH_SAMPLE *sample, unsigned span,
                     uint32_t offset, st_MEMSEARCH_VALUE *out)
{
    memset ( out, 0, sizeof ( *out ) );
    if ( !sample ) return;
    uint32_t end = s->span_base[span + 1];
    unsigned w = s->fmt.width;
    unsigned n = ( offset + w <= end ) ? w : (unsigned) ( end - offset );
    memcpy ( out->raw, &sample->data[offset], n );
    if ( n == w && sample->span_connected[span] ) {
        out->valid = memsearch_decode ( &s->fmt, out->raw, &out->value );
    };
}


bool memsearch_item(const st_MEMSEARCH_SESSION *s, uint32_t offset,
                    st_MEMSEARCH_ITEM *out)
{
    if ( !s || !s->st.started || offset >= s->size || !out ) return false;
    unsigned span = ms_span_of ( s, offset );
    memset ( out, 0, sizeof ( *out ) );
    out->offset = offset;
    out->span_index = span;
    out->region_kind = s->spans[span].region_kind;
    out->sub_id = s->spans[span].sub_id;
    out->region_offset = s->spans[span].offset + ( offset - s->span_base[span] );
    out->connected = s->st.cur->span_connected[span] != 0;
    ms_value ( s, s->st.cur, span, offset, &out->cur );
    ms_value ( s, s->st.prev, span, offset, &out->prev );
    ms_value ( s, s->st.first, span, offset, &out->first );
    return true;
}


bool memsearch_find_offset(const st_MEMSEARCH_SESSION *s, int region_kind,
                           int sub_id, uint32_t region_offset,
                           uint32_t *out_offset)
{
    if ( !s || !out_offset ) return false;
    for ( unsigned i = 0; i < s->span_count; i++ ) {
        const st_MEMSEARCH_SPAN *sp = &s->spans[i];
        if ( sp->region_kind != region_kind || sp->sub_id != sub_id ) continue;
        if ( region_offset < sp->offset ) continue;
        uint32_t d = region_offset - sp->offset;
        if ( d >= sp->length ) continue;
        *out_offset = s->span_base[i] + d;
        return true;
    };
    return false;
}


/* ------------------------------------------------------------------ */
/* Textové názvy                                                       */
/* ------------------------------------------------------------------ */

/** @brief Názvy operací v pořadí en_MEMSEARCH_OP. */
static const char *const s_op_names[MEMSEARCH_OP_COUNT] = {
    "eq", "ne", "lt", "le", "gt", "ge", "between", "inc", "dec",
    "inc_by", "dec_by", "changed", "unchanged", "changed_by", "mask_eq",
};

/** @brief Názvy referencí v pořadí en_MEMSEARCH_CMP. */
static const char *const s_cmp_names[MEMSEARCH_CMP_COUNT] = {
    "value", "previous", "first",
};

/** @brief Názvy kódování číslic v pořadí en_MEMSEARCH_DIGIT_ENC. */
static const char *const s_enc_names[MEMSEARCH_DIGITS_COUNT] = {
    "raw", "ascii", "display",
};


/**
 * @brief Najde řetězec v tabulce názvů.
 * @return Index, nebo -1.
 */
static int ms_lookup(const char *const *names, int n, const char *name)
{
    if ( !name ) return -1;
    for ( int i = 0; i < n; i++ ) {
        if ( strcmp ( names[i], name ) == 0 ) return i;
    };
    return -1;
}


const char *memsearch_op_name(en_MEMSEARCH_OP op)
{
    return ( (unsigned) op < MEMSEARCH_OP_COUNT ) ? s_op_names[op] : NULL;
}

int memsearch_op_from_name(const char *name)
{
    return ms_lookup ( s_op_names, MEMSEARCH_OP_COUNT, name );
}

const char *memsearch_cmp_name(en_MEMSEARCH_CMP c)
{
    return ( (unsigned) c < MEMSEARCH_CMP_COUNT ) ? s_cmp_names[c] : NULL;
}

int memsearch_cmp_from_name(const char *name)
{
    return ms_lookup ( s_cmp_names, MEMSEARCH_CMP_COUNT, name );
}

const char *memsearch_digit_enc_name(en_MEMSEARCH_DIGIT_ENC e)
{
    return ( (unsigned) e < MEMSEARCH_DIGITS_COUNT ) ? s_enc_names[e] : NULL;
}

int memsearch_digit_enc_from_name(const char *name)
{
    return ms_lookup ( s_enc_names, MEMSEARCH_DIGITS_COUNT, name );
}


bool memsearch_type_from_name(const char *name, en_MEMSEARCH_TYPE *type,
                              unsigned *width)
{
    static const struct { const char *name; en_MEMSEARCH_TYPE type; unsigned width; } tab[] = {
        { "u8",  MEMSEARCH_TYPE_UINT, 1 }, { "s8",  MEMSEARCH_TYPE_SINT, 1 },
        { "u16", MEMSEARCH_TYPE_UINT, 2 }, { "s16", MEMSEARCH_TYPE_SINT, 2 },
        { "u24", MEMSEARCH_TYPE_UINT, 3 }, { "s24", MEMSEARCH_TYPE_SINT, 3 },
        { "u32", MEMSEARCH_TYPE_UINT, 4 }, { "s32", MEMSEARCH_TYPE_SINT, 4 },
        { "bcd", MEMSEARCH_TYPE_BCD, 0 },  { "digits", MEMSEARCH_TYPE_DIGITS, 0 },
    };
    if ( !name ) return false;
    for ( size_t i = 0; i < G_N_ELEMENTS ( tab ); i++ ) {
        if ( strcmp ( tab[i].name, name ) == 0 ) {
            if ( type ) *type = tab[i].type;
            if ( width ) *width = tab[i].width;
            return true;
        };
    };
    return false;
}


/* ------------------------------------------------------------------ */
/* Předvolby prostoru                                                  */
/* ------------------------------------------------------------------ */

/** @brief Názvy předvoleb v pořadí en_MEMSEARCH_PRESET. */
static const char *const s_preset_names[MEMSEARCH_PRESET_COUNT] = {
    "ram", "logical", "vram", "memext", "ramdisk", "all_writable",
};


const char *memsearch_preset_name(en_MEMSEARCH_PRESET p)
{
    return ( (unsigned) p < MEMSEARCH_PRESET_COUNT ) ? s_preset_names[p] : NULL;
}

int memsearch_preset_from_name(const char *name)
{
    return ms_lookup ( s_preset_names, MEMSEARCH_PRESET_COUNT, name );
}


/**
 * @brief true = druh regionu patří do předvolby.
 * @param p    Předvolba.
 * @param kind en_REGION_KIND.
 */
static bool ms_preset_has_kind(en_MEMSEARCH_PRESET p, int kind)
{
    switch ( p ) {
        case MEMSEARCH_PRESET_RAM:
            return kind == REGION_KIND_RAM;
        case MEMSEARCH_PRESET_LOGICAL:
            return kind == REGION_KIND_LOGICAL;
        case MEMSEARCH_PRESET_VRAM:
            return kind == REGION_KIND_VRAM_700_CHAR || kind == REGION_KIND_VRAM_700_ATTR
                   || kind == REGION_KIND_VRAM_PHYS_PLANE;
        case MEMSEARCH_PRESET_MEMEXT:
            return kind == REGION_KIND_MEMEXT_RAM;
        case MEMSEARCH_PRESET_RAMDISK:
            return kind == REGION_KIND_RAMDISK_STD || kind == REGION_KIND_RAMDISK_PEZIK;
        case MEMSEARCH_PRESET_ALL_WRITABLE:
            return kind == REGION_KIND_RAM
                   || kind == REGION_KIND_VRAM_700_CHAR || kind == REGION_KIND_VRAM_700_ATTR
                   || kind == REGION_KIND_VRAM_PHYS_PLANE || kind == REGION_KIND_CGRAM_700
                   || kind == REGION_KIND_PCG_1500 || kind == REGION_KIND_MEMEXT_RAM
                   || kind == REGION_KIND_RAMDISK_STD || kind == REGION_KIND_RAMDISK_PEZIK;
        default:
            return false;
    };
}


unsigned memsearch_spans_from_preset(en_MEMSEARCH_PRESET p, const void *regs,
                                     int count, st_MEMSEARCH_SPAN *out)
{
    const st_REGION_DESC *r = (const st_REGION_DESC *) regs;
    unsigned n = 0;
    if ( !r || !out ) return 0;
    for ( int i = 0; i < count; i++ ) {
        if ( !r[i].connected || r[i].size == 0 ) continue;
        if ( !ms_preset_has_kind ( p, (int) r[i].kind ) ) continue;
        out[n].region_kind = (int) r[i].kind;
        out[n].sub_id = r[i].sub_id;
        out[n].offset = 0;
        out[n].length = r[i].size;
        n++;
    };
    return n;
}



/* ------------------------------------------------------------------ */
/* Převod kandidáta na adresu Z80 a write breakpoint                   */
/* ------------------------------------------------------------------ */

bool memsearch_target(int region_kind, int sub_id, uint32_t region_offset,
                      unsigned width, const st_MEMSEARCH_MEMEXT *mx,
                      st_MEMSEARCH_TARGET *out)
{
    if ( !out ) return false;
    memset ( out, 0, sizeof ( *out ) );
    out->bp_bank = -1;
    if ( width < 1 ) width = 1;
    int32_t z80 = -1;
    switch ( (en_REGION_KIND) region_kind ) {
        case REGION_KIND_RAM:
            z80 = (int32_t) region_offset;
            out->bp_zone = MEMSEARCH_BPZONE_RAM;
            break;
        case REGION_KIND_LOGICAL:
            z80 = (int32_t) region_offset;
            out->bp_zone = MEMSEARCH_BPZONE_CPU_VIEW;
            break;
        case REGION_KIND_VRAM_700_CHAR:
            z80 = 0xD000 + (int32_t) region_offset;
            out->bp_zone = MEMSEARCH_BPZONE_VRAM_FB;
            break;
        case REGION_KIND_VRAM_700_ATTR:
            z80 = 0xD800 + (int32_t) region_offset;
            out->bp_zone = MEMSEARCH_BPZONE_VRAM_FB;
            break;
        case REGION_KIND_MEMEXT_RAM:
            if ( mx && mx->connected && mx->map_valid ) {
                for ( int slot = 0; slot < 16; slot++ ) {
                    if ( (int) mx->map[slot] == sub_id ) {
                        z80 = slot * 0x1000 + (int32_t) region_offset;
                        break;
                    };
                };
            };
            if ( mx && mx->connected && mx->pehu ) {
                out->bp_zone = MEMSEARCH_BPZONE_MMEXT_BANK;
                out->bp_bank = sub_id >> 1;
                out->bp_addr = (uint16_t) ( ( sub_id & 1 ) * 0x1000 + region_offset );
                out->bp_addr_end = (uint16_t) ( out->bp_addr + width - 1 );
            } else if ( z80 >= 0 ) {
                out->bp_zone = MEMSEARCH_BPZONE_CPU_VIEW;
            };
            break;
        default:
            break;
    };
    if ( z80 >= 0 && z80 <= 0xFFFF ) {
        out->has_z80 = true;
        out->z80_addr = (uint16_t) z80;
        if ( out->bp_zone != MEMSEARCH_BPZONE_MMEXT_BANK && out->bp_zone != MEMSEARCH_BPZONE_NONE ) {
            out->bp_addr = (uint16_t) z80;
            out->bp_addr_end = (uint16_t) ( z80 + width - 1 );
        };
    } else if ( out->bp_zone != MEMSEARCH_BPZONE_MMEXT_BANK ) {
        out->bp_zone = MEMSEARCH_BPZONE_NONE;
    };
    return out->has_z80 || out->bp_zone != MEMSEARCH_BPZONE_NONE;
}


const char *memsearch_bpzone_name(en_MEMSEARCH_BPZONE z)
{
    switch ( z ) {
        case MEMSEARCH_BPZONE_CPU_VIEW:   return "CPU_VIEW";
        case MEMSEARCH_BPZONE_RAM:        return "RAM";
        case MEMSEARCH_BPZONE_VRAM_FB:    return "VRAM_FB";
        case MEMSEARCH_BPZONE_MMEXT_BANK: return "MMEXT_BANK";
        default:                          return NULL;
    };
}
