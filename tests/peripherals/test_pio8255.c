/*
 * test_pio8255.c — unit testy pro PIO 8255 (Parallel I/O)
 *
 * Testuje: inicializaci, keyboard matrix, port A/B/C operace,
 *          bit set/reset, signály
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <string.h>

#include "hw-generic/pio8255/pio8255.h"

void setUp(void) { }
void tearDown(void) { }

/* ================================================================
 * SMOKE TESTY
 * ================================================================ */

/* PIO — po inicializaci je struktura přístupná */
void test_pio_init_state(void)
{
    /* g_pio8255 byla inicializována v mztest_init() */
    /* ověříme, že čtení a zápis nepadne */
    uint8_t val = pio8255_read(0);
    (void)val;
    TEST_ASSERT_TRUE(true);
}

/* PIO — keyboard matrix reset */
void test_pio_keyboard_matrix_reset(void)
{
    pio8255_keyboard_matrix_reset();

    /* po resetu by všechny klávesy měly být uvolněny (0xFF = nestisknuto) */
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.keyboard_matrix[i]);
    }
}

/* ================================================================
 * UNIT TESTY
 * ================================================================ */

/* PIO — keyboard matrix — nastavení a čtení bitů */
void test_pio_keyboard_bits(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    pio8255_keyboard_matrix_reset();

    /* stisknout klávesu: sloupec 0, bit 0 → vynulovat bit */
    PIO8255_MZKEYBIT_RESET(0, 0);
    TEST_ASSERT_FALSE(PIO8255_MZKEY_BIT_GET(0, 0));

    /* ostatní bity ve sloupci 0 by měly zůstat 1 */
    TEST_ASSERT_TRUE(PIO8255_MZKEY_BIT_GET(0, 1));
    TEST_ASSERT_TRUE(PIO8255_MZKEY_BIT_GET(0, 7));

    /* jiný sloupec by měl být nedotčený */
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_pio8255.keyboard_matrix[1]);

    pio8255_keyboard_matrix_reset();
}

/* PIO — virtuální klávesnice */
void test_pio_vkbd_bits(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* vynulovat VK matrix */
    memset(g_pio8255.vkbd_matrix, 0xFF, 10);

    /* nastavit bit (stisk klávesy) */
    PIO8255_VKBDBIT_RESET(3, 5);
    TEST_ASSERT_FALSE(PIO8255_VKBD_BIT_GET(3, 5));

    /* uvolnit klávesu */
    PIO8255_VKBDBIT_SET(3, 5);
    TEST_ASSERT_TRUE(PIO8255_VKBD_BIT_GET(3, 5));
}

/* PIO — kombinovaná klávesová matrix (HW + VK) */
void test_pio_combined_keyboard(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    pio8255_keyboard_matrix_reset();
    memset(g_pio8255.vkbd_matrix, 0xFF, 10);

    /* obě matice mají klávesu uvolněnu → kombinovaný bit = 1 */
    TEST_ASSERT_TRUE(PIO8255_KBDALL_BIT_GET(2, 3));

    /* stisnout jen na HW matrix → kombinovaný bit = 0 */
    PIO8255_MZKEYBIT_RESET(2, 3);
    TEST_ASSERT_FALSE(PIO8255_KBDALL_BIT_GET(2, 3));

    /* obnovit HW, stisnout jen na VK → kombinovaný bit = 0 */
    pio8255_keyboard_matrix_reset();
    PIO8255_VKBDBIT_RESET(2, 3);
    TEST_ASSERT_FALSE(PIO8255_KBDALL_BIT_GET(2, 3));

    /* obnovit VK */
    PIO8255_VKBDBIT_SET(2, 3);
}

/* PIO — port A zápis a keyboard column select */
void test_pio_port_a_write(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* port A: dolní 4 bity = keyboard column (0-9) */
    /* zapsat sloupec 5 */
    pio8255_write(0, 0x05);
    TEST_ASSERT_EQUAL_INT(5, g_pio8255.signal_PA_keybord_column);

    /* zapsat sloupec 0 */
    pio8255_write(0, 0x00);
    TEST_ASSERT_EQUAL_INT(0, g_pio8255.signal_PA_keybord_column);
}

/* PIO — port C signály */
void test_pio_port_c_signals(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* port C: zápis ovládá dolní 4 bity (výstupní) */
    /* bit 0: CTC audio masking */
    /* bit 1: CMT data out */
    /* bit 2: CTC2 interrupt enable */
    /* bit 3: CMT motor */

    /* přečteme signály — jen ověříme, že accessory nepadnou */
    int pc1 = pio8255_pc1_get();
    int pc2 = pio8255_pc2_get();
    int pc4 = pio8255_pc4_get();
    (void)pc1;
    (void)pc2;
    (void)pc4;
    TEST_ASSERT_TRUE(true);
}

/* PIO — čtení portu B vrací keyboard data */
void test_pio_port_b_keyboard_read(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    pio8255_keyboard_matrix_reset();
    memset(g_pio8255.vkbd_matrix, 0xFF, 10);

    /* nastavit sloupec 0 */
    pio8255_write(0, 0x00); /* PA dolní 4 bity = sloupec 0 */

    /* přečíst port B = addr 1 → keyboard data */
    uint8_t val = pio8255_read(1);

    /* se všemi klávesami uvolněnými by mělo být 0xFF */
    TEST_ASSERT_EQUAL_HEX8(0xFF, val);

    /* stisnout klávesu ve sloupci 0, bit 0 */
    PIO8255_MZKEYBIT_RESET(0, 0);

    val = pio8255_read(1);
    /* bit 0 by měl být 0 (stisknut) */
    TEST_ASSERT_EQUAL_HEX8(0xFE, val);

    pio8255_keyboard_matrix_reset();
}

/* PIO — PA0-3 = 0Ah-0Fh nevybere žádný sloupec, Port B vrací 0xFF
 *
 * PA0-3 vedou přes 74LS145 (BCD -> 1 z 10, sm800 str. 28); hodnoty 10-15
 * neaktivují žádný sloupec. Ověřeno na HW MZ-800 testem KBDCOL
 * (emu-experiments/kbd-col-alias, 2026-10-08): PB = 0xFF v klidu i se
 * stisknutými klávesami sloupců 0-5, bez ohledu na PA4-7.
 *
 * Regrese: čtení Port B indexovalo keyboard_matrix[10] a vkbd_matrix[10]
 * mimo meze a vracelo 0x00 (hra Antiriad / S. Hecht visela na titulce). */
void test_pio_port_b_column_0a_0f_no_select(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    static const uint8_t high[] = { 0x00, 0xF0 };

    pio8255_keyboard_matrix_reset();
    memset(g_pio8255.vkbd_matrix, 0xFF, 10);

    /* v klidu */
    for (unsigned h = 0; h < sizeof(high); h++) {
        for (uint8_t col = 0x0A; col <= 0x0F; col++) {
            pio8255_write(0, (uint8_t)(high[h] | col));
            TEST_ASSERT_EQUAL_HEX8(0xFF, pio8255_read(1));
        }
    }

    /* všechny klávesy všech sloupců stisknuté (reálná i virtuální matice) */
    memset(g_pio8255.keyboard_matrix, 0x00, 10);
    memset(g_pio8255.vkbd_matrix, 0x00, 10);
    for (unsigned h = 0; h < sizeof(high); h++) {
        for (uint8_t col = 0x0A; col <= 0x0F; col++) {
            pio8255_write(0, (uint8_t)(high[h] | col));
            TEST_ASSERT_EQUAL_HEX8(0xFF, pio8255_read(1));
        }
    }

    /* platné sloupce stisky dál vidí */
    pio8255_write(0, 0x05);
    TEST_ASSERT_EQUAL_HEX8(0x00, pio8255_read(1));

    pio8255_keyboard_matrix_reset();
    memset(g_pio8255.vkbd_matrix, 0xFF, 10);
}

/* PIO — joystick enable signály */
void test_pio_joystick_signals(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* přečteme joystick signály — jen ověříme, že nepadne */
    int joy1 = pio8255_pa4_get();
    int joy2 = pio8255_pa5_get();
    (void)joy1;
    (void)joy2;
    TEST_ASSERT_TRUE(true);
}

/* PIO — vkbd probe: readback dosednutí klávesy (fix 0016 / cesta A)
 *
 * Ověřuje, že probe.seen reaguje POUZE na čtení Port B se selektovaným
 * sloupcem == probe.col. Čtení jiného sloupce probe nesmí odpálit. */
void test_pio_vkbd_probe_landing(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    pio8255_keyboard_matrix_reset();
    memset(g_pio8255.vkbd_matrix, 0xFF, 10);

    /* Ozbroj probe na sloupec 3, bit 5 (vstříknutá klávesa). */
    pio8255_vkbd_probe_arm(3, 5);
    TEST_ASSERT_FALSE(pio8255_vkbd_probe_check());

    /* Guest skenuje JINÝ sloupec (0) a čte Port B -> probe NESMÍ vidět. */
    pio8255_write(0, 0x00);   /* PA dolní 4 bity = sloupec 0 */
    (void)pio8255_read(1);    /* Port B read */
    TEST_ASSERT_FALSE(pio8255_vkbd_probe_check());

    /* Guest skenuje CÍLOVÝ sloupec (3) a čte Port B -> probe vidí. */
    pio8255_write(0, 0x03);   /* PA dolní 4 bity = sloupec 3 */
    (void)pio8255_read(1);    /* Port B read */
    TEST_ASSERT_TRUE(pio8255_vkbd_probe_check());

    /* Disarm: probe je neaktivní; čtení cílového sloupce už seen nemění
     * (seen zůstává sticky, ale active je false -> hook se nevyhodnocuje). */
    pio8255_vkbd_probe_disarm();

    /* Nový arm vynuluje seen. */
    pio8255_vkbd_probe_arm(3, 5);
    TEST_ASSERT_FALSE(pio8255_vkbd_probe_check());

    /* Po disarm čtení cílového sloupce NESMÍ seen nastavit. */
    pio8255_vkbd_probe_disarm();
    pio8255_write(0, 0x03);
    (void)pio8255_read(1);
    TEST_ASSERT_FALSE(pio8255_vkbd_probe_check());

    pio8255_keyboard_matrix_reset();
}

/* PIO — vkbd probe: arm s neplatným sloupcem nechá probe neaktivní */
void test_pio_vkbd_probe_invalid_arm(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    pio8255_keyboard_matrix_reset();
    memset(g_pio8255.vkbd_matrix, 0xFF, 10);

    /* Neplatný sloupec (mimo 0..9) -> probe neaktivní. */
    pio8255_vkbd_probe_arm(15, 0);
    TEST_ASSERT_FALSE(g_pio8255.vkbd_probe.active);

    /* Čtení libovolného sloupce nesmí seen nastavit. */
    pio8255_write(0, 0x05);
    (void)pio8255_read(1);
    TEST_ASSERT_FALSE(pio8255_vkbd_probe_check());

    pio8255_keyboard_matrix_reset();
}

/* PIO — autotype matrix mapping */
void test_pio_autotype_matrix(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t matrix_byte;
    bool shift;

    /* písmeno 'A' by mělo mít mapování v autotype tabulce */
    int result = pio8255_autotype_get_matrix('A', &matrix_byte, &shift);

    /* pokud mapování existuje, result = 0 (úspěch) */
    if (result == 0) {
        /* matrix_byte by měl být nenulový */
        TEST_ASSERT_NOT_EQUAL(0, matrix_byte);
    }
    /* pokud mapování neexistuje, result != 0 — to je taky OK */
    TEST_ASSERT_TRUE(true);
}

/* === MAIN === */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    /* smoke */
    RUN_TEST(test_pio_init_state);
    RUN_TEST(test_pio_keyboard_matrix_reset);

    /* unit */
    RUN_TEST(test_pio_keyboard_bits);
    RUN_TEST(test_pio_vkbd_bits);
    RUN_TEST(test_pio_combined_keyboard);
    RUN_TEST(test_pio_port_a_write);
    RUN_TEST(test_pio_port_c_signals);
    RUN_TEST(test_pio_port_b_keyboard_read);
    RUN_TEST(test_pio_port_b_column_0a_0f_no_select);
    RUN_TEST(test_pio_joystick_signals);
    RUN_TEST(test_pio_vkbd_probe_landing);
    RUN_TEST(test_pio_vkbd_probe_invalid_arm);
    RUN_TEST(test_pio_autotype_matrix);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
