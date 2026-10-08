/**
 * @file   test_cmtspeed.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Testy knihovny cmtspeed — rychlosti CMT pásky.
 *
 * Testuje: enum hodnoty, divisor pole, ratio řetězce, inline funkce
 *          (get_divisor, get_bdspeed, get_speedtxt, get_ratiospeedtxt),
 *          bounds checking, is_valid.
 *
 * Standalone — nepotřebuje mztest_init ani emulátor.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní revize a refaktorizace. Vytvořeny unit testy.
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "unity.h"

#include <string.h>
#include <math.h>

#include "libs/cmtspeed/cmtspeed.h"


/* ========================================================================
 * setUp / tearDown
 * ======================================================================== */

/** @brief Inicializace před každým testem (prázdná — není potřeba). */
void setUp ( void ) {}

/** @brief Úklid po každém testu (prázdný — není potřeba). */
void tearDown ( void ) {}


/* ========================================================================
 * SMOKE testy — základní invarianty
 * ======================================================================== */

/** @brief Testuje, že divisor pro CMTSPEED_NONE je 0. */
static void test_divisor_none_is_zero ( void ) {
    TEST_ASSERT_EQUAL_DOUBLE ( 0.0, cmtspeed_get_divisor ( CMTSPEED_NONE ) );
}


/** @brief Testuje, že divisor pro CMTSPEED_1_1 je 1.0. */
static void test_divisor_1_1 ( void ) {
    TEST_ASSERT_EQUAL_DOUBLE ( 1.0, cmtspeed_get_divisor ( CMTSPEED_1_1 ) );
}


/** @brief Testuje správné hodnoty divisorů pro všechny platné rychlosti. */
static void test_divisor_values ( void ) {
    TEST_ASSERT_EQUAL_DOUBLE ( 2.0, cmtspeed_get_divisor ( CMTSPEED_2_1 ) );
    TEST_ASSERT_EQUAL_DOUBLE ( 2.0, cmtspeed_get_divisor ( CMTSPEED_2_1_CPM ) );
    TEST_ASSERT_EQUAL_DOUBLE ( 3.0, cmtspeed_get_divisor ( CMTSPEED_3_1 ) );
    TEST_ASSERT_DOUBLE_WITHIN ( 0.001, 1.5, cmtspeed_get_divisor ( CMTSPEED_3_2 ) );
    TEST_ASSERT_DOUBLE_WITHIN ( 0.001, 2.333, cmtspeed_get_divisor ( CMTSPEED_7_3 ) );
    TEST_ASSERT_DOUBLE_WITHIN ( 0.001, 2.667, cmtspeed_get_divisor ( CMTSPEED_8_3 ) );
    TEST_ASSERT_DOUBLE_WITHIN ( 0.001, 1.286, cmtspeed_get_divisor ( CMTSPEED_9_7 ) );
    TEST_ASSERT_DOUBLE_WITHIN ( 0.001, 1.786, cmtspeed_get_divisor ( CMTSPEED_25_14 ) );
}


/** @brief Testuje správné textové poměry pro všechny rychlosti. */
static void test_ratio_strings ( void ) {
    TEST_ASSERT_EQUAL_STRING ( "?:?", cmtspeed_get_ratiotxt ( CMTSPEED_NONE ) );
    TEST_ASSERT_EQUAL_STRING ( "1:1", cmtspeed_get_ratiotxt ( CMTSPEED_1_1 ) );
    TEST_ASSERT_EQUAL_STRING ( "2:1", cmtspeed_get_ratiotxt ( CMTSPEED_2_1 ) );
    TEST_ASSERT_EQUAL_STRING ( "2:1 (cp/m)", cmtspeed_get_ratiotxt ( CMTSPEED_2_1_CPM ) );
    TEST_ASSERT_EQUAL_STRING ( "3:1", cmtspeed_get_ratiotxt ( CMTSPEED_3_1 ) );
    TEST_ASSERT_EQUAL_STRING ( "3:2", cmtspeed_get_ratiotxt ( CMTSPEED_3_2 ) );
    TEST_ASSERT_EQUAL_STRING ( "7:3", cmtspeed_get_ratiotxt ( CMTSPEED_7_3 ) );
    TEST_ASSERT_EQUAL_STRING ( "8:3", cmtspeed_get_ratiotxt ( CMTSPEED_8_3 ) );
    TEST_ASSERT_EQUAL_STRING ( "9:7", cmtspeed_get_ratiotxt ( CMTSPEED_9_7 ) );
    TEST_ASSERT_EQUAL_STRING ( "25:14", cmtspeed_get_ratiotxt ( CMTSPEED_25_14 ) );
}


/** @brief Testuje cmtspeed_is_valid — NONE není platná, ostatní ano. */
static void test_is_valid ( void ) {
    TEST_ASSERT_FALSE ( cmtspeed_is_valid ( CMTSPEED_NONE ) );
    TEST_ASSERT_TRUE ( cmtspeed_is_valid ( CMTSPEED_1_1 ) );
    TEST_ASSERT_TRUE ( cmtspeed_is_valid ( CMTSPEED_2_1 ) );
    TEST_ASSERT_TRUE ( cmtspeed_is_valid ( CMTSPEED_25_14 ) );
    TEST_ASSERT_FALSE ( cmtspeed_is_valid ( CMTSPEED_COUNT ) );
    TEST_ASSERT_FALSE ( cmtspeed_is_valid ( (en_CMTSPEED) -1 ) );
    TEST_ASSERT_FALSE ( cmtspeed_is_valid ( (en_CMTSPEED) 99 ) );
}


/* ========================================================================
 * UNIT testy — funkce
 * ======================================================================== */

/** @brief Testuje výpočet baudové rychlosti pro všechny kombinace. */
static void test_get_bdspeed ( void ) {
    uint16_t base = 1200;

    /* 1:1 → 1200 Bd */
    TEST_ASSERT_EQUAL_UINT16 ( 1200, cmtspeed_get_bdspeed ( CMTSPEED_1_1, base ) );

    /* 2:1 → 2400 Bd */
    TEST_ASSERT_EQUAL_UINT16 ( 2400, cmtspeed_get_bdspeed ( CMTSPEED_2_1, base ) );

    /* 2:1 CP/M → 2400 Bd */
    TEST_ASSERT_EQUAL_UINT16 ( 2400, cmtspeed_get_bdspeed ( CMTSPEED_2_1_CPM, base ) );

    /* 3:1 → 3600 Bd */
    TEST_ASSERT_EQUAL_UINT16 ( 3600, cmtspeed_get_bdspeed ( CMTSPEED_3_1, base ) );

    /* 7:3 → 2800 Bd */
    TEST_ASSERT_EQUAL_UINT16 ( 2800, cmtspeed_get_bdspeed ( CMTSPEED_7_3, base ) );

    /* 8:3 → 3200 Bd */
    TEST_ASSERT_EQUAL_UINT16 ( 3200, cmtspeed_get_bdspeed ( CMTSPEED_8_3, base ) );

    /* NONE → 0 */
    TEST_ASSERT_EQUAL_UINT16 ( 0, cmtspeed_get_bdspeed ( CMTSPEED_NONE, base ) );
}


/** @brief Testuje formátování řetězce s baudovou rychlostí. */
static void test_get_speedtxt ( void ) {
    char buf[64];

    cmtspeed_get_speedtxt ( buf, sizeof ( buf ), CMTSPEED_1_1, 1200 );
    TEST_ASSERT_EQUAL_STRING ( "1200 Bd", buf );

    cmtspeed_get_speedtxt ( buf, sizeof ( buf ), CMTSPEED_2_1, 1200 );
    TEST_ASSERT_EQUAL_STRING ( "2400 Bd", buf );

    /* CP/M varianta má suffix */
    cmtspeed_get_speedtxt ( buf, sizeof ( buf ), CMTSPEED_2_1_CPM, 1200 );
    TEST_ASSERT_EQUAL_STRING ( "2400 Bd (cp/m)", buf );

    cmtspeed_get_speedtxt ( buf, sizeof ( buf ), CMTSPEED_3_1, 1200 );
    TEST_ASSERT_EQUAL_STRING ( "3600 Bd", buf );
}


/** @brief Testuje kombinovaný řetězec "poměr - rychlost Bd". */
static void test_get_ratiospeedtxt ( void ) {
    char buf[64];

    cmtspeed_get_ratiospeedtxt ( buf, sizeof ( buf ), CMTSPEED_1_1, 1200 );
    TEST_ASSERT_EQUAL_STRING ( "1:1 - 1200 Bd", buf );

    cmtspeed_get_ratiospeedtxt ( buf, sizeof ( buf ), CMTSPEED_7_3, 1200 );
    TEST_ASSERT_EQUAL_STRING ( "7:3 - 2800 Bd", buf );
}


/** @brief Testuje bounds checking — neplatné hodnoty nesmí způsobit OOB přístup. */
static void test_bounds_invalid_speed ( void ) {
    /* Neplatný enum → bezpečný fallback */
    TEST_ASSERT_EQUAL_DOUBLE ( 0.0, cmtspeed_get_divisor ( CMTSPEED_COUNT ) );
    TEST_ASSERT_EQUAL_DOUBLE ( 0.0, cmtspeed_get_divisor ( (en_CMTSPEED) -1 ) );
    TEST_ASSERT_EQUAL_UINT16 ( 0, cmtspeed_get_bdspeed ( CMTSPEED_COUNT, 1200 ) );
    TEST_ASSERT_EQUAL_STRING ( "?:?", cmtspeed_get_ratiotxt ( CMTSPEED_COUNT ) );

    char buf[64];
    cmtspeed_get_speedtxt ( buf, sizeof ( buf ), CMTSPEED_COUNT, 1200 );
    TEST_ASSERT_EQUAL_STRING ( "? Bd", buf );

    cmtspeed_get_ratiospeedtxt ( buf, sizeof ( buf ), CMTSPEED_COUNT, 1200 );
    TEST_ASSERT_EQUAL_STRING ( "?:? - ? Bd", buf );
}


/**
 * @brief Testuje CMTSPEED_CUSTOM — vlastní délky pulzů nejsou poměr.
 *
 * Hodnota se přidala na konec enumu, čísla dosavadních rychlostí (MCP, INI)
 * se tedy nesmí změnit. Funkce pracující s poměrem ji musí odmítnout.
 */
static void test_custom ( void ) {
    TEST_ASSERT_EQUAL_INT ( 9, CMTSPEED_25_14 );
    TEST_ASSERT_EQUAL_INT ( 10, CMTSPEED_CUSTOM );
    TEST_ASSERT_FALSE ( cmtspeed_is_valid ( CMTSPEED_CUSTOM ) );
    TEST_ASSERT_TRUE ( cmtspeed_is_custom ( CMTSPEED_CUSTOM ) );
    TEST_ASSERT_FALSE ( cmtspeed_is_custom ( CMTSPEED_1_1 ) );
    TEST_ASSERT_FALSE ( cmtspeed_is_custom ( CMTSPEED_NONE ) );
    TEST_ASSERT_EQUAL_DOUBLE ( 0.0, cmtspeed_get_divisor ( CMTSPEED_CUSTOM ) );
    TEST_ASSERT_EQUAL_UINT16 ( 0, cmtspeed_get_bdspeed ( CMTSPEED_CUSTOM, 1200 ) );
    TEST_ASSERT_EQUAL_STRING ( "custom", cmtspeed_get_ratiotxt ( CMTSPEED_CUSTOM ) );

    char buf[64];
    cmtspeed_get_speedtxt ( buf, sizeof ( buf ), CMTSPEED_CUSTOM, 1200 );
    TEST_ASSERT_EQUAL_STRING ( "? Bd", buf );
    cmtspeed_get_ratiospeedtxt ( buf, sizeof ( buf ), CMTSPEED_CUSTOM, 1200 );
    TEST_ASSERT_EQUAL_STRING ( "custom - ? Bd", buf );
}


/* ========================================================================
 * main
 * ======================================================================== */

int main ( void ) {

    UNITY_BEGIN ();

    /* Smoke testy */
    RUN_TEST ( test_divisor_none_is_zero );
    RUN_TEST ( test_divisor_1_1 );
    RUN_TEST ( test_divisor_values );
    RUN_TEST ( test_ratio_strings );
    RUN_TEST ( test_is_valid );

    /* Unit testy */
    RUN_TEST ( test_get_bdspeed );
    RUN_TEST ( test_get_speedtxt );
    RUN_TEST ( test_get_ratiospeedtxt );
    RUN_TEST ( test_bounds_invalid_speed );
    RUN_TEST ( test_custom );

    return UNITY_END ();
}
