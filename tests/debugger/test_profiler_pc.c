/**
 * @file   test_profiler_pc.c
 * @brief  Unit testy profileru podle PC (profiler_pc.h).
 *
 * Testy volají háček profiler_pc_on_instruction přímo, jako hlavní smyčka
 * po z80_step: před voláním posunou `cpu->total_cycles` o T-stavy instrukce
 * (a případně o přijetí přerušení před ní) a nastaví `cpu->halted`.
 *
 * Ověřuje:
 * - součty na adresu a souhrn (počet, T-stavy, WAIT, počet adres),
 * - nečinný HALT zvlášť (samotná instrukce HALT se počítá na adrese),
 * - čas mimo instrukce (přijetí přerušení) v other_tstates,
 * - nespojitost čítače (reset CPU) nezkreslí uplynulý čas,
 * - start uprostřed běhu neměří čas před startem,
 * - start nebo reset sběru uprostřed HALT nepočítá první fetch jako instrukci,
 * - výběr řádků (řazení, rozsah, jen počet) a export CSV,
 * - bitmapa vykonaných adres (bit na adresu, nečinný HALT bez dalšího bitu).
 *
 * @par Licence: GPLv3
 */

#include "mztest.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "debugger/profiler_pc.h"
#include "libs/cpu-z80/z80.h"
#include "mzarch/mzarch.h"


/**
 * @brief Simuluje dokončenou instrukci.
 *
 * @param pc     Adresa začátku instrukce.
 * @param t      T-stavy instrukce včetně WAIT.
 * @param wait   Čekací stavy.
 * @param before T-stavy mimo instrukce před ní (přijetí přerušení).
 * @param halted Stav cpu->halted po instrukci.
 */
static void step ( uint16_t pc, uint32_t t, uint32_t wait, uint32_t before, bool halted )
{
    g_mzarch_main.cpu->total_cycles += before + t;
    g_mzarch_main.cpu->halted = halted;
    profiler_pc_on_instruction ( pc, t, wait );
}


void setUp ( void )
{
    g_mzarch_main.cpu->total_cycles = 1000;
    g_mzarch_main.cpu->halted = false;
    profiler_pc_reset ( );
    profiler_pc_set_active ( true );
}


void tearDown ( void )
{
    profiler_pc_set_active ( false );
    profiler_pc_reset ( );
    g_mzarch_main.cpu->halted = false;
}


/** @brief Součty na adresu a souhrn. */
void test_profiler_pc_aggregation ( void )
{
    TEST_ASSERT_TRUE ( g_profiler_pc_active );
    step ( 0x1000, 4, 0, 0, false );
    step ( 0x1001, 13, 3, 0, false );
    step ( 0x1000, 4, 0, 0, false );

    st_PROFILER_PC_SUMMARY s;
    profiler_pc_get_summary ( &s );
    TEST_ASSERT_TRUE ( s.active );
    TEST_ASSERT_EQUAL_UINT64 ( 3, s.instructions );
    TEST_ASSERT_EQUAL_UINT64 ( 21, s.tstates );
    TEST_ASSERT_EQUAL_UINT64 ( 3, s.wait );
    TEST_ASSERT_EQUAL_UINT64 ( 21, s.elapsed_tstates );
    TEST_ASSERT_EQUAL_UINT64 ( 0, s.other_tstates );
    TEST_ASSERT_EQUAL_UINT ( 2, s.unique_pcs );

    st_PROFILER_PC_ROW rows[ 4 ];
    TEST_ASSERT_EQUAL_size_t ( 2, profiler_pc_get_rows ( PROFILER_PC_SORT_ADDR, 0, 0xFFFF, rows, 4 ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1000, rows[ 0 ].pc );
    TEST_ASSERT_EQUAL_UINT64 ( 2, rows[ 0 ].count );
    TEST_ASSERT_EQUAL_UINT64 ( 8, rows[ 0 ].tstates );
    TEST_ASSERT_EQUAL_HEX16 ( 0x1001, rows[ 1 ].pc );
    TEST_ASSERT_EQUAL_UINT64 ( 13, rows[ 1 ].tstates );
    TEST_ASSERT_EQUAL_UINT64 ( 3, rows[ 1 ].wait );
}


/**
 * @brief HALT: vykonání instrukce HALT na adrese, opakované fetche jako
 *        halt_idle, přijetí přerušení jako other_tstates.
 */
void test_profiler_pc_halt_and_irq ( void )
{
    step ( 0x2000, 4, 0, 0, true );   /* HALT vykonán, CPU drží PC = 2000h */
    step ( 0x2000, 4, 0, 0, true );   /* nečinný HALT */
    step ( 0x2000, 4, 0, 0, true );   /* nečinný HALT */
    step ( 0x0038, 11, 0, 13, false ); /* přijetí IM1 (13 T) + RST obsluhy */

    st_PROFILER_PC_SUMMARY s;
    profiler_pc_get_summary ( &s );
    TEST_ASSERT_EQUAL_UINT64 ( 2, s.instructions );
    TEST_ASSERT_EQUAL_UINT64 ( 15, s.tstates );
    TEST_ASSERT_EQUAL_UINT64 ( 8, s.halt_idle );
    TEST_ASSERT_EQUAL_UINT64 ( 13, s.other_tstates );
    TEST_ASSERT_EQUAL_UINT64 ( 36, s.elapsed_tstates );
    TEST_ASSERT_EQUAL_UINT64 ( s.elapsed_tstates, s.tstates + s.halt_idle + s.other_tstates );

    st_PROFILER_PC_ROW r;
    TEST_ASSERT_EQUAL_size_t ( 1, profiler_pc_get_rows ( PROFILER_PC_SORT_ADDR, 0x2000, 0x2000, &r, 1 ) );
    TEST_ASSERT_EQUAL_UINT64 ( 1, r.count );
    TEST_ASSERT_EQUAL_UINT64 ( 4, r.tstates );
}


/** @brief Reset CPU (total_cycles = 0) ani velká mezera nezkreslí uplynulý čas. */
void test_profiler_pc_discontinuity ( void )
{
    step ( 0x3000, 7, 0, 0, false );
    g_mzarch_main.cpu->total_cycles = 0;      /* reset CPU */
    step ( 0x0000, 4, 0, 0, false );
    step ( 0x0001, 4, 0, PROFILER_PC_GAP_MAX + 1, false ); /* snapshot apod. */

    st_PROFILER_PC_SUMMARY s;
    profiler_pc_get_summary ( &s );
    TEST_ASSERT_EQUAL_UINT64 ( 15, s.tstates );
    TEST_ASSERT_EQUAL_UINT64 ( 15, s.elapsed_tstates );
    TEST_ASSERT_EQUAL_UINT64 ( 0, s.other_tstates );
}


/** @brief Start uprostřed běhu: čas před startem se nepočítá; stop data drží. */
void test_profiler_pc_start_stop ( void )
{
    profiler_pc_set_active ( false );
    g_mzarch_main.cpu->total_cycles += 500;   /* běh bez profileru */
    profiler_pc_set_active ( true );
    step ( 0x4000, 4, 0, 0, false );

    st_PROFILER_PC_SUMMARY s;
    profiler_pc_get_summary ( &s );
    TEST_ASSERT_EQUAL_UINT64 ( 4, s.elapsed_tstates );

    profiler_pc_set_active ( false );
    profiler_pc_get_summary ( &s );
    TEST_ASSERT_FALSE ( s.active );
    TEST_ASSERT_EQUAL_UINT64 ( 1, s.instructions );

    profiler_pc_reset ( );
    profiler_pc_get_summary ( &s );
    TEST_ASSERT_EQUAL_UINT64 ( 0, s.instructions );
    TEST_ASSERT_EQUAL_UINT ( 0, s.unique_pcs );
}


/**
 * @brief Start sběru, když CPU už stojí v HALT: první opakovaný fetch HALT
 *        je nečinnost, ne nová instrukce.
 *
 * Jádro během HALT drží PC = adresa HALT a hlavní smyčka předává háčku
 * cpu->pc z doby před krokem, takže i první krok po startu dostane adresu
 * HALT. Dřív začátek úseku nastavil prev_halted = false a první fetch se
 * započetl jako instrukce (reprodukce přes MCP:
 * DI; HALT; JR $ na 1200h, profiler_pc_start s reset, step_n 3).
 */
void test_profiler_pc_start_inside_halt ( void )
{
    profiler_pc_set_active ( false );
    profiler_pc_reset ( );
    g_mzarch_main.cpu->pc = 0x1200;     /* jádro drží PC = adresa HALT */
    g_mzarch_main.cpu->halted = true;     /* HALT vykonán před startem */
    profiler_pc_set_active ( true );

    for ( int i = 0; i < 3; i++ ) step ( 0x1200, 4, 0, 0, true );

    st_PROFILER_PC_SUMMARY s;
    profiler_pc_get_summary ( &s );
    TEST_ASSERT_EQUAL_UINT64 ( 0, s.instructions );
    TEST_ASSERT_EQUAL_UINT64 ( 0, s.tstates );
    TEST_ASSERT_EQUAL_UINT64 ( 12, s.halt_idle );
    TEST_ASSERT_EQUAL_UINT64 ( 12, s.elapsed_tstates );
    TEST_ASSERT_EQUAL_UINT ( 0, s.unique_pcs );
}


/** @brief Reset za běhu sběru uprostřed HALT: totéž jako start v HALT. */
void test_profiler_pc_reset_active_inside_halt ( void )
{
    step ( 0x2000, 4, 0, 0, true );       /* HALT vykonán, CPU drží PC = 2000h */
    g_mzarch_main.cpu->pc = 0x2000;
    profiler_pc_reset ( );                /* sběr dál aktivní */

    step ( 0x2000, 4, 0, 0, true );
    step ( 0x2000, 4, 0, 0, true );

    st_PROFILER_PC_SUMMARY s;
    profiler_pc_get_summary ( &s );
    TEST_ASSERT_TRUE ( s.active );
    TEST_ASSERT_EQUAL_UINT64 ( 0, s.instructions );
    TEST_ASSERT_EQUAL_UINT64 ( 8, s.halt_idle );
    TEST_ASSERT_EQUAL_UINT ( 0, s.unique_pcs );
}


/** @brief Řazení, rozsah, kapacita a dotaz jen na počet. */
void test_profiler_pc_rows_sort_range ( void )
{
    for ( int i = 0; i < 3; i++ ) step ( 0x5000, 4, 0, 0, false );  /* count 3, T 12 */
    step ( 0x5010, 23, 0, 0, false );                                  /* count 1, T 23 */
    step ( 0x6000, 10, 6, 0, false );                                  /* T 10, wait 6 */

    st_PROFILER_PC_ROW rows[ 3 ];
    TEST_ASSERT_EQUAL_size_t ( 3, profiler_pc_get_rows ( PROFILER_PC_SORT_TSTATES, 0, 0xFFFF, rows, 3 ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x5010, rows[ 0 ].pc );
    TEST_ASSERT_EQUAL_HEX16 ( 0x5000, rows[ 1 ].pc );
    TEST_ASSERT_EQUAL_HEX16 ( 0x6000, rows[ 2 ].pc );

    TEST_ASSERT_EQUAL_size_t ( 3, profiler_pc_get_rows ( PROFILER_PC_SORT_COUNT, 0, 0xFFFF, rows, 1 ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x5000, rows[ 0 ].pc );

    TEST_ASSERT_EQUAL_size_t ( 3, profiler_pc_get_rows ( PROFILER_PC_SORT_WAIT, 0, 0xFFFF, rows, 1 ) );
    TEST_ASSERT_EQUAL_HEX16 ( 0x6000, rows[ 0 ].pc );

    TEST_ASSERT_EQUAL_size_t ( 2, profiler_pc_get_rows ( PROFILER_PC_SORT_ADDR, 0x5000, 0x5FFF, NULL, 0 ) );
    TEST_ASSERT_EQUAL_size_t ( 0, profiler_pc_get_rows ( PROFILER_PC_SORT_ADDR, 0x5001, 0x5000, rows, 3 ) );
}


/** @brief Export CSV: hlavička, hex adresa, desetinná tečka. */
void test_profiler_pc_export_csv ( void )
{
    step ( 0x0123, 3, 0, 0, false );
    step ( 0xABCD, 1, 1, 0, false );

    char *dir = g_dir_make_tmp ( "pcstats_XXXXXX", NULL );
    TEST_ASSERT_NOT_NULL ( dir );
    char *path = g_build_filename ( dir, "pc.csv", NULL );
    char err[ 128 ];
    TEST_ASSERT_EQUAL_INT ( 2, profiler_pc_export_csv ( path, PROFILER_PC_SORT_ADDR, err, sizeof ( err ) ) );

    char *text = NULL;
    TEST_ASSERT_TRUE ( g_file_get_contents ( path, &text, NULL, NULL ) );
    TEST_ASSERT_EQUAL_STRING ( "pc,count,tstates,wait,tstates_pct\n"
                               "0123,1,3,0,75.0000\n"
                               "ABCD,1,1,1,25.0000\n", text );
    g_free ( text );

    char *bad = g_build_filename ( dir, "missing", "pc.csv", NULL );
    TEST_ASSERT_EQUAL_INT ( -1, profiler_pc_export_csv ( bad, PROFILER_PC_SORT_ADDR, err, sizeof ( err ) ) );
    TEST_ASSERT_TRUE ( err[ 0 ] != '\0' );

    g_remove ( path );
    g_rmdir ( dir );
    g_free ( bad );
    g_free ( path );
    g_free ( dir );
}


/**
 * @brief Bitmapa vykonaných adres: pořadí bitů (LSB = adresa & 7), krajní
 *        adresy, počet = unique_pcs; nečinný HALT ani opakování nepřidá bit.
 */
void test_profiler_pc_bitmap ( void )
{
    step ( 0x0000, 4, 0, 0, false );
    step ( 0x0007, 4, 0, 0, false );
    step ( 0x1200, 21, 0, 0, false );    /* LDIR: dvakrát stejný začátek */
    step ( 0x1200, 16, 0, 0, false );
    step ( 0x1202, 4, 0, 0, true );      /* HALT */
    step ( 0x1202, 4, 0, 0, true );      /* nečinný HALT */
    step ( 0xFFFF, 4, 0, 0, false );

    uint8_t bm[ PROFILER_PC_BITMAP_BYTES ];
    memset ( bm, 0xAA, sizeof ( bm ) );  /* funkce musí přepsat celý buffer */
    TEST_ASSERT_EQUAL_UINT ( 5, profiler_pc_get_bitmap ( bm ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x81, bm[ 0x0000 ] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x05, bm[ 0x1200 >> 3 ] );
    TEST_ASSERT_EQUAL_HEX8 ( 0x80, bm[ 0x1FFF ] );
    unsigned bits = 0;
    for ( size_t i = 0; i < sizeof ( bm ); i++ ) {
        for ( uint8_t v = bm[ i ]; v; v &= (uint8_t) ( v - 1 ) ) bits++;
    };
    TEST_ASSERT_EQUAL_UINT ( 5, bits );

    st_PROFILER_PC_SUMMARY s;
    profiler_pc_get_summary ( &s );
    TEST_ASSERT_EQUAL_UINT ( 5, s.unique_pcs );

    profiler_pc_reset ( );
    TEST_ASSERT_EQUAL_UINT ( 0, profiler_pc_get_bitmap ( bm ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x00, bm[ 0x0000 ] );
}


int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    RUN_TEST ( test_profiler_pc_aggregation );
    RUN_TEST ( test_profiler_pc_halt_and_irq );
    RUN_TEST ( test_profiler_pc_discontinuity );
    RUN_TEST ( test_profiler_pc_start_stop );
    RUN_TEST ( test_profiler_pc_start_inside_halt );
    RUN_TEST ( test_profiler_pc_reset_active_inside_halt );
    RUN_TEST ( test_profiler_pc_rows_sort_range );
    RUN_TEST ( test_profiler_pc_export_csv );
    RUN_TEST ( test_profiler_pc_bitmap );

    return UNITY_END ( );
}
