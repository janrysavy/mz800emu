/*
 * test_snapshot_periph_hidden.c - snapshot uloží a obnoví stav periferií,
 * který dřív vynechával (jádro MZ-800)
 *
 * Testuje round-trip přes buffer API (snapshot_save_to_buffer /
 * snapshot_load_from_buffer):
 *   - CTC0 rychlá cesta (MZ800EMU_CFG_CLK1M1_FAST): naplánovaná událost
 *     clk1m1_event.ticks a báze líného dopočtu clk1m1_last_event_total_ticks,
 *   - registry HW scrollu MZ-800 (g_hwscroll),
 *   - FDC WD279x: přechodné RNF na konci stopy (pending_rnf_end) a seznam
 *     ID sektorů READ TRACK (rt_sinfo).
 *
 * Dřív se tato pole po načtení vrátila jako hodnoty stroje, který se
 * přepisoval. Latch WAIT VRAM v buildu MZ-700 ověřuje e2e test
 * tests/mcp/test_snapshot_z80_hidden.py (C testy běží jen nad MZ-800).
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <string.h>

#include "emulator/snapshot/snapshot.h"
#include "emulator/emulator.h"
#include "mzarch/mzarch.h"
#include "hw-generic/ctc8253/ctc8253.h"
#include "hw-generic/fdc/fdc.h"
#include "mzarch/mz800/gdg/mz800_hwscroll.h"

void setUp(void) { }
void tearDown(void)
{
    g_emulator.paused = false;
}

/** @brief Uloží snapshot do bufferu (v pauze), vrátí ho; volající uvolní g_free(). */
static uint8_t *save(size_t *size)
{
    g_emulator.paused = true;
    uint8_t *buf = NULL;
    en_SNAPSHOT_RESULT res = snapshot_save_to_buffer("periph hidden", &buf, size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res, snapshot_result_to_string(res));
    return buf;
}

/** @brief Načte snapshot z bufferu a buffer uvolní. */
static void load(uint8_t *buf, size_t size)
{
    en_SNAPSHOT_RESULT res = snapshot_load_from_buffer(buf, size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res, snapshot_result_to_string(res));
    g_free(buf);
}

#ifdef MZ800EMU_CFG_CLK1M1_FAST
/* Událost a báze CTC0 se po načtení obnoví (dřív zůstaly ze stroje). */
void test_ctc0_fast_path_roundtrip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_CTC8253 *ctc0 = &g_ctc8253[CTC_CS0];
    st_CTC8253 backup = *ctc0;

    ctc0->clk1m1_event.ticks = 12345;
    ctc0->clk1m1_last_event_total_ticks = 0x00ABCDEFu;
    size_t size = 0;
    uint8_t *buf = save(&size);

    ctc0->clk1m1_event.ticks = 777;
    ctc0->clk1m1_last_event_total_ticks = 42;
    load(buf, size);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(12345, ctc0->clk1m1_event.ticks, "clk1m1_event.ticks");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0x00ABCDEFu, ctc0->clk1m1_last_event_total_ticks,
                                    "clk1m1_last_event_total_ticks");
    *ctc0 = backup;
}
#endif

/* Registry HW scrollu se po načtení obnoví (dřív handler neexistoval). */
void test_hwscroll_roundtrip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_HWSCROLL backup = g_hwscroll;

    g_hwscroll.regSSA = 0x0140;
    g_hwscroll.regSEA = 0x1E00;
    g_hwscroll.regSW = 0x1CC0;
    g_hwscroll.regSOF = 0x0280;
    g_hwscroll.enabled = 1;
    size_t size = 0;
    uint8_t *buf = save(&size);

    hwscroll_reset();
    load(buf, size);

    TEST_ASSERT_EQUAL_HEX_MESSAGE(0x0140, g_hwscroll.regSSA, "regSSA");
    TEST_ASSERT_EQUAL_HEX_MESSAGE(0x1E00, g_hwscroll.regSEA, "regSEA");
    TEST_ASSERT_EQUAL_HEX_MESSAGE(0x1CC0, g_hwscroll.regSW, "regSW");
    TEST_ASSERT_EQUAL_HEX_MESSAGE(0x0280, g_hwscroll.regSOF, "regSOF");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, g_hwscroll.enabled, "enabled");
    g_hwscroll = backup;
}

/* Přechodné RNF a ID sektorů READ TRACK se po načtení obnoví. */
void test_fdc_hidden_roundtrip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_WD279X *chip = &g_fdc[FDC0].wd279x;
    uint8_t sinfo[sizeof(chip->rt_sinfo)];
    for (size_t i = 0; i < sizeof(sinfo); i++) sinfo[i] = (uint8_t)(0xC1 + i * 7);

    uint8_t backup_rnf = chip->pending_rnf_end;
    uint8_t backup_sinfo[sizeof(chip->rt_sinfo)];
    memcpy(backup_sinfo, chip->rt_sinfo, sizeof(backup_sinfo));

    chip->pending_rnf_end = 1;
    memcpy(chip->rt_sinfo, sinfo, sizeof(sinfo));
    size_t size = 0;
    uint8_t *buf = save(&size);

    chip->pending_rnf_end = 0;
    memset(chip->rt_sinfo, 0x55, sizeof(chip->rt_sinfo));
    load(buf, size);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, chip->pending_rnf_end, "pending_rnf_end");
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(sinfo, chip->rt_sinfo, sizeof(sinfo), "rt_sinfo");

    chip->pending_rnf_end = backup_rnf;
    memcpy(chip->rt_sinfo, backup_sinfo, sizeof(backup_sinfo));
}

/* === MAIN === */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

#ifdef MZ800EMU_CFG_CLK1M1_FAST
    RUN_TEST(test_ctc0_fast_path_roundtrip);
#endif
    RUN_TEST(test_hwscroll_roundtrip);
    RUN_TEST(test_fdc_hidden_roundtrip);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
