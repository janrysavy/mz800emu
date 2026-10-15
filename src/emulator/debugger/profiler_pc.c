/**
 * @file   profiler_pc.c
 * @brief  Profiler podle PC - implementace (viz profiler_pc.h).
 *
 * Data jsou tři statická pole po 65536 položkách (1,5 MiB). Háček po
 * instrukci jen přičítá; řazení a výběr se dělají až při dotazu.
 *
 * @par Licence: GPLv3
 */

#include "mzarch/mzarch_config.h"
#include "profiler_pc.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "libs/cpu-z80/z80.h"
#include "emulator/mzarch/mzarch.h"


int g_profiler_pc_active = 0;

/** @brief Počet vykonání instrukce začínající na adrese. */
static uint64_t s_count[0x10000];
/** @brief Součet T-stavů (včetně WAIT) instrukcí na adrese. */
static uint64_t s_tstates[0x10000];
/** @brief Součet čekacích stavů instrukcí na adrese. */
static uint64_t s_wait[0x10000];

/**
 * @brief Souhrnné čítače a stav sledování času.
 *
 * @invariant instructions == součet s_count, tstates == součet s_tstates,
 *            wait == součet s_wait.
 * @invariant elapsed >= tstates + halt_idle (rozdíl = čas mimo instrukce).
 */
static struct {
    uint64_t instructions; /**< Vykonané instrukce celkem. */
    uint64_t tstates;      /**< T-stavy instrukcí celkem (včetně WAIT). */
    uint64_t wait;         /**< Čekací stavy celkem. */
    uint64_t halt_idle;    /**< T-stavy nečinného HALT. */
    uint64_t elapsed;      /**< Uplynulý čas sběru [T-stavy CPU]. */
    uint32_t last_total;  /**< cpu->total_cycles po předchozí instrukci. */
    bool prev_halted;     /**< CPU byl po předchozí instrukci v HALT. */
    uint16_t prev_pc;     /**< Adresa předchozí instrukce. */
} s_pp;


/** @brief Aktuální cpu->total_cycles (0 bez CPU). */
static uint32_t pp_now ( void )
{
    return g_mzarch_main.cpu ? g_mzarch_main.cpu->total_cycles : 0u;
}


/** @brief Začne nový úsek měření času (bez návaznosti na předchozí instrukci). */
static void pp_restart_timing ( void )
{
    s_pp.last_total = pp_now ( );
    s_pp.prev_halted = false;
    s_pp.prev_pc = 0;
}


void profiler_pc_set_active ( bool enable )
{
    if ( enable && !g_profiler_pc_active ) {
        pp_restart_timing ( );
    };
    g_profiler_pc_active = enable ? 1 : 0;
}


void profiler_pc_reset ( void )
{
    memset ( s_count, 0, sizeof ( s_count ) );
    memset ( s_tstates, 0, sizeof ( s_tstates ) );
    memset ( s_wait, 0, sizeof ( s_wait ) );
    s_pp.instructions = 0;
    s_pp.tstates = 0;
    s_pp.wait = 0;
    s_pp.halt_idle = 0;
    s_pp.elapsed = 0;
    pp_restart_timing ( );
}


void profiler_pc_on_instruction ( uint16_t pc, uint32_t tstates, uint32_t wait )
{
    uint32_t now = pp_now ( );
    uint32_t delta = now - s_pp.last_total;
    s_pp.last_total = now;

    /* Mezi instrukcemi smí přibýt jen přijetí přerušení; jinak jde
     * o nespojitost (reset CPU, snapshot) a počítá se jen instrukce. */
    if ( delta < tstates || delta - tstates > PROFILER_PC_GAP_MAX ) {
        delta = tstates;
    };
    s_pp.elapsed += delta;

    bool halted = g_mzarch_main.cpu && g_mzarch_main.cpu->halted;
    if ( s_pp.prev_halted && halted && pc == s_pp.prev_pc ) {
        /* Opakovaný fetch HALT - CPU nic nevykonává. */
        s_pp.halt_idle += tstates;
    } else {
        s_count[pc]++;
        s_tstates[pc] += tstates;
        s_wait[pc] += wait;
        s_pp.instructions++;
        s_pp.tstates += tstates;
        s_pp.wait += wait;
    };
    s_pp.prev_halted = halted;
    s_pp.prev_pc = pc;
}


void profiler_pc_get_summary ( st_PROFILER_PC_SUMMARY *out )
{
    if ( !out ) return;
    memset ( out, 0, sizeof ( *out ) );
    out->active = g_profiler_pc_active != 0;
    out->instructions = s_pp.instructions;
    out->tstates = s_pp.tstates;
    out->wait = s_pp.wait;
    out->halt_idle = s_pp.halt_idle;
    out->elapsed_tstates = s_pp.elapsed;
    uint64_t used = s_pp.tstates + s_pp.halt_idle;
    out->other_tstates = ( s_pp.elapsed > used ) ? ( s_pp.elapsed - used ) : 0u;
    unsigned n = 0;
    for ( unsigned a = 0; a < 0x10000u; a++ ) {
        if ( s_count[a] ) n++;
    };
    out->unique_pcs = n;
}


/** @brief Klíč řazení pro qsort (jedno vlákno -> statická proměnná stačí). */
static en_PROFILER_PC_SORT s_sort_key;


/** @brief Hodnota řádku podle aktuálního klíče řazení. */
static uint64_t pp_row_key ( const st_PROFILER_PC_ROW *r )
{
    switch ( s_sort_key ) {
        case PROFILER_PC_SORT_COUNT: return r->count;
        case PROFILER_PC_SORT_WAIT:  return r->wait;
        default:                     return r->tstates;
    };
}


/** @brief Porovnání pro qsort: sestupně podle klíče, při shodě vzestupně podle adresy. */
static int pp_row_cmp ( const void *a, const void *b )
{
    const st_PROFILER_PC_ROW *ra = (const st_PROFILER_PC_ROW *) a;
    const st_PROFILER_PC_ROW *rb = (const st_PROFILER_PC_ROW *) b;
    if ( s_sort_key != PROFILER_PC_SORT_ADDR ) {
        uint64_t ka = pp_row_key ( ra ), kb = pp_row_key ( rb );
        if ( ka != kb ) return ( ka > kb ) ? -1 : 1;
    };
    return (int) ra->pc - (int) rb->pc;
}


/**
 * @brief Seřazené řádky v rozsahu adres (alokuje pole, volající uvolní free()).
 * @param[out] n_out Počet řádků.
 * @return Pole, nebo NULL (žádný řádek nebo chyba alokace - pak *n_out = 0
 *         pro prázdný výsledek, (size_t)-1 pro chybu alokace).
 */
static st_PROFILER_PC_ROW *pp_collect ( en_PROFILER_PC_SORT sort, uint16_t lo, uint16_t hi, size_t *n_out )
{
    *n_out = 0;
    if ( hi < lo ) return NULL;
    size_t n = 0;
    for ( unsigned a = lo; a <= hi; a++ ) {
        if ( s_count[a] ) n++;
    };
    if ( n == 0 ) return NULL;
    st_PROFILER_PC_ROW *rows = (st_PROFILER_PC_ROW *) malloc ( n * sizeof ( *rows ) );
    if ( !rows ) {
        *n_out = (size_t) -1;
        return NULL;
    };
    size_t i = 0;
    for ( unsigned a = lo; a <= hi; a++ ) {
        if ( !s_count[a] ) continue;
        rows[i].pc = (uint16_t) a;
        rows[i].count = s_count[a];
        rows[i].tstates = s_tstates[a];
        rows[i].wait = s_wait[a];
        i++;
    };
    s_sort_key = sort;
    qsort ( rows, n, sizeof ( *rows ), pp_row_cmp );
    *n_out = n;
    return rows;
}


size_t profiler_pc_get_rows ( en_PROFILER_PC_SORT sort, uint16_t lo, uint16_t hi,
                              st_PROFILER_PC_ROW *out, size_t max )
{
    if ( hi < lo ) return 0;
    if ( !out || max == 0 ) {
        size_t n = 0;
        for ( unsigned a = lo; a <= hi; a++ ) {
            if ( s_count[a] ) n++;
        };
        return n;
    };
    size_t n = 0;
    st_PROFILER_PC_ROW *rows = pp_collect ( sort, lo, hi, &n );
    if ( !rows ) {
        /* Chyba alokace: vrátit aspoň počet bez řádků. */
        return ( n == (size_t) -1 ) ? profiler_pc_get_rows ( sort, lo, hi, NULL, 0 ) : 0;
    };
    memcpy ( out, rows, ( n < max ? n : max ) * sizeof ( *rows ) );
    free ( rows );
    return n;
}


int profiler_pc_export_csv ( const char *path, en_PROFILER_PC_SORT sort, char *err, size_t err_size )
{
    if ( err && err_size ) err[0] = '\0';
    if ( !path || !path[0] ) {
        if ( err ) snprintf ( err, err_size, "empty path" );
        return -1;
    };
    size_t n = 0;
    st_PROFILER_PC_ROW *rows = pp_collect ( sort, 0x0000, 0xFFFF, &n );
    if ( n == (size_t) -1 ) {
        if ( err ) snprintf ( err, err_size, "out of memory" );
        return -1;
    };
    /* g_fopen: UTF-8 cesta i na Windows; "wb" = konce řádků LF na všech platformách. */
    FILE *f = g_fopen ( path, "wb" );
    if ( !f ) {
        if ( err ) snprintf ( err, err_size, "cannot open '%s': %s", path, strerror ( errno ) );
        free ( rows );
        return -1;
    };
    fprintf ( f, "pc,count,tstates,wait,tstates_pct\n" );
    for ( size_t i = 0; i < n; i++ ) {
        double pct = s_pp.tstates ? ( 100.0 * (double) rows[i].tstates / (double) s_pp.tstates ) : 0.0;
        /* Desetinná tečka bez ohledu na locale (jinak "64,79" rozbije CSV). */
        char pct_buf[ G_ASCII_DTOSTR_BUF_SIZE ];
        g_ascii_formatd ( pct_buf, sizeof ( pct_buf ), "%.4f", pct );
        fprintf ( f, "%04X,%llu,%llu,%llu,%s\n", rows[i].pc,
                  (unsigned long long) rows[i].count,
                  (unsigned long long) rows[i].tstates,
                  (unsigned long long) rows[i].wait, pct_buf );
    };
    free ( rows );
    bool ok = !ferror ( f );
    if ( fclose ( f ) != 0 ) ok = false;
    if ( !ok ) {
        if ( err ) snprintf ( err, err_size, "write error on '%s'", path );
        return -1;
    };
    return (int) n;
}


#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
