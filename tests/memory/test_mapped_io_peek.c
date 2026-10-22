/**
 * @file test_mapped_io_peek.c
 * @brief Čtení mapovaných portů 0E000h-0E008h debuggerem bez vedlejšího efektu.
 *
 * V režimu MZ-700 jsou na 0E000h-0E008h namapované porty 8255, 8253 a status
 * GDG. Nesynchronní čtení (memory_read_byte_no_se pro Watch, Memory Browser,
 * výrazy BP a debugger; memory_read_byte pro `[addr]!`) dřív volalo
 * pio8255_read(): čtení 0E001h posunulo autotype, načetlo klávesnici
 * hostitele a označilo vkbd probe. 8253 se v této cestě nečetl vůbec
 * (konstanta). Nově se čte přes *_peek funkce čipů. Ověřuje:
 *
 *  - 0E001h s aktivním autotype: hodnota jako při čtení CPU, ukazatel
 *    autotype ani sloupec se nezmění (memory_read_byte_no_se i memory_read_byte),
 *  - 0E001h nevyhodnotí vkbd probe (seen zůstane false),
 *  - 0E005h (čítač 1, LSB/MSB): vrací bajt podle klopného obvodu, ten se
 *    nepřepne; čtení CPU ho přepne dál.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <string.h>

#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/memory/mz800_memory.h"

#if MZARCH != 800
#error "test_mapped_io_peek: testovací jádro je jen pro MZARCH=800"
#endif


/** @brief Uložený DMD (obnoví se v tearDown). */
static unsigned s_saved_dmd;
/** @brief Uložený stav 8255 (autotype, probe, sloupec). */
static st_PIO8255 s_saved_pio;
/** @brief Uložený stav čítače 1 8253. */
static struct st_CTC8253 s_saved_ctc1;
/** @brief Text pro autotype (pio8255 drží jen ukazatel). */
static char s_autotype_text[] = "AB";


/**
 * @brief Režim MZ-700 s namapovanými porty na 0E000h (mapa po resetu), bez MemExtu.
 */
void setUp(void)
{
    memext_disconnect ( );
    memory_reset ( );   /* ROM na 0E000h namapovaná (stav po resetu) */
    s_saved_dmd = g_gdg.regDMD;
    s_saved_pio = g_pio8255;
    s_saved_ctc1 = g_ctc8253[CTC_CS1];
    g_gdg.regDMD = REGISTER_DMD_FLAG_MZ700;
}


void tearDown(void)
{
    g_gdg.regDMD = s_saved_dmd;
    g_pio8255 = s_saved_pio;
    g_ctc8253[CTC_CS1] = s_saved_ctc1;
}


/**
 * @brief Nastaví autotype "AB" čekající na první znak a sloupec klávesy 'A'.
 * @return Sloupec klávesy 'A'.
 */
static int arm_autotype ( void )
{
    uint8_t ret;
    bool shift;
    int col = pio8255_autotype_get_matrix ( 'A', &ret, &shift );
    TEST_ASSERT_TRUE_MESSAGE ( col >= 0, "autotype table maps 'A'" );
    g_pio8255.vkbd_autotype = s_autotype_text;
    g_pio8255.vkbd_autotype_col = -1;
    g_pio8255.signal_PA_keybord_column = (unsigned) col;
    return col;
}


/* 0E001h s autotype: no_se i memory_read_byte autotype neposunou. */
void test_mapped_e001_autotype_not_advanced(void)
{
    TEST_ASSERT_TRUE ( MEMORY_MZ800_MAP_TEST_ROM_E000 );
    arm_autotype ( );

    uint8_t v_no_se = memory_read_byte_no_se ( 0xE001 );
    uint8_t v_dbg = memory_read_byte ( 0xE001 );
    TEST_ASSERT_EQUAL_PTR ( s_autotype_text, g_pio8255.vkbd_autotype );
    TEST_ASSERT_EQUAL_INT ( -1, g_pio8255.vkbd_autotype_col );

    /* Čtení CPU teď vrátí totéž a autotype posune. */
    uint8_t v_cpu = pio8255_read ( 1 );
    TEST_ASSERT_EQUAL_HEX8 ( v_cpu, v_no_se );
    TEST_ASSERT_EQUAL_HEX8 ( v_cpu, v_dbg );
    TEST_ASSERT_TRUE ( g_pio8255.vkbd_autotype != s_autotype_text );
    TEST_ASSERT_TRUE ( g_pio8255.vkbd_autotype_col >= 0 );
}


/* 0E001h nevyhodnotí vkbd probe (readback send_keys). */
void test_mapped_e001_probe_not_marked(void)
{
    g_pio8255.vkbd_autotype = NULL;
    g_pio8255.signal_PA_keybord_column = 3;
    g_pio8255.vkbd_probe.active = true;
    g_pio8255.vkbd_probe.col = 3;
    g_pio8255.vkbd_probe.seen = false;

    (void) memory_read_byte_no_se ( 0xE001 );
    TEST_ASSERT_FALSE ( g_pio8255.vkbd_probe.seen );

    (void) pio8255_read ( 1 );
    TEST_ASSERT_TRUE ( g_pio8255.vkbd_probe.seen );
}


/* 0E005h: bajt podle klopného obvodu LSB/MSB, obvod se nepřepne. */
void test_mapped_e005_ctc_flipflop_kept(void)
{
    g_ctc8253[CTC_CS1].rlf = CTC_RLF_LSBMSB;
    g_ctc8253[CTC_CS1].latch_op = 0;
    g_ctc8253[CTC_CS1].rl_byte = 0;
    g_ctc8253[CTC_CS1].value = 0x1234;

    TEST_ASSERT_EQUAL_HEX8 ( 0x34, memory_read_byte_no_se ( 0xE005 ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x34, memory_read_byte_no_se ( 0xE005 ) );
    TEST_ASSERT_EQUAL_HEX8 ( 0x34, memory_read_byte ( 0xE005 ) );
    TEST_ASSERT_EQUAL_UINT ( 0, g_ctc8253[CTC_CS1].rl_byte );

    TEST_ASSERT_EQUAL_HEX8 ( 0x34, ctc8253_read_byte ( CTC_CS1 ) );
    TEST_ASSERT_EQUAL_UINT ( 1, g_ctc8253[CTC_CS1].rl_byte );
    TEST_ASSERT_EQUAL_HEX8 ( 0x12, memory_read_byte_no_se ( 0xE005 ) );
    TEST_ASSERT_EQUAL_UINT ( 1, g_ctc8253[CTC_CS1].rl_byte );
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_mapped_e001_autotype_not_advanced);
    RUN_TEST(test_mapped_e001_probe_not_marked);
    RUN_TEST(test_mapped_e005_ctc_flipflop_kept);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
