/**
 * @file test_debugger_init_flag.c
 * @brief Test příznaku debugger_is_initialized().
 *
 * debugger_init() běží v emu vlákně až po inicializaci video rozhraní, UI
 * vlákno už v tu chvíli vykresluje. Okna debuggeru (main_window.cpp) se
 * proto kreslí jen při debugger_is_initialized() != 0; dřív okno Breakpoints
 * četlo g_breakpoints.breakpoints == NULL a padalo.
 *
 * Ověřuje se:
 *  - před inicializací emulátoru (mztest_init -> mzarch_platform_fn_init ->
 *    debugger_init) příznak 0,
 *  - po inicializaci 1 a stav, který okna čtou, existuje (g_breakpoints).
 *
 * Nepokrývá: debugger_exit() (úklid dělá mztest_teardown) ani souběh vláken
 * samotný - ten v headless testu nevznikne.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "debugger/debugger.h"
#include "debugger/breakpoints.h"

/** @brief Hodnota příznaku zjištěná v main() před mztest_init(). */
static int s_flag_before_init = -1;

void setUp ( void ) { }
void tearDown ( void ) { }


/** Před debugger_init() okna debuggeru nesmí kreslit. */
void test_flag_is_zero_before_init ( void )
{
    TEST_ASSERT_EQUAL_INT ( 0, s_flag_before_init );
}


/** Po debugger_init() je příznak nastavený a stav pro okna existuje. */
void test_flag_is_set_after_init ( void )
{
    TEST_ASSERT_NOT_EQUAL ( 0, debugger_is_initialized ( ) );
    TEST_ASSERT_NOT_NULL ( g_breakpoints.breakpoints );
    TEST_ASSERT_NOT_NULL ( g_breakpoints.groups );
}


int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    s_flag_before_init = debugger_is_initialized ( );
    mztest_init ( );

    UNITY_BEGIN ( );
    RUN_TEST ( test_flag_is_zero_before_init );
    RUN_TEST ( test_flag_is_set_after_init );
    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
