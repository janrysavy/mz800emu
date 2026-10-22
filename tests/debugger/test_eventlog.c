/*
 * test_eventlog.c - unit testy pro Event Viewer in-memory ring (Vlna 1 Commit 1).
 *
 * Testuje:
 *  - 26 B layout (Vlna 4 Commit 24 - ambient state pole)
 *  - init / destroy (alokace, cleanup)
 *  - basic record + get_event
 *  - ring wrap-around (capacity 10000, write 12000 -> count = 10000, overflow)
 *  - clear nuluje stav (zachovává capacity)
 *  - set_capacity resize (capacity_after >= MIN, count = 0)
 *  - capacity clamping do [MIN, MAX]
 *  - start/stop přepíná g_eventlog_active flag
 *  - mask gate: caller-test pattern (eventlog_record je robustní, ale gate u caller)
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <stdio.h>
#include <string.h>

#include "debugger/trace/eventlog.h"
#include "debugger/trace/hwlog.h"
#include "debugger/trace/intlog.h"
#include "debugger/trace/marklog.h"
#include "debugger/bp_action.h"
#include "debugger/io_history.h"
#include "emulator/mzarch/mzarch.h"
#include "libs/cpu-z80/z80.h"


void setUp ( void )
{
    /* Pre-test čistý stav. eventlog_init alokuje ring s defaultní capacity
     * (cfg propagate v init nastaví mode = OFF, mask = vše). */
    eventlog_init ( EVENTLOG_DEFAULT_CAPACITY );
    g_eventlog_active = 0;
}


void tearDown ( void )
{
    eventlog_destroy ( );
}


/* ================================================================
 * Layout
 * ================================================================ */

void test_eventlog_record_size_is_24_bytes ( void )
{
    /* Vlna 4 Commit 24 - layout rozšířen na 32 B (= ambient 2B + trailing
     * padding 6B kvůli 8B alignment).
     * Static_assert v headeru zajistí compile-time check, ale potvrdíme
     * i runtime kvůli MSVC kompilaci (bez C11 _Static_assert). Název
     * testu zachováván z BC důvodů (= test runner reference). */
    TEST_ASSERT_EQUAL_size_t ( 32, sizeof ( st_EVENTLOG_EVENT ) );
}


void test_eventlog_category_count_fits_in_64bit_mask ( void )
{
    TEST_ASSERT_TRUE ( (int) EVENTLOG_CAT_COUNT <= 64 );
}


/* ================================================================
 * Init / destroy
 * ================================================================ */

void test_eventlog_init_default_capacity ( void )
{
    TEST_ASSERT_NOT_NULL ( g_eventlog.events );
    TEST_ASSERT_EQUAL_size_t ( EVENTLOG_DEFAULT_CAPACITY, g_eventlog.capacity );
    TEST_ASSERT_EQUAL_size_t ( 0, g_eventlog.count );
    TEST_ASSERT_EQUAL_size_t ( 0, g_eventlog.head );
    TEST_ASSERT_FALSE ( g_eventlog.overflow );
}


void test_eventlog_init_clamps_below_min ( void )
{
    eventlog_destroy ( );
    eventlog_init ( 100 );  /* < MIN */
    TEST_ASSERT_EQUAL_size_t ( EVENTLOG_MIN_CAPACITY, g_eventlog.capacity );
}


void test_eventlog_init_clamps_above_max ( void )
{
    eventlog_destroy ( );
    eventlog_init ( 10000000 );  /* > MAX */
    TEST_ASSERT_EQUAL_size_t ( EVENTLOG_MAX_CAPACITY, g_eventlog.capacity );
}


void test_eventlog_destroy_cleans_up ( void )
{
    eventlog_destroy ( );
    TEST_ASSERT_NULL ( g_eventlog.events );
    TEST_ASSERT_EQUAL_size_t ( 0, g_eventlog.capacity );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_active );
    /* Po destroy je init znovu OK (idempotence + re-alloc). */
    eventlog_init ( EVENTLOG_DEFAULT_CAPACITY );
    TEST_ASSERT_NOT_NULL ( g_eventlog.events );
}


/* ================================================================
 * Basic record + read
 * ================================================================ */

void test_eventlog_basic_write_read ( void )
{
    eventlog_record ( EVENTLOG_CAT_IORQ_OUT, 0x12, 0x4042, 0x000000CEu );

    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.count );
    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.head );
    TEST_ASSERT_FALSE ( g_eventlog.overflow );

    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_IORQ_OUT, e->category );
    TEST_ASSERT_EQUAL_UINT8 ( 0x12, e->subtype );
    TEST_ASSERT_EQUAL_HEX16 ( 0x4042, e->pc );
    TEST_ASSERT_EQUAL_HEX32 ( 0x000000CEu, e->payload );
}


void test_eventlog_get_event_out_of_range ( void )
{
    eventlog_record ( EVENTLOG_CAT_CPU_INT, 0, 0x4000, 0 );
    TEST_ASSERT_NULL ( eventlog_get_event ( 1 ) );
    TEST_ASSERT_NULL ( eventlog_get_event ( 100 ) );
}


void test_eventlog_get_count_matches_writes ( void )
{
    for ( unsigned i = 0; i < 50; i++ ) {
        eventlog_record ( EVENTLOG_CAT_CPU_INT, (uint8_t) i, (uint16_t) i, i );
    }
    TEST_ASSERT_EQUAL_size_t ( 50, eventlog_get_count ( ) );
}


/* ================================================================
 * Ring wrap-around / overflow
 * ================================================================ */

void test_eventlog_ring_wrap ( void )
{
    /* Smaller capacity pro rychlost testu. set_capacity clampuje na MIN. */
    eventlog_set_capacity ( EVENTLOG_MIN_CAPACITY );
    TEST_ASSERT_EQUAL_size_t ( EVENTLOG_MIN_CAPACITY, g_eventlog.capacity );

    /* Write o 20% více než capacity -> wrap o 20%. */
    size_t total = EVENTLOG_MIN_CAPACITY + ( EVENTLOG_MIN_CAPACITY / 5 );
    for ( size_t i = 0; i < total; i++ ) {
        /* PC = i (low 16 bitů), payload = i jako fingerprint. */
        eventlog_record ( EVENTLOG_CAT_IORQ_IN, 0, (uint16_t) ( i & 0xFFFFu ),
                          (uint32_t) i );
    }

    TEST_ASSERT_EQUAL_size_t ( EVENTLOG_MIN_CAPACITY, g_eventlog.count );
    TEST_ASSERT_TRUE ( g_eventlog.overflow );

    /* Oldest = i = (total - capacity), newest = i = (total - 1). */
    const st_EVENTLOG_EVENT *oldest = eventlog_get_event ( 0 );
    const st_EVENTLOG_EVENT *newest = eventlog_get_event ( EVENTLOG_MIN_CAPACITY - 1 );
    TEST_ASSERT_NOT_NULL ( oldest );
    TEST_ASSERT_NOT_NULL ( newest );
    TEST_ASSERT_EQUAL_UINT32 ( (uint32_t) ( total - EVENTLOG_MIN_CAPACITY ),
                               oldest->payload );
    TEST_ASSERT_EQUAL_UINT32 ( (uint32_t) ( total - 1 ), newest->payload );
}


/* seq proudu (trace_read): next_seq roste i přes wrap, nejstarší event má
 * seq next_seq - count; clear a změna kapacity = nový segment, seq od 0. */
void test_eventlog_seq_and_segment ( void )
{
    eventlog_set_capacity ( EVENTLOG_MIN_CAPACITY );
    unsigned seg = g_eventlog.segment;
    TEST_ASSERT_TRUE ( seg > 0 );
    TEST_ASSERT_EQUAL_UINT64 ( 0, g_eventlog.next_seq );
    size_t total = EVENTLOG_MIN_CAPACITY + 7;
    for ( size_t i = 0; i < total; i++ ) {
        eventlog_record ( EVENTLOG_CAT_IORQ_IN, 0, 0, (uint32_t) i );
    }
    TEST_ASSERT_EQUAL_UINT64 ( total, g_eventlog.next_seq );
    TEST_ASSERT_EQUAL_UINT64 ( 7, g_eventlog.next_seq - g_eventlog.count );
    TEST_ASSERT_EQUAL_UINT ( seg, g_eventlog.segment );
    eventlog_clear ( );
    TEST_ASSERT_EQUAL_UINT64 ( 0, g_eventlog.next_seq );
    TEST_ASSERT_EQUAL_UINT ( seg + 1, g_eventlog.segment );
    eventlog_set_capacity ( EVENTLOG_MIN_CAPACITY * 2 );
    TEST_ASSERT_EQUAL_UINT ( seg + 2, g_eventlog.segment );
}


/* ================================================================
 * Clear
 * ================================================================ */

void test_eventlog_clear ( void )
{
    eventlog_record ( EVENTLOG_CAT_CPU_INT,    0, 0x4000, 0 );
    eventlog_record ( EVENTLOG_CAT_GDG_MODE,   1, 0x4002, 1 );
    eventlog_record ( EVENTLOG_CAT_USER_MARK,  2, 0x4004, 2 );

    size_t saved_capacity = g_eventlog.capacity;
    eventlog_clear ( );

    TEST_ASSERT_EQUAL_size_t ( 0, g_eventlog.count );
    TEST_ASSERT_EQUAL_size_t ( 0, g_eventlog.head );
    TEST_ASSERT_FALSE ( g_eventlog.overflow );
    /* Capacity zachována (clear nemění alokaci). */
    TEST_ASSERT_EQUAL_size_t ( saved_capacity, g_eventlog.capacity );
}


/* ================================================================
 * Set capacity
 * ================================================================ */

void test_eventlog_set_capacity_resizes_and_clears ( void )
{
    eventlog_record ( EVENTLOG_CAT_CPU_INT, 0, 0x4000, 0xAAu );
    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.count );

    eventlog_set_capacity ( 25000 );

    TEST_ASSERT_EQUAL_size_t ( 25000, g_eventlog.capacity );
    TEST_ASSERT_EQUAL_size_t ( 0, g_eventlog.count );
    TEST_ASSERT_FALSE ( g_eventlog.overflow );
    TEST_ASSERT_NOT_NULL ( g_eventlog.events );
}


void test_eventlog_set_capacity_clamps ( void )
{
    eventlog_set_capacity ( 100 );  /* < MIN */
    TEST_ASSERT_EQUAL_size_t ( EVENTLOG_MIN_CAPACITY, g_eventlog.capacity );

    eventlog_set_capacity ( 10000000 );  /* > MAX */
    TEST_ASSERT_EQUAL_size_t ( EVENTLOG_MAX_CAPACITY, g_eventlog.capacity );
}


/* ================================================================
 * Start / stop
 * ================================================================ */

void test_eventlog_start_stop ( void )
{
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_active );

    int rc = eventlog_start ( );
    TEST_ASSERT_EQUAL_INT ( 0, rc );
    TEST_ASSERT_EQUAL_INT ( 1, g_eventlog_active );
    TEST_ASSERT_TRUE ( TEST_TRACE_EVENTLOG_ACTIVE );

    /* Idempotence start. */
    rc = eventlog_start ( );
    TEST_ASSERT_EQUAL_INT ( 0, rc );
    TEST_ASSERT_EQUAL_INT ( 1, g_eventlog_active );

    eventlog_stop ( );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_active );
    TEST_ASSERT_FALSE ( TEST_TRACE_EVENTLOG_ACTIVE );

    /* Idempotence stop. */
    eventlog_stop ( );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_active );
}


void test_eventlog_start_fails_without_ring ( void )
{
    /* Destroy zruší alokaci - start MUSÍ vrátit -1. */
    eventlog_destroy ( );
    int rc = eventlog_start ( );
    TEST_ASSERT_EQUAL_INT ( -1, rc );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_active );

    /* Restore pro tearDown - re-init. */
    eventlog_init ( EVENTLOG_DEFAULT_CAPACITY );
}


/* ================================================================
 * Mask gate (caller-side gate test)
 * ================================================================ */

void test_eventlog_mask_filter_caller_gate ( void )
{
    /* eventlog_record samo o sobě negate na mask - gate je v caller.
     * Tento test demonstruje doporučený pattern: caller testuje
     * (active && (mask & bit)) a teprve potom volá eventlog_record. */
    g_eventlog_active = 1;
    g_eventlog_active_mask = 0;  /* Žádná kategorie není povolená. */

    /* Simulate caller gate pro CPU_INT (kategorie 0). */
    if ( TEST_TRACE_EVENTLOG_ACTIVE
         && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_CPU_INT ) ) ) {
        eventlog_record ( EVENTLOG_CAT_CPU_INT, 0, 0x4000, 0 );
    }
    TEST_ASSERT_EQUAL_size_t ( 0, eventlog_get_count ( ) );

    /* Povolit CPU_INT a opakovat - tentokrát se zapíše. */
    g_eventlog_active_mask = ( 1ULL << EVENTLOG_CAT_CPU_INT );
    if ( TEST_TRACE_EVENTLOG_ACTIVE
         && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_CPU_INT ) ) ) {
        eventlog_record ( EVENTLOG_CAT_CPU_INT, 0, 0x4000, 0 );
    }
    TEST_ASSERT_EQUAL_size_t ( 1, eventlog_get_count ( ) );

    /* IORQ_OUT (kategorie 4) NENÍ v masce -> nezapíše se. */
    if ( TEST_TRACE_EVENTLOG_ACTIVE
         && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_IORQ_OUT ) ) ) {
        eventlog_record ( EVENTLOG_CAT_IORQ_OUT, 0, 0x4002, 0 );
    }
    TEST_ASSERT_EQUAL_size_t ( 1, eventlog_get_count ( ) );
}


/* ================================================================
 * Recompute active
 * ================================================================ */

void test_eventlog_recompute_active_modes ( void )
{
    /* OFF -> nikdy aktivní. */
    g_eventlog_config.mode = EVENTLOG_MODE_OFF;
    eventlog_recompute_active ( 1 );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_active );

    /* ALWAYS -> vždy aktivní. */
    g_eventlog_config.mode = EVENTLOG_MODE_ALWAYS;
    eventlog_recompute_active ( 0 );
    TEST_ASSERT_EQUAL_INT ( 1, g_eventlog_active );

    /* WHEN_WINDOW_OPEN -> v Commit 1 vždy 0 (interní static = 0). */
    g_eventlog_config.mode = EVENTLOG_MODE_WHEN_WINDOW_OPEN;
    eventlog_recompute_active ( 1 );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_active );

    /* Návrat na OFF pro tearDown. */
    g_eventlog_config.mode = EVENTLOG_MODE_OFF;
    eventlog_recompute_active ( 0 );
    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_active );
}


/* ================================================================
 * Fan-out hooky (Commit 2): hwlog / intlog / marklog / io_history
 * volají paralelně eventlog_record s respektováním active gate +
 * per-kategorie mask. Tlog disk write side zůstává mimo scope těchto
 * testů (jeho writery nejsou v unit testu otevřené - testujeme jen,
 * že fan-out cesta nezávisle skončí v ringu).
 * ================================================================ */

/**
 * @brief Helper: nastavit eventlog do plně aktivního stavu pro fan-out test.
 *
 * Aktivuje recording a povolí všechny kategorie. Volat na začátku každého
 * fan-out testu. Test_eventlog setUp uz volal eventlog_init.
 */
static void fanout_helper_enable_eventlog ( void )
{
    g_eventlog_active_mask = UINT64_C ( 0xFFFFFFFFFFFFFFFF );
    eventlog_clear ( );
    eventlog_start ( );
}


void test_eventlog_fanout_hwlog_gdg_mode ( void )
{
    fanout_helper_enable_eventlog ( );

    /* hwlog HWLOG_CHIP_GDG_MODE -> EVENTLOG_CAT_GDG_MODE. Payload 1 B = DMD
     * value, ostatní pole nuly. Caller-test gate v test prostředí nahradíme
     * přímým voláním hwlog_record_byte (= chip driver hot-path), které samo
     * uvnitř udělá fan-out. */
    hwlog_record_byte ( HWLOG_CHIP_GDG_MODE, 0x01 /* sub */, 0x08u );

    TEST_ASSERT_EQUAL_size_t ( 1, eventlog_get_count ( ) );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_GDG_MODE, e->category );
    TEST_ASSERT_EQUAL_UINT8 ( 0x01, e->subtype );
    /* Payload pack: B0 = value (0x08), B1..B3 = 0. */
    TEST_ASSERT_EQUAL_HEX32 ( 0x00000008u, e->payload );
}


void test_eventlog_fanout_intlog_pin_edge ( void )
{
    fanout_helper_enable_eventlog ( );

    intlog_record_pin_edge ( INTLOG_CHIP_CTC2, 0u, INTLOG_EDGE_RISING );

    TEST_ASSERT_EQUAL_size_t ( 1, eventlog_get_count ( ) );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_CPU_PIN_EDGE, e->category );
    /* Subtype = source_chip (INTLOG_CHIP_CTC2 = 1). */
    TEST_ASSERT_EQUAL_UINT8 ( INTLOG_CHIP_CTC2, e->subtype );
    /* Payload: pin (low B) | edge (B+1). */
    uint32_t expected = (uint32_t) 0u | ( (uint32_t) INTLOG_EDGE_RISING << 8 );
    TEST_ASSERT_EQUAL_HEX32 ( expected, e->payload );
}


void test_eventlog_fanout_marklog ( void )
{
    fanout_helper_enable_eventlog ( );

    /* marklog hot-path je gated pouze marker_id != INVALID. Test pro
     * fan-out nepotřebuje registr - volá přímo s id, který je jakákoliv
     * hodnota mimo MARKLOG_INVALID_ID. */
    marklog_record ( 0x42u );

    TEST_ASSERT_EQUAL_size_t ( 1, eventlog_get_count ( ) );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_USER_MARK, e->category );
    TEST_ASSERT_EQUAL_UINT8 ( 0, e->subtype );
    TEST_ASSERT_EQUAL_HEX32 ( 0x00000042u, e->payload );
}


void test_eventlog_fanout_mask_disabled ( void )
{
    eventlog_clear ( );
    eventlog_start ( );
    /* Active = 1, ale mask vynulovaná -> fan-out se NEsmí zapsat. */
    g_eventlog_active_mask = 0;

    hwlog_record_byte ( HWLOG_CHIP_GDG_MODE, 0x01, 0x08u );
    intlog_record_pin_edge ( INTLOG_CHIP_CTC2, 0, INTLOG_EDGE_RISING );
    marklog_record ( 0x10u );

    TEST_ASSERT_EQUAL_size_t ( 0, eventlog_get_count ( ) );
}


void test_eventlog_fanout_inactive ( void )
{
    eventlog_clear ( );
    /* Active = 0 (= eventlog_start nezavolán), mask "vše" - fan-out se
     * NEsmí zapsat. */
    g_eventlog_active = 0;
    g_eventlog_active_mask = UINT64_C ( 0xFFFFFFFFFFFFFFFF );

    hwlog_record_byte ( HWLOG_CHIP_PSG, 0x01, 0x55u );
    intlog_record_cpu_int_state ( INTLOG_STATE_BIT_IFF1 );
    marklog_record ( 0x10u );

    TEST_ASSERT_EQUAL_size_t ( 0, eventlog_get_count ( ) );
}


void test_eventlog_fanout_hwlog_unmapped_chip ( void )
{
    fanout_helper_enable_eventlog ( );

    /* Hodnota mimo en_HWLOG_CHIP rozsah (= žádné mapování na eventlog
     * kategorii). hwlog_chip_to_eventlog_cat vrátí EVENTLOG_CAT_COUNT
     * -> fan-out skipne. */
    hwlog_record_byte ( (en_HWLOG_CHIP) 0x7Fu, 0x01, 0xAAu );

    TEST_ASSERT_EQUAL_size_t ( 0, eventlog_get_count ( ) );
}


void test_eventlog_fanout_multi_categories_independent ( void )
{
    fanout_helper_enable_eventlog ( );

    /* Tři různé fan-out cesty, každá z jiného subsystému + jiné kategorie.
     * Cílem je ověřit, že eventlog ring dostává oddělené eventy v pořadí
     * volání. */
    hwlog_record_byte ( HWLOG_CHIP_PSG, 0x01, 0x33u );
    intlog_record_cpu_int_state ( INTLOG_STATE_BIT_RETI );
    marklog_record ( 0x07u );

    TEST_ASSERT_EQUAL_size_t ( 3, eventlog_get_count ( ) );

    const st_EVENTLOG_EVENT *e0 = eventlog_get_event ( 0 );
    const st_EVENTLOG_EVENT *e1 = eventlog_get_event ( 1 );
    const st_EVENTLOG_EVENT *e2 = eventlog_get_event ( 2 );
    TEST_ASSERT_NOT_NULL ( e0 );
    TEST_ASSERT_NOT_NULL ( e1 );
    TEST_ASSERT_NOT_NULL ( e2 );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_PSG, e0->category );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_CPU_INT, e1->category );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_USER_MARK, e2->category );
    TEST_ASSERT_EQUAL_HEX32 ( 0x00000033u, e0->payload );
    TEST_ASSERT_EQUAL_HEX32 ( INTLOG_STATE_BIT_RETI, e1->payload );
    TEST_ASSERT_EQUAL_HEX32 ( 0x00000007u, e2->payload );
}


void test_eventlog_fanout_new_categories_count ( void )
{
    /* Sanity: nové kategorie GDG_WFRF / QD / RD jsou pridány, EVENTLOG_CAT_COUNT
     * = 25 (= 21 původních + 3 Vlny 2 + 1 Vlna 5 Commit 31 SYS). */
    TEST_ASSERT_EQUAL_INT ( 25, (int) EVENTLOG_CAT_COUNT );
    /* Compile-time pojistka, že enum se vejde do uint64 masky. */
    TEST_ASSERT_TRUE ( (int) EVENTLOG_CAT_COUNT <= 64 );
}


/* ================================================================
 * Commit 2.1: IORQ subtype rozlišení NORMAL vs UNCONNECTED
 * ================================================================
 *
 * Per-arch port_*_with_logging_cb vyplňuje subtype eventu IORQ_IN /
 * IORQ_OUT podle ambient flagu g_tracelog_iorq_unconnected. Tady
 * ověřujeme jen, že enum hodnoty existují a že zápis přes
 * eventlog_record() je přenese korektně do ringu (= field-level test,
 * nezávislý na per-arch CPU path - tu pokrývají integration testy).
 */

void test_eventlog_iorq_subtype_enum_values ( void )
{
    /* Hodnoty NESMÍ změnit svůj číselný význam - persistence v cfg / UI. */
    TEST_ASSERT_EQUAL_UINT8 ( 0, (uint8_t) EVENTLOG_IORQ_SUB_NORMAL );
    TEST_ASSERT_EQUAL_UINT8 ( 1, (uint8_t) EVENTLOG_IORQ_SUB_UNCONNECTED );
}


void test_eventlog_iorq_subtype_normal ( void )
{
    /* Standardní IORQ_IN s portem 0xCE (GDG status) = NORMAL. */
    g_eventlog_active = 1;
    eventlog_record ( EVENTLOG_CAT_IORQ_IN,
                      (uint8_t) EVENTLOG_IORQ_SUB_NORMAL,
                      0x4042u,
                      0x000000CEu );

    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.count );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_IORQ_IN, e->category );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_IORQ_SUB_NORMAL, e->subtype );
    TEST_ASSERT_EQUAL_HEX16 ( 0x4042u, e->pc );
    TEST_ASSERT_EQUAL_HEX32 ( 0x000000CEu, e->payload );
}


void test_eventlog_iorq_subtype_unconnected ( void )
{
    /* Ghost IORQ_IN na nemapovaný port (např. 0xAA) = UNCONNECTED.
     * Stejná category, jen jiný subtype - UI filter pak rozlišuje. */
    g_eventlog_active = 1;
    eventlog_record ( EVENTLOG_CAT_IORQ_IN,
                      (uint8_t) EVENTLOG_IORQ_SUB_UNCONNECTED,
                      0x4100u,
                      0x000000AAu );
    eventlog_record ( EVENTLOG_CAT_IORQ_OUT,
                      (uint8_t) EVENTLOG_IORQ_SUB_UNCONNECTED,
                      0x4200u,
                      0x000000ABu );

    TEST_ASSERT_EQUAL_size_t ( 2, g_eventlog.count );

    const st_EVENTLOG_EVENT *in_evt = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( in_evt );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_IORQ_IN, in_evt->category );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_IORQ_SUB_UNCONNECTED, in_evt->subtype );

    const st_EVENTLOG_EVENT *out_evt = eventlog_get_event ( 1 );
    TEST_ASSERT_NOT_NULL ( out_evt );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_IORQ_OUT, out_evt->category );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_IORQ_SUB_UNCONNECTED, out_evt->subtype );
}


/* ================================================================
 * Commit 3 - BP_FIRE kategorie
 * ================================================================ */

void test_eventlog_bp_fire_subtype_enum_values ( void )
{
    /* Hodnoty NESMÍ změnit svůj číselný význam - persistence v cfg / UI +
     * sjednocení s en_BP_ACTION_SUB v bp_action.h (1:1 mapping). */
    TEST_ASSERT_EQUAL_UINT8 ( 0, (uint8_t) EVENTLOG_BP_FIRE_SUB_HALT );
    TEST_ASSERT_EQUAL_UINT8 ( 1, (uint8_t) EVENTLOG_BP_FIRE_SUB_MARK );
    TEST_ASSERT_EQUAL_UINT8 ( 2, (uint8_t) EVENTLOG_BP_FIRE_SUB_CONTINUE );
    TEST_ASSERT_EQUAL_UINT8 ( 3, (uint8_t) EVENTLOG_BP_FIRE_SUB_IGNORE );
    TEST_ASSERT_EQUAL_UINT8 ( 4, (uint8_t) EVENTLOG_BP_FIRE_SUB_ENABLE );
    TEST_ASSERT_EQUAL_UINT8 ( 5, (uint8_t) EVENTLOG_BP_FIRE_SUB_DISABLE );

    /* Stejné hodnoty MUSÍ mít i veřejný classifier enum v bp_action.h. */
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_BP_FIRE_SUB_HALT,
                              (uint8_t) BP_ACTION_SUB_HALT );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_BP_FIRE_SUB_MARK,
                              (uint8_t) BP_ACTION_SUB_MARK );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_BP_FIRE_SUB_CONTINUE,
                              (uint8_t) BP_ACTION_SUB_CONTINUE );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_BP_FIRE_SUB_IGNORE,
                              (uint8_t) BP_ACTION_SUB_IGNORE );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_BP_FIRE_SUB_ENABLE,
                              (uint8_t) BP_ACTION_SUB_ENABLE );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_BP_FIRE_SUB_DISABLE,
                              (uint8_t) BP_ACTION_SUB_DISABLE );
}


void test_eventlog_bp_fire_emit ( void )
{
    /* Direct emit (= simulace fan-outu z breakpoints_enforce).
     * Payload schéma: low 16 = bp_id, next 8 = reason. */
    g_eventlog_active = 1;

    uint16_t bp_id   = 0x0005u;
    uint8_t  reason  = 0x03u;   /* BP_REASON_IFF_INT_ACK */
    uint16_t pc      = 0x1234u;
    uint32_t payload = (uint32_t) bp_id | ( (uint32_t) reason << 16 );

    eventlog_record ( EVENTLOG_CAT_BP_FIRE,
                      (uint8_t) EVENTLOG_BP_FIRE_SUB_HALT,
                      pc,
                      payload );

    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.count );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );
    TEST_ASSERT_EQUAL_UINT8  ( EVENTLOG_CAT_BP_FIRE,       e->category );
    TEST_ASSERT_EQUAL_UINT8  ( EVENTLOG_BP_FIRE_SUB_HALT,  e->subtype );
    TEST_ASSERT_EQUAL_HEX16  ( pc,                          e->pc );
    TEST_ASSERT_EQUAL_HEX32  ( payload,                     e->payload );

    /* Ověření dekódování: low 16 b = bp_id, bity 16..23 = reason. */
    TEST_ASSERT_EQUAL_HEX16 ( bp_id,  (uint16_t) ( e->payload & 0xFFFFu ) );
    TEST_ASSERT_EQUAL_HEX8  ( reason, (uint8_t)  ( ( e->payload >> 16 ) & 0xFFu ) );
}


void test_eventlog_bp_fire_classify_subtype ( void )
{
    /* Classifier je čistá funkce nad parsovaným AST.
     * NULL action = HALT (= "stop" sémantika). */
    TEST_ASSERT_EQUAL_INT ( BP_ACTION_SUB_HALT,
                            bp_action_classify_subtype ( NULL ) );

    /* "continue" sám o sobě = CONTINUE (= explicit no-op v DSL). */
    {
        char err[128] = "";
        bp_action_t *a = bp_action_parse ( "continue", err, sizeof ( err ) );
        TEST_ASSERT_NOT_NULL ( a );
        TEST_ASSERT_EQUAL_INT ( BP_ACTION_SUB_CONTINUE,
                                bp_action_classify_subtype ( a ) );
        bp_action_free ( a );
    }

    /* log "x" = CONTINUE (= side-effect bez explicit kategorie). */
    {
        char err[128] = "";
        bp_action_t *a = bp_action_parse ( "log \"hit\"", err, sizeof ( err ) );
        TEST_ASSERT_NOT_NULL ( a );
        TEST_ASSERT_EQUAL_INT ( BP_ACTION_SUB_CONTINUE,
                                bp_action_classify_subtype ( a ) );
        bp_action_free ( a );
    }

    /* mark "name" = MARK (= dominantní statement). */
    {
        char err[128] = "";
        bp_action_t *a = bp_action_parse ( "mark \"frame\"",
                                            err, sizeof ( err ) );
        TEST_ASSERT_NOT_NULL ( a );
        TEST_ASSERT_EQUAL_INT ( BP_ACTION_SUB_MARK,
                                bp_action_classify_subtype ( a ) );
        bp_action_free ( a );
    }

    /* disable_self = DISABLE. */
    {
        char err[128] = "";
        bp_action_t *a = bp_action_parse ( "disable_self",
                                            err, sizeof ( err ) );
        TEST_ASSERT_NOT_NULL ( a );
        TEST_ASSERT_EQUAL_INT ( BP_ACTION_SUB_DISABLE,
                                bp_action_classify_subtype ( a ) );
        bp_action_free ( a );
    }

    /* enable other_bp = ENABLE. */
    {
        char err[128] = "";
        bp_action_t *a = bp_action_parse ( "enable other_bp",
                                            err, sizeof ( err ) );
        TEST_ASSERT_NOT_NULL ( a );
        TEST_ASSERT_EQUAL_INT ( BP_ACTION_SUB_ENABLE,
                                bp_action_classify_subtype ( a ) );
        bp_action_free ( a );
    }

    /* Kombinace: "log + mark" - MARK má vyšší prioritu (dominantní). */
    {
        char err[128] = "";
        bp_action_t *a = bp_action_parse ( "log \"x\"\nmark \"frame\"",
                                            err, sizeof ( err ) );
        TEST_ASSERT_NOT_NULL ( a );
        TEST_ASSERT_EQUAL_INT ( BP_ACTION_SUB_MARK,
                                bp_action_classify_subtype ( a ) );
        bp_action_free ( a );
    }

    /* Vnořený if - klasifikace musí rekursivně sestoupit do then větve. */
    {
        char err[128] = "";
        bp_action_t *a = bp_action_parse ( "if Hits == 1 then mark \"once\"",
                                            err, sizeof ( err ) );
        TEST_ASSERT_NOT_NULL ( a );
        TEST_ASSERT_EQUAL_INT ( BP_ACTION_SUB_MARK,
                                bp_action_classify_subtype ( a ) );
        bp_action_free ( a );
    }
}


/* ================================================================
 * Commit 5 - CPU_CTRL kategorie (HALT enter/exit, RST 00..38)
 * ================================================================ */

void test_eventlog_cpu_ctrl_subtype_enum_values ( void )
{
    /* Hodnoty NESMÍ změnit svůj číselný význam - persistence v cfg / UI +
     * sjednocení s z80_cpu_ctrl_event_t v libs/cpu-z80/z80.h (1:1 mapping,
     * konzument v mzarch_cpu_ctrl_event_cb dělá jen cast). */
    TEST_ASSERT_EQUAL_UINT8 ( 0, (uint8_t) EVENTLOG_CPU_CTRL_SUB_HALT_ENTER );
    TEST_ASSERT_EQUAL_UINT8 ( 1, (uint8_t) EVENTLOG_CPU_CTRL_SUB_HALT_EXIT );
    TEST_ASSERT_EQUAL_UINT8 ( 2, (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_00 );
    TEST_ASSERT_EQUAL_UINT8 ( 3, (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_08 );
    TEST_ASSERT_EQUAL_UINT8 ( 4, (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_10 );
    TEST_ASSERT_EQUAL_UINT8 ( 5, (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_18 );
    TEST_ASSERT_EQUAL_UINT8 ( 6, (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_20 );
    TEST_ASSERT_EQUAL_UINT8 ( 7, (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_28 );
    TEST_ASSERT_EQUAL_UINT8 ( 8, (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_30 );
    TEST_ASSERT_EQUAL_UINT8 ( 9, (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_38 );

    /* Stejné hodnoty MUSÍ mít i z80 lib enum. Jakýkoliv drift = compile-time
     * (přidaná hodnota v z80 lib) NEBO test failure (změna pořadí). */
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_CPU_CTRL_SUB_HALT_ENTER,
                              (uint8_t) Z80_CPU_CTRL_HALT_ENTER );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_CPU_CTRL_SUB_HALT_EXIT,
                              (uint8_t) Z80_CPU_CTRL_HALT_EXIT );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_00,
                              (uint8_t) Z80_CPU_CTRL_RST_00 );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_08,
                              (uint8_t) Z80_CPU_CTRL_RST_08 );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_10,
                              (uint8_t) Z80_CPU_CTRL_RST_10 );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_18,
                              (uint8_t) Z80_CPU_CTRL_RST_18 );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_20,
                              (uint8_t) Z80_CPU_CTRL_RST_20 );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_28,
                              (uint8_t) Z80_CPU_CTRL_RST_28 );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_30,
                              (uint8_t) Z80_CPU_CTRL_RST_30 );
    TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_38,
                              (uint8_t) Z80_CPU_CTRL_RST_38 );
}


void test_eventlog_cpu_ctrl_emit_halt_enter ( void )
{
    /* Direct emit (= simulace fan-outu z mzarch_cpu_ctrl_event_cb).
     * HALT_ENTER při dispatch opcode 0x76 - PC ukazuje za HALT instrukci. */
    g_eventlog_active = 1;

    uint16_t pc = 0x4042u;
    eventlog_record ( (uint8_t) EVENTLOG_CAT_CPU_CTRL,
                      (uint8_t) EVENTLOG_CPU_CTRL_SUB_HALT_ENTER,
                      pc,
                      0u );

    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.count );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );
    TEST_ASSERT_EQUAL_UINT8  ( EVENTLOG_CAT_CPU_CTRL,           e->category );
    TEST_ASSERT_EQUAL_UINT8  ( EVENTLOG_CPU_CTRL_SUB_HALT_ENTER, e->subtype );
    TEST_ASSERT_EQUAL_HEX16  ( pc,                                e->pc );
    TEST_ASSERT_EQUAL_HEX32  ( 0u,                                e->payload );
}


void test_eventlog_cpu_ctrl_emit_rst_38 ( void )
{
    /* Direct emit RST 38h (= opcode 0xFF). PC za RST opcodem (= cílová
     * adresa návratu po PUSH PC). */
    g_eventlog_active = 1;

    uint16_t pc = 0x0066u;  /* libovolná, simuluje return PC po RST */
    eventlog_record ( (uint8_t) EVENTLOG_CAT_CPU_CTRL,
                      (uint8_t) EVENTLOG_CPU_CTRL_SUB_RST_38,
                      pc,
                      0u );

    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.count );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );
    TEST_ASSERT_EQUAL_UINT8  ( EVENTLOG_CAT_CPU_CTRL,        e->category );
    TEST_ASSERT_EQUAL_UINT8  ( EVENTLOG_CPU_CTRL_SUB_RST_38, e->subtype );
    TEST_ASSERT_EQUAL_HEX16  ( pc,                            e->pc );
    TEST_ASSERT_EQUAL_HEX32  ( 0u,                            e->payload );
}


/* ================================================================
 * Vlna 5 Commit 31 - SYS kategorie (lifecycle events)
 *
 * Testy ověřují:
 *   - eventlog_sys_event emituje event s kategorií CAT_SYS, payload 0
 *     pro reset / start / stop
 *   - SNAPSHOT/MZF_INJECT propaguje filename hash do payloadu
 *   - eventlog_filename_hash je stabilní + ignoruje cestu (= jen basename)
 *   - eventlog_filename_hash je bezpečné pro NULL / prázdný string
 *   - gate kontrola: gate OFF nebo mask vypne SYS = no record
 * ================================================================ */

void test_eventlog_sys_emit_cold_reset ( void )
{
    g_eventlog_active = 1;
    g_eventlog_active_mask = UINT64_C ( 0xFFFFFFFFFFFFFFFF );

    eventlog_sys_event ( (uint8_t) EVENTLOG_SYS_COLD_RESET, 0u );

    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.count );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_SYS,         e->category );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_SYS_COLD_RESET,  e->subtype );
    TEST_ASSERT_EQUAL_HEX16 ( 0u,                       e->pc );
    TEST_ASSERT_EQUAL_HEX32 ( 0u,                       e->payload );
}


void test_eventlog_sys_emit_snapshot_save ( void )
{
    g_eventlog_active = 1;
    g_eventlog_active_mask = UINT64_C ( 0xFFFFFFFFFFFFFFFF );

    uint32_t hash = eventlog_filename_hash ( "/tmp/some/dir/save.mzs" );
    eventlog_sys_event ( (uint8_t) EVENTLOG_SYS_SNAPSHOT_SAVE, hash );

    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.count );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_SYS,           e->category );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_SYS_SNAPSHOT_SAVE, e->subtype );
    TEST_ASSERT_EQUAL_HEX32 ( hash,                       e->payload );
    TEST_ASSERT_TRUE         ( hash != 0u );
}


void test_eventlog_sys_emit_mzf_inject ( void )
{
    g_eventlog_active = 1;
    g_eventlog_active_mask = UINT64_C ( 0xFFFFFFFFFFFFFFFF );

    uint32_t hash = eventlog_filename_hash ( "Gardner.mzf" );
    eventlog_sys_event ( (uint8_t) EVENTLOG_SYS_MZF_INJECT, hash );

    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.count );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_SYS,        e->category );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_SYS_MZF_INJECT, e->subtype );
    TEST_ASSERT_EQUAL_HEX32 ( hash,                    e->payload );
}


void test_eventlog_sys_gate_off_no_record ( void )
{
    /* Gate inactive (= active = 0) => eventlog_sys_event no-op. */
    g_eventlog_active = 0;
    g_eventlog_active_mask = UINT64_C ( 0xFFFFFFFFFFFFFFFF );

    eventlog_sys_event ( (uint8_t) EVENTLOG_SYS_COLD_RESET, 0u );
    TEST_ASSERT_EQUAL_size_t ( 0, g_eventlog.count );
}


void test_eventlog_sys_mask_disabled_no_record ( void )
{
    /* Gate active, ale per-kategorie SYS bit clear => no record. */
    g_eventlog_active = 1;
    g_eventlog_active_mask = ~( UINT64_C ( 1 ) << EVENTLOG_CAT_SYS );

    eventlog_sys_event ( (uint8_t) EVENTLOG_SYS_COLD_RESET, 0u );
    TEST_ASSERT_EQUAL_size_t ( 0, g_eventlog.count );

    /* Naopak povolení masky umožní zápis. */
    g_eventlog_active_mask = UINT64_C ( 1 ) << EVENTLOG_CAT_SYS;
    eventlog_sys_event ( (uint8_t) EVENTLOG_SYS_COLD_RESET, 0u );
    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.count );
}


void test_eventlog_filename_hash_basename_only ( void )
{
    /* Stejný basename z různých cest => stejný hash. */
    uint32_t a = eventlog_filename_hash ( "/home/michal/games/Gardner.mzf" );
    uint32_t b = eventlog_filename_hash ( "C:\\Users\\m\\Gardner.mzf" );
    uint32_t c = eventlog_filename_hash ( "Gardner.mzf" );
    TEST_ASSERT_EQUAL_HEX32 ( a, b );
    TEST_ASSERT_EQUAL_HEX32 ( a, c );

    /* Jiný basename => jiný hash (pro běžné stringy s vysokou
     * pravděpodobností). */
    uint32_t d = eventlog_filename_hash ( "Batman.mzf" );
    TEST_ASSERT_NOT_EQUAL ( a, d );
}


void test_eventlog_filename_hash_null_safe ( void )
{
    TEST_ASSERT_EQUAL_HEX32 ( 0u, eventlog_filename_hash ( NULL ) );
    TEST_ASSERT_EQUAL_HEX32 ( 0u, eventlog_filename_hash ( "" ) );
    /* Cesta končící separátorem (= prázdný basename) - taky 0. */
    TEST_ASSERT_EQUAL_HEX32 ( 0u, eventlog_filename_hash ( "/tmp/" ) );
}


/* ================================================================
 * Commit 19 - Pause-on-match callback hook
 *
 * Testy ověřují že:
 *   - Callback je volán z eventlog_record() při aktivním gate.
 *   - Callback NEvolán pokud je gate vypnutý (default OFF).
 *   - Callback dostane správný pointer na event s expected daty.
 *   - destroy() resetuje gate na 0.
 * ================================================================ */

/**
 * @brief Static state pro pause callback testy.
 *
 * Naplňuje se callbackem; testy ji čtou po eventlog_record.
 */
static struct {
    int                 call_count;
    uint8_t             last_category;
    uint8_t             last_subtype;
    uint16_t            last_pc;
    uint32_t            last_payload;
    const void         *last_event_ptr;
} s_pause_cb_state;


static void test_pause_callback_recorder ( const st_EVENTLOG_EVENT *e )
{
    s_pause_cb_state.call_count++;
    if ( e ) {
        s_pause_cb_state.last_category  = e->category;
        s_pause_cb_state.last_subtype   = e->subtype;
        s_pause_cb_state.last_pc        = e->pc;
        s_pause_cb_state.last_payload   = e->payload;
        s_pause_cb_state.last_event_ptr = (const void *) e;
    }
}


static void pause_cb_state_reset ( void )
{
    s_pause_cb_state.call_count = 0;
    s_pause_cb_state.last_category = 0xFF;
    s_pause_cb_state.last_subtype = 0xFF;
    s_pause_cb_state.last_pc = 0xFFFF;
    s_pause_cb_state.last_payload = 0xFFFFFFFFu;
    s_pause_cb_state.last_event_ptr = NULL;
}


void test_eventlog_pause_callback_disabled_gate ( void )
{
    /* Gate vypnutý (default OFF) - callback se nesmí volat ani když je
     * pointer registrován. */
    pause_cb_state_reset ( );
    g_eventlog_pause_callback = test_pause_callback_recorder;
    g_eventlog_pause_trigger_active = 0;

    eventlog_record ( EVENTLOG_CAT_CPU_INT, 0x01, 0x4042, 0x12345678u );
    eventlog_record ( EVENTLOG_CAT_GDG_COLORS, 0x02, 0x4044, 0u );

    TEST_ASSERT_EQUAL_INT ( 0, s_pause_cb_state.call_count );

    /* Cleanup. */
    g_eventlog_pause_callback = NULL;
    g_eventlog_pause_trigger_active = 0;
}


void test_eventlog_pause_callback_active_gate ( void )
{
    /* Gate zapnutý + callback registrován - každý record musí volat
     * callback s pointerem na právě zapsaný event. */
    pause_cb_state_reset ( );
    g_eventlog_pause_callback = test_pause_callback_recorder;
    g_eventlog_pause_trigger_active = 1;

    eventlog_record ( EVENTLOG_CAT_GDG_COLORS, 0x03, 0x4100, 0x000000E0u );

    TEST_ASSERT_EQUAL_INT ( 1, s_pause_cb_state.call_count );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_GDG_COLORS, s_pause_cb_state.last_category );
    TEST_ASSERT_EQUAL_UINT8 ( 0x03,    s_pause_cb_state.last_subtype );
    TEST_ASSERT_EQUAL_HEX16 ( 0x4100u, s_pause_cb_state.last_pc );
    TEST_ASSERT_EQUAL_HEX32 ( 0x000000E0u, s_pause_cb_state.last_payload );
    /* Pointer musí ukazovat do ringu na slot 0 (= první zápis). */
    TEST_ASSERT_EQUAL_PTR ( (const void *) &g_eventlog.events[0],
                            s_pause_cb_state.last_event_ptr );

    /* Druhý record - callback se zavolá zase, count = 2. */
    eventlog_record ( EVENTLOG_CAT_PSG, 0x04, 0x4200, 0x00007F00u );
    TEST_ASSERT_EQUAL_INT ( 2, s_pause_cb_state.call_count );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_PSG, s_pause_cb_state.last_category );

    g_eventlog_pause_callback = NULL;
    g_eventlog_pause_trigger_active = 0;
}


void test_eventlog_pause_callback_null_pointer_safe ( void )
{
    /* Gate zapnutý ale callback NULL - musí být no-op bez crash. */
    pause_cb_state_reset ( );
    g_eventlog_pause_callback = NULL;
    g_eventlog_pause_trigger_active = 1;

    eventlog_record ( EVENTLOG_CAT_CPU_INT, 0x00, 0x4000, 0u );

    /* count stejně se inkrementuje (= callback failure neblokuje record). */
    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.count );
    TEST_ASSERT_EQUAL_INT ( 0, s_pause_cb_state.call_count );

    g_eventlog_pause_trigger_active = 0;
}


void test_eventlog_pause_destroy_resets_gate ( void )
{
    /* destroy() musí resetovat gate. Callback pointer ponecháme
     * (kontrakt: UI registruje znovu při dalším initu). */
    g_eventlog_pause_callback = test_pause_callback_recorder;
    g_eventlog_pause_trigger_active = 1;

    eventlog_destroy ( );

    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_pause_trigger_active );

    /* Re-init pro tearDown. */
    eventlog_init ( EVENTLOG_DEFAULT_CAPACITY );
    g_eventlog_pause_callback = NULL;
}


/* ================================================================
 * Commit 20 - auto-mark on match callback hook
 *
 * Sdílí pattern s pause callback. Druhý nezávislý hook v
 * eventlog_record() - oba mohou běžet paralelně, oba dostanou
 * pointer na právě zapsaný event.
 *
 * Re-entry guard (= skip USER_MARK kategorie) je implementační detail
 * UI callbacku (evw_automark_callback v event_viewer_window.cpp), ne
 * eventlog vrstvy. Testujeme stejnou guard semantiku v test helperu
 * (= regression test: filter match na USER_MARK event nesmí způsobit
 * infinite re-entry).
 * ================================================================ */

/**
 * @brief Static state pro auto-mark callback testy.
 */
static struct {
    int          call_count;
    uint8_t      last_category;
    uint8_t      last_subtype;
    uint16_t     last_pc;
    uint32_t     last_payload;
    const void  *last_event_ptr;
    int          guard_skipped;  /* count skipů kvůli USER_MARK guardu */
} s_automark_cb_state;


static void test_automark_callback_recorder ( const st_EVENTLOG_EVENT *e )
{
    s_automark_cb_state.call_count++;
    if ( e ) {
        s_automark_cb_state.last_category  = e->category;
        s_automark_cb_state.last_subtype   = e->subtype;
        s_automark_cb_state.last_pc        = e->pc;
        s_automark_cb_state.last_payload   = e->payload;
        s_automark_cb_state.last_event_ptr = (const void *) e;
    }
}


/**
 * @brief Test helper: simuluje guard pattern z evw_automark_callback.
 *
 * Skipne události s kategorií USER_MARK. Pro regression test
 * infinite re-entry by tato funkce při bug-fix v eventlog vrstvě
 * nadále chránila proti smyčce.
 */
static void test_automark_callback_with_user_mark_guard ( const st_EVENTLOG_EVENT *e )
{
    if ( !e ) return;
    if ( e->category == EVENTLOG_CAT_USER_MARK ) {
        s_automark_cb_state.guard_skipped++;
        return;
    }
    s_automark_cb_state.call_count++;
    s_automark_cb_state.last_category = e->category;
}


static void automark_cb_state_reset ( void )
{
    s_automark_cb_state.call_count = 0;
    s_automark_cb_state.last_category = 0xFF;
    s_automark_cb_state.last_subtype = 0xFF;
    s_automark_cb_state.last_pc = 0xFFFF;
    s_automark_cb_state.last_payload = 0xFFFFFFFFu;
    s_automark_cb_state.last_event_ptr = NULL;
    s_automark_cb_state.guard_skipped = 0;
}


void test_eventlog_automark_callback_disabled_gate ( void )
{
    /* Gate vypnutý - callback se nesmí volat ani když je pointer
     * registrován. Pause callback OFF, izolovaná aut-mark cesta. */
    automark_cb_state_reset ( );
    g_eventlog_automark_callback = test_automark_callback_recorder;
    g_eventlog_automark_trigger_active = 0;

    eventlog_record ( EVENTLOG_CAT_PSG, 0x01, 0x4042, 0x12345678u );
    eventlog_record ( EVENTLOG_CAT_GDG_COLORS, 0x02, 0x4044, 0u );

    TEST_ASSERT_EQUAL_INT ( 0, s_automark_cb_state.call_count );

    g_eventlog_automark_callback = NULL;
    g_eventlog_automark_trigger_active = 0;
}


void test_eventlog_automark_callback_active_gate ( void )
{
    /* Gate ON + callback registrován - každý record volá callback s
     * pointerem na zapsaný event. */
    automark_cb_state_reset ( );
    g_eventlog_automark_callback = test_automark_callback_recorder;
    g_eventlog_automark_trigger_active = 1;

    eventlog_record ( EVENTLOG_CAT_PSG, 0x05, 0x4100, 0x000000E0u );

    TEST_ASSERT_EQUAL_INT ( 1, s_automark_cb_state.call_count );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_PSG, s_automark_cb_state.last_category );
    TEST_ASSERT_EQUAL_UINT8 ( 0x05,    s_automark_cb_state.last_subtype );
    TEST_ASSERT_EQUAL_HEX16 ( 0x4100u, s_automark_cb_state.last_pc );
    TEST_ASSERT_EQUAL_HEX32 ( 0x000000E0u, s_automark_cb_state.last_payload );
    TEST_ASSERT_EQUAL_PTR ( (const void *) &g_eventlog.events[0],
                            s_automark_cb_state.last_event_ptr );

    eventlog_record ( EVENTLOG_CAT_GDG_COLORS, 0x06, 0x4200, 0x00007F00u );
    TEST_ASSERT_EQUAL_INT ( 2, s_automark_cb_state.call_count );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_GDG_COLORS,
                              s_automark_cb_state.last_category );

    g_eventlog_automark_callback = NULL;
    g_eventlog_automark_trigger_active = 0;
}


void test_eventlog_automark_callback_null_pointer_safe ( void )
{
    /* Gate ON ale callback NULL - no-op bez crash. */
    automark_cb_state_reset ( );
    g_eventlog_automark_callback = NULL;
    g_eventlog_automark_trigger_active = 1;

    eventlog_record ( EVENTLOG_CAT_PSG, 0x00, 0x4000, 0u );

    TEST_ASSERT_EQUAL_size_t ( 1, g_eventlog.count );
    TEST_ASSERT_EQUAL_INT ( 0, s_automark_cb_state.call_count );

    g_eventlog_automark_trigger_active = 0;
}


void test_eventlog_automark_destroy_resets_gate ( void )
{
    /* destroy() musí resetovat automark gate (analogicky s pause). */
    g_eventlog_automark_callback = test_automark_callback_recorder;
    g_eventlog_automark_trigger_active = 1;

    eventlog_destroy ( );

    TEST_ASSERT_EQUAL_INT ( 0, g_eventlog_automark_trigger_active );

    eventlog_init ( EVENTLOG_DEFAULT_CAPACITY );
    g_eventlog_automark_callback = NULL;
}


void test_eventlog_automark_independent_from_pause ( void )
{
    /* Oba gate ON + oba callbacky registrované - oba dostanou stejný
     * event pointer. Ověříme že jsou nezávislé (= žádný neblokuje
     * druhý). */
    pause_cb_state_reset ( );
    automark_cb_state_reset ( );
    g_eventlog_pause_callback = test_pause_callback_recorder;
    g_eventlog_pause_trigger_active = 1;
    g_eventlog_automark_callback = test_automark_callback_recorder;
    g_eventlog_automark_trigger_active = 1;

    eventlog_record ( EVENTLOG_CAT_PSG, 0x07, 0x4300, 0xDEADBEEFu );

    TEST_ASSERT_EQUAL_INT ( 1, s_pause_cb_state.call_count );
    TEST_ASSERT_EQUAL_INT ( 1, s_automark_cb_state.call_count );
    /* Oba dostali stejný pointer (= ten samý event v ringu). */
    TEST_ASSERT_EQUAL_PTR ( s_pause_cb_state.last_event_ptr,
                            s_automark_cb_state.last_event_ptr );

    g_eventlog_pause_callback = NULL;
    g_eventlog_pause_trigger_active = 0;
    g_eventlog_automark_callback = NULL;
    g_eventlog_automark_trigger_active = 0;
}


void test_eventlog_automark_user_mark_guard ( void )
{
    /* Regression test: callback s USER_MARK guardem nesmí počítat
     * záznamy kategorie USER_MARK (= replicuje guard z UI callbacku).
     * Mock-style test: UI callback static v cpp není přístupný,
     * tady ověříme že stejný guard pattern funguje pro libovolný
     * USER_MARK event jdoucí přes eventlog_record. */
    automark_cb_state_reset ( );
    g_eventlog_automark_callback = test_automark_callback_with_user_mark_guard;
    g_eventlog_automark_trigger_active = 1;

    /* Ne-USER_MARK event - callback ho započítá. */
    eventlog_record ( EVENTLOG_CAT_PSG, 0x00, 0x4000, 0u );
    TEST_ASSERT_EQUAL_INT ( 1, s_automark_cb_state.call_count );
    TEST_ASSERT_EQUAL_INT ( 0, s_automark_cb_state.guard_skipped );

    /* USER_MARK event (= simulace fan-outu z marklog_record) -
     * callback ho SKIPNE. To zabraňuje infinite re-entry. */
    eventlog_record ( EVENTLOG_CAT_USER_MARK, 0, 0x4100, 0x0042u );
    TEST_ASSERT_EQUAL_INT ( 1, s_automark_cb_state.call_count );
    TEST_ASSERT_EQUAL_INT ( 1, s_automark_cb_state.guard_skipped );

    /* Další ne-USER_MARK - opět započítán (= guard se nemění). */
    eventlog_record ( EVENTLOG_CAT_GDG_COLORS, 0x02, 0x4200, 0u );
    TEST_ASSERT_EQUAL_INT ( 2, s_automark_cb_state.call_count );
    TEST_ASSERT_EQUAL_INT ( 1, s_automark_cb_state.guard_skipped );

    g_eventlog_automark_callback = NULL;
    g_eventlog_automark_trigger_active = 0;
}


/* ================================================================
 * Commit 24 - ambient state bits (Vlna 4)
 * ================================================================ */

#include "debugger/bp_event.h"

/**
 * Ambient capture v idle stavu: po čerstvém init (= mzarch_platform_fn_init
 * v mztest_init udělal reset CPU), IFF1=0, IM=0, reason=NONE,
 * banking=DEFAULT na všech architekturách.
 *
 * Test ověřuje, že capture zapisuje sane defaults (= žádný garbage v
 * ambient poli) a že reason field je správně přemapovaný z 0xFF na
 * EVENTLOG_AMBIENT_REASON_NONE.
 */
void test_eventlog_ambient_default_after_init ( void )
{
    /* Po čerstvém setUp je g_bp_fire_reason = BP_REASON_NONE (0xFF).
     * CPU má IFF1=0, IM=0 po power-on reset. */
    g_bp_fire_reason = (uint8_t) BP_REASON_NONE;
    g_mzarch_main.cpu->iff1 = 0;
    g_mzarch_main.cpu->im   = 0;

    eventlog_record ( EVENTLOG_CAT_PSG, 0x00, 0x1000, 0u );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );

    /* IFF1 = 0, IM = 0 -> bity 0..2 nuly. */
    TEST_ASSERT_EQUAL_UINT16 ( 0, e->ambient & EVENTLOG_AMBIENT_IFF1 );
    TEST_ASSERT_EQUAL_UINT16 ( 0, e->ambient & EVENTLOG_AMBIENT_IM_MASK );

    /* Reason NONE = 7 v ambient (= 0xFF v g_bp_fire_reason). */
    uint8_t reason = (uint8_t) ( ( e->ambient & EVENTLOG_AMBIENT_REASON_MASK )
                                 >> EVENTLOG_AMBIENT_REASON_SHIFT );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_AMBIENT_REASON_NONE, reason );

    /* Rezervované bity 9..15 musí být 0. */
    TEST_ASSERT_EQUAL_UINT16 ( 0, e->ambient & EVENTLOG_AMBIENT_RESERVED_MASK );
}


/**
 * Capture s IFF1=1 a IM=2: ověř že bity 0 + 1..2 odpovídají.
 */
void test_eventlog_ambient_capture_iff1_im2 ( void )
{
    g_bp_fire_reason = (uint8_t) BP_REASON_NONE;
    g_mzarch_main.cpu->iff1 = 1;
    g_mzarch_main.cpu->im   = 2;

    eventlog_record ( EVENTLOG_CAT_PSG, 0x00, 0x2000, 0u );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );

    TEST_ASSERT_TRUE ( ( e->ambient & EVENTLOG_AMBIENT_IFF1 ) != 0 );
    uint8_t im = (uint8_t) ( ( e->ambient & EVENTLOG_AMBIENT_IM_MASK )
                             >> EVENTLOG_AMBIENT_IM_SHIFT );
    TEST_ASSERT_EQUAL_UINT8 ( 2, im );

    /* IM=2 + IFF1=1 reset (cleanup). */
    g_mzarch_main.cpu->iff1 = 0;
    g_mzarch_main.cpu->im   = 0;
}


/**
 * Reason field encoding: g_bp_fire_reason = INT_ACK (3) -> ambient reason = 3.
 *
 * Po emit reset reason na NONE, aby další testy neměly garbage.
 */
void test_eventlog_ambient_reason_int_ack ( void )
{
    g_bp_fire_reason = (uint8_t) BP_REASON_IFF_INT_ACK;
    g_mzarch_main.cpu->iff1 = 0;
    g_mzarch_main.cpu->im   = 1;

    eventlog_record ( EVENTLOG_CAT_CPU_INT, 0x00, 0x3000, 0u );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );

    uint8_t reason = (uint8_t) ( ( e->ambient & EVENTLOG_AMBIENT_REASON_MASK )
                                 >> EVENTLOG_AMBIENT_REASON_SHIFT );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_AMBIENT_REASON_IFF_INT_ACK, reason );

    uint8_t im = (uint8_t) ( ( e->ambient & EVENTLOG_AMBIENT_IM_MASK )
                             >> EVENTLOG_AMBIENT_IM_SHIFT );
    TEST_ASSERT_EQUAL_UINT8 ( 1, im );

    g_bp_fire_reason = (uint8_t) BP_REASON_NONE;
    g_mzarch_main.cpu->im = 0;
}


/**
 * Reason NONE (0xFF) v g_bp_fire_reason MUSÍ se přemapovat na 7
 * v ambient (= 0xFF se do 3 bitů nevejde).
 */
void test_eventlog_ambient_reason_none_maps_to_seven ( void )
{
    g_bp_fire_reason = (uint8_t) BP_REASON_NONE; /* 0xFF */

    eventlog_record ( EVENTLOG_CAT_PSG, 0x00, 0x4000, 0u );
    const st_EVENTLOG_EVENT *e = eventlog_get_event ( 0 );
    TEST_ASSERT_NOT_NULL ( e );

    uint8_t reason = (uint8_t) ( ( e->ambient & EVENTLOG_AMBIENT_REASON_MASK )
                                 >> EVENTLOG_AMBIENT_REASON_SHIFT );
    TEST_ASSERT_EQUAL_UINT8 ( 7u, reason );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_AMBIENT_REASON_NONE, reason );
}


/**
 * Struct size = 32 B (= explicitní compile + runtime check).
 *
 * Layout: 24 B původní + 2 B ambient + 6 B padding/reserved (alignment 8 B).
 */
void test_eventlog_ambient_struct_size_26 ( void )
{
    TEST_ASSERT_EQUAL_size_t ( 32, sizeof ( st_EVENTLOG_EVENT ) );
}


/* ================================================================
 * Main
 * ================================================================ */

/* ================================================================
 * Commit 29 - eventlog export / import (Vlna 4)
 * ================================================================ */

#include <stdlib.h>


/**
 * @brief Sestaví unikátní temp cestu pro test soubor.
 *
 * Používáme TMPDIR/TEMP/TMP nebo /tmp + g_get_monotonic_time + counter,
 * abychom v paralelním test runu (= ctest -j N) nesráželi soubory.
 */
static void evt_tmp_path ( char *buf, size_t bufsz, const char *suffix )
{
    const char *tmp = g_get_tmp_dir ( );
    static int counter = 0;
    counter++;
    snprintf ( buf, bufsz, "%s/evt_export_test_%lld_%d_%s.bin",
               tmp, (long long) g_get_monotonic_time ( ), counter,
               suffix ? suffix : "x" );
}


void test_eventlog_export_empty_ring ( void )
{
    char path[ 512 ];
    evt_tmp_path ( path, sizeof ( path ), "empty" );

    TEST_ASSERT_EQUAL_size_t ( 0, eventlog_get_count ( ) );
    TEST_ASSERT_EQUAL_INT ( 0, eventlog_export_to_file ( path ) );

    /* Header musí být 32 B, count = 0, žádné records za ním. */
    FILE *fp = fopen ( path, "rb" );
    TEST_ASSERT_NOT_NULL ( fp );
    fseek ( fp, 0, SEEK_END );
    long sz = ftell ( fp );
    TEST_ASSERT_EQUAL_INT ( 32, (int) sz );

    fseek ( fp, 0, SEEK_SET );
    st_EVENTLOG_EXPORT_HEADER hdr;
    TEST_ASSERT_EQUAL_size_t ( 1, fread ( &hdr, sizeof ( hdr ), 1, fp ) );
    TEST_ASSERT_EQUAL_INT ( 0, memcmp ( hdr.magic, "MZEVTLOG", 8 ) );
    TEST_ASSERT_EQUAL_UINT32 ( EVENTLOG_EXPORT_VERSION, hdr.version );
    TEST_ASSERT_EQUAL_UINT32 ( 32u, hdr.record_size );
    TEST_ASSERT_EQUAL_UINT64 ( 0u, hdr.record_count );
    fclose ( fp );

    remove ( path );
}


void test_eventlog_export_single_event ( void )
{
    char path[ 512 ];
    evt_tmp_path ( path, sizeof ( path ), "single" );

    eventlog_record ( EVENTLOG_CAT_IORQ_OUT, 0x12, 0x4042, 0xABCDu );
    TEST_ASSERT_EQUAL_INT ( 0, eventlog_export_to_file ( path ) );

    /* 32 B header + 32 B record = 64 B. */
    FILE *fp = fopen ( path, "rb" );
    TEST_ASSERT_NOT_NULL ( fp );
    fseek ( fp, 0, SEEK_END );
    long sz = ftell ( fp );
    TEST_ASSERT_EQUAL_INT ( 64, (int) sz );

    fseek ( fp, 0, SEEK_SET );
    st_EVENTLOG_EXPORT_HEADER hdr;
    TEST_ASSERT_EQUAL_size_t ( 1, fread ( &hdr, sizeof ( hdr ), 1, fp ) );
    TEST_ASSERT_EQUAL_UINT64 ( 1u, hdr.record_count );

    st_EVENTLOG_EVENT e;
    TEST_ASSERT_EQUAL_size_t ( 1, fread ( &e, sizeof ( e ), 1, fp ) );
    TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_IORQ_OUT, e.category );
    TEST_ASSERT_EQUAL_UINT8 ( 0x12, e.subtype );
    TEST_ASSERT_EQUAL_HEX16 ( 0x4042, e.pc );
    TEST_ASSERT_EQUAL_HEX32 ( 0xABCDu, e.payload );
    fclose ( fp );

    remove ( path );
}


void test_eventlog_export_full_ring_chronological ( void )
{
    char path[ 512 ];
    evt_tmp_path ( path, sizeof ( path ), "full" );

    /* Capacity MIN, zapíšeme 1.5x = wrap. Po wrapu obsahuje ring eventy
     * z indexu [count/2 .. total-1] (= staré jsou přepsané). Export
     * MUSÍ projít chronologicky (oldest first), takže první record v
     * souboru je payload (total - capacity), poslední payload (total - 1). */
    eventlog_set_capacity ( EVENTLOG_MIN_CAPACITY );
    size_t cap   = g_eventlog.capacity;
    size_t total = cap + cap / 2;

    for ( size_t i = 0; i < total; i++ ) {
        eventlog_record ( EVENTLOG_CAT_CPU_INT, 0, (uint16_t) ( i & 0xFFFFu ),
                          (uint32_t) i );
    }
    TEST_ASSERT_EQUAL_size_t ( cap, eventlog_get_count ( ) );
    TEST_ASSERT_TRUE ( g_eventlog.overflow );

    TEST_ASSERT_EQUAL_INT ( 0, eventlog_export_to_file ( path ) );

    FILE *fp = fopen ( path, "rb" );
    TEST_ASSERT_NOT_NULL ( fp );
    st_EVENTLOG_EXPORT_HEADER hdr;
    TEST_ASSERT_EQUAL_size_t ( 1, fread ( &hdr, sizeof ( hdr ), 1, fp ) );
    TEST_ASSERT_EQUAL_UINT64 ( (uint64_t) cap, hdr.record_count );

    /* První record = oldest přeživší = payload (total - cap). */
    st_EVENTLOG_EVENT first;
    TEST_ASSERT_EQUAL_size_t ( 1, fread ( &first, sizeof ( first ), 1, fp ) );
    TEST_ASSERT_EQUAL_UINT32 ( (uint32_t) ( total - cap ), first.payload );

    /* Poslední record = newest = payload (total - 1). Seek na konec. */
    fseek ( fp, (long) ( sizeof ( hdr ) + ( cap - 1 ) * sizeof ( st_EVENTLOG_EVENT ) ),
            SEEK_SET );
    st_EVENTLOG_EVENT last;
    TEST_ASSERT_EQUAL_size_t ( 1, fread ( &last, sizeof ( last ), 1, fp ) );
    TEST_ASSERT_EQUAL_UINT32 ( (uint32_t) ( total - 1 ), last.payload );
    fclose ( fp );

    remove ( path );
}


void test_eventlog_import_roundtrip ( void )
{
    char path[ 512 ];
    evt_tmp_path ( path, sizeof ( path ), "roundtrip" );

    /* Zapiš 5 různých events, export, clear, import, ověř identitu. */
    for ( unsigned i = 0; i < 5; i++ ) {
        eventlog_record ( EVENTLOG_CAT_GDG_MODE, (uint8_t) ( i + 1 ),
                          (uint16_t) ( 0x1000 + i ), 0xDEAD0000u + i );
    }
    TEST_ASSERT_EQUAL_INT ( 0, eventlog_export_to_file ( path ) );

    eventlog_clear ( );
    TEST_ASSERT_EQUAL_size_t ( 0, eventlog_get_count ( ) );

    TEST_ASSERT_EQUAL_INT ( 0, eventlog_import_from_file ( path ) );
    TEST_ASSERT_EQUAL_size_t ( 5, eventlog_get_count ( ) );

    for ( unsigned i = 0; i < 5; i++ ) {
        const st_EVENTLOG_EVENT *e = eventlog_get_event ( i );
        TEST_ASSERT_NOT_NULL ( e );
        TEST_ASSERT_EQUAL_UINT8 ( EVENTLOG_CAT_GDG_MODE, e->category );
        TEST_ASSERT_EQUAL_UINT8 ( (uint8_t) ( i + 1 ), e->subtype );
        TEST_ASSERT_EQUAL_HEX16 ( (uint16_t) ( 0x1000 + i ), e->pc );
        TEST_ASSERT_EQUAL_HEX32 ( 0xDEAD0000u + i, e->payload );
    }

    remove ( path );
}


void test_eventlog_import_invalid_magic ( void )
{
    char path[ 512 ];
    evt_tmp_path ( path, sizeof ( path ), "badmagic" );

    /* Zapiš fake hlavičku s wrong magic. */
    st_EVENTLOG_EXPORT_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );
    memcpy ( hdr.magic, "BADMAGIC", 8 );
    hdr.version      = EVENTLOG_EXPORT_VERSION;
    hdr.record_size  = (uint32_t) sizeof ( st_EVENTLOG_EVENT );
    hdr.record_count = 0;
    hdr.timestamp    = 0;

    FILE *fp = fopen ( path, "wb" );
    TEST_ASSERT_NOT_NULL ( fp );
    fwrite ( &hdr, sizeof ( hdr ), 1, fp );
    fclose ( fp );

    /* Naplň ring nějakým eventem - import s chybou ho NESMÍ vymazat. */
    eventlog_record ( EVENTLOG_CAT_CPU_INT, 0, 0x1234, 0x5678 );
    size_t before = eventlog_get_count ( );

    TEST_ASSERT_EQUAL_INT ( -1, eventlog_import_from_file ( path ) );
    /* Po error import zachová předchozí ring obsah (= clear se nikdy
     * nezavolal, protože validace selhala před clear / fread). */
    TEST_ASSERT_EQUAL_size_t ( before, eventlog_get_count ( ) );

    remove ( path );
}


void test_eventlog_import_wrong_record_size ( void )
{
    char path[ 512 ];
    evt_tmp_path ( path, sizeof ( path ), "badrecsz" );

    st_EVENTLOG_EXPORT_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );
    memcpy ( hdr.magic, "MZEVTLOG", 8 );
    hdr.version      = EVENTLOG_EXPORT_VERSION;
    hdr.record_size  = 24u;  /* Wrong - aktuální je 32. */
    hdr.record_count = 0;
    hdr.timestamp    = 0;

    FILE *fp = fopen ( path, "wb" );
    TEST_ASSERT_NOT_NULL ( fp );
    fwrite ( &hdr, sizeof ( hdr ), 1, fp );
    fclose ( fp );

    TEST_ASSERT_EQUAL_INT ( -1, eventlog_import_from_file ( path ) );

    remove ( path );
}


void test_eventlog_import_wrong_version ( void )
{
    char path[ 512 ];
    evt_tmp_path ( path, sizeof ( path ), "badver" );

    st_EVENTLOG_EXPORT_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );
    memcpy ( hdr.magic, "MZEVTLOG", 8 );
    hdr.version      = 99u;  /* Unsupported. */
    hdr.record_size  = (uint32_t) sizeof ( st_EVENTLOG_EVENT );
    hdr.record_count = 0;
    hdr.timestamp    = 0;

    FILE *fp = fopen ( path, "wb" );
    TEST_ASSERT_NOT_NULL ( fp );
    fwrite ( &hdr, sizeof ( hdr ), 1, fp );
    fclose ( fp );

    TEST_ASSERT_EQUAL_INT ( -1, eventlog_import_from_file ( path ) );

    remove ( path );
}


void test_eventlog_import_capacity_resize ( void )
{
    char path[ 512 ];
    evt_tmp_path ( path, sizeof ( path ), "resize" );

    /* Vytvoř export se 30000 events při dočasně zvětšené capacity. */
    eventlog_set_capacity ( 35000 );
    for ( unsigned i = 0; i < 30000; i++ ) {
        eventlog_record ( EVENTLOG_CAT_CPU_INT, 0, (uint16_t) i, i );
    }
    TEST_ASSERT_EQUAL_INT ( 0, eventlog_export_to_file ( path ) );

    /* Resize ring na MIN (= 10000), pak import: capacity se musí
     * zvětšit zpět na alespoň 30000. */
    eventlog_set_capacity ( EVENTLOG_MIN_CAPACITY );
    TEST_ASSERT_EQUAL_size_t ( EVENTLOG_MIN_CAPACITY, g_eventlog.capacity );

    TEST_ASSERT_EQUAL_INT ( 0, eventlog_import_from_file ( path ) );
    TEST_ASSERT_TRUE ( g_eventlog.capacity >= 30000 );
    TEST_ASSERT_EQUAL_size_t ( 30000, eventlog_get_count ( ) );

    /* Spot check pár records. */
    const st_EVENTLOG_EVENT *e0     = eventlog_get_event ( 0 );
    const st_EVENTLOG_EVENT *e_last = eventlog_get_event ( 29999 );
    TEST_ASSERT_NOT_NULL ( e0 );
    TEST_ASSERT_NOT_NULL ( e_last );
    TEST_ASSERT_EQUAL_UINT32 ( 0u, e0->payload );
    TEST_ASSERT_EQUAL_UINT32 ( 29999u, e_last->payload );

    remove ( path );
}


void test_eventlog_import_truncated_file ( void )
{
    char path[ 512 ];
    evt_tmp_path ( path, sizeof ( path ), "trunc" );

    /* Hlavička claim-uje 10 records, ale soubor obsahuje jen 3. */
    st_EVENTLOG_EXPORT_HEADER hdr;
    memset ( &hdr, 0, sizeof ( hdr ) );
    memcpy ( hdr.magic, "MZEVTLOG", 8 );
    hdr.version      = EVENTLOG_EXPORT_VERSION;
    hdr.record_size  = (uint32_t) sizeof ( st_EVENTLOG_EVENT );
    hdr.record_count = 10;
    hdr.timestamp    = 0;

    FILE *fp = fopen ( path, "wb" );
    TEST_ASSERT_NOT_NULL ( fp );
    fwrite ( &hdr, sizeof ( hdr ), 1, fp );
    st_EVENTLOG_EVENT e;
    memset ( &e, 0, sizeof ( e ) );
    for ( int i = 0; i < 3; i++ ) {
        e.payload = (uint32_t) i;
        fwrite ( &e, sizeof ( e ), 1, fp );
    }
    fclose ( fp );

    TEST_ASSERT_EQUAL_INT ( -1, eventlog_import_from_file ( path ) );
    /* Po partial import obsahuje ring 3 záznamy (= lepší než nic). */
    TEST_ASSERT_EQUAL_size_t ( 3, eventlog_get_count ( ) );

    remove ( path );
}


void test_eventlog_export_null_path ( void )
{
    TEST_ASSERT_EQUAL_INT ( -1, eventlog_export_to_file ( NULL ) );
    TEST_ASSERT_EQUAL_INT ( -1, eventlog_export_to_file ( "" ) );
}


void test_eventlog_import_null_path ( void )
{
    TEST_ASSERT_EQUAL_INT ( -1, eventlog_import_from_file ( NULL ) );
    TEST_ASSERT_EQUAL_INT ( -1, eventlog_import_from_file ( "" ) );
}


void test_eventlog_export_header_size_is_32 ( void )
{
    /* Static_assert v headeru chrání compile-time, runtime potvrzení. */
    TEST_ASSERT_EQUAL_size_t ( 32, sizeof ( st_EVENTLOG_EXPORT_HEADER ) );
}


int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    RUN_TEST ( test_eventlog_record_size_is_24_bytes );
    RUN_TEST ( test_eventlog_category_count_fits_in_64bit_mask );

    RUN_TEST ( test_eventlog_init_default_capacity );
    RUN_TEST ( test_eventlog_init_clamps_below_min );
    RUN_TEST ( test_eventlog_init_clamps_above_max );
    RUN_TEST ( test_eventlog_destroy_cleans_up );

    RUN_TEST ( test_eventlog_basic_write_read );
    RUN_TEST ( test_eventlog_get_event_out_of_range );
    RUN_TEST ( test_eventlog_get_count_matches_writes );

    RUN_TEST ( test_eventlog_ring_wrap );
    RUN_TEST ( test_eventlog_seq_and_segment );
    RUN_TEST ( test_eventlog_clear );
    RUN_TEST ( test_eventlog_set_capacity_resizes_and_clears );
    RUN_TEST ( test_eventlog_set_capacity_clamps );

    RUN_TEST ( test_eventlog_start_stop );
    RUN_TEST ( test_eventlog_start_fails_without_ring );

    RUN_TEST ( test_eventlog_mask_filter_caller_gate );
    RUN_TEST ( test_eventlog_recompute_active_modes );

    /* Commit 2 fan-out hooky. */
    RUN_TEST ( test_eventlog_fanout_hwlog_gdg_mode );
    RUN_TEST ( test_eventlog_fanout_intlog_pin_edge );
    RUN_TEST ( test_eventlog_fanout_marklog );
    RUN_TEST ( test_eventlog_fanout_mask_disabled );
    RUN_TEST ( test_eventlog_fanout_inactive );
    RUN_TEST ( test_eventlog_fanout_hwlog_unmapped_chip );
    RUN_TEST ( test_eventlog_fanout_multi_categories_independent );
    RUN_TEST ( test_eventlog_fanout_new_categories_count );

    /* Commit 2.1 - IORQ subtype NORMAL vs UNCONNECTED. */
    RUN_TEST ( test_eventlog_iorq_subtype_enum_values );
    RUN_TEST ( test_eventlog_iorq_subtype_normal );
    RUN_TEST ( test_eventlog_iorq_subtype_unconnected );

    /* Commit 3 - BP_FIRE kategorie. */
    RUN_TEST ( test_eventlog_bp_fire_subtype_enum_values );
    RUN_TEST ( test_eventlog_bp_fire_emit );
    RUN_TEST ( test_eventlog_bp_fire_classify_subtype );

    /* Commit 5 - CPU_CTRL kategorie (HALT enter/exit, RST 00..38). */
    RUN_TEST ( test_eventlog_cpu_ctrl_subtype_enum_values );
    RUN_TEST ( test_eventlog_cpu_ctrl_emit_halt_enter );
    RUN_TEST ( test_eventlog_cpu_ctrl_emit_rst_38 );

    /* Commit 19 - pause-on-match callback hook. */
    RUN_TEST ( test_eventlog_pause_callback_disabled_gate );
    RUN_TEST ( test_eventlog_pause_callback_active_gate );
    RUN_TEST ( test_eventlog_pause_callback_null_pointer_safe );
    RUN_TEST ( test_eventlog_pause_destroy_resets_gate );

    /* Commit 20 - auto-mark on match callback hook. */
    RUN_TEST ( test_eventlog_automark_callback_disabled_gate );
    RUN_TEST ( test_eventlog_automark_callback_active_gate );
    RUN_TEST ( test_eventlog_automark_callback_null_pointer_safe );
    RUN_TEST ( test_eventlog_automark_destroy_resets_gate );
    RUN_TEST ( test_eventlog_automark_independent_from_pause );
    RUN_TEST ( test_eventlog_automark_user_mark_guard );

    /* Commit 24 - ambient state bits (Vlna 4). */
    RUN_TEST ( test_eventlog_ambient_struct_size_26 );
    RUN_TEST ( test_eventlog_ambient_default_after_init );
    RUN_TEST ( test_eventlog_ambient_capture_iff1_im2 );
    RUN_TEST ( test_eventlog_ambient_reason_int_ack );
    RUN_TEST ( test_eventlog_ambient_reason_none_maps_to_seven );

    /* Commit 29 - eventlog export / import (Vlna 4). */
    RUN_TEST ( test_eventlog_export_header_size_is_32 );
    RUN_TEST ( test_eventlog_export_empty_ring );
    RUN_TEST ( test_eventlog_export_single_event );
    RUN_TEST ( test_eventlog_export_full_ring_chronological );
    RUN_TEST ( test_eventlog_import_roundtrip );
    RUN_TEST ( test_eventlog_import_invalid_magic );
    RUN_TEST ( test_eventlog_import_wrong_record_size );
    RUN_TEST ( test_eventlog_import_wrong_version );
    RUN_TEST ( test_eventlog_import_capacity_resize );
    RUN_TEST ( test_eventlog_import_truncated_file );
    RUN_TEST ( test_eventlog_export_null_path );
    RUN_TEST ( test_eventlog_import_null_path );

    /* Vlna 5 Commit 31 - SYS kategorie (lifecycle events). */
    RUN_TEST ( test_eventlog_sys_emit_cold_reset );
    RUN_TEST ( test_eventlog_sys_emit_snapshot_save );
    RUN_TEST ( test_eventlog_sys_emit_mzf_inject );
    RUN_TEST ( test_eventlog_sys_gate_off_no_record );
    RUN_TEST ( test_eventlog_sys_mask_disabled_no_record );
    RUN_TEST ( test_eventlog_filename_hash_basename_only );
    RUN_TEST ( test_eventlog_filename_hash_null_safe );

    return UNITY_END ( );
}
