/*
 * test_gdg_mirror.c — smoke testy pro GDG mirror snapshot API
 *
 * Pojistka proti regresi public API:
 *   - gdg_mirror_snapshot() (arch-specific, zde MZ-800)
 *   - gdg_mirror_raster_snapshot() (arch-independent shape, per-arch impl)
 *
 * Testy ověřují:
 *   1. že volání nepadne na default state emulátoru po init,
 *   2. že snapshot fieldy jsou v rozumném rozsahu (sanity, nikoliv
 *      konkrétní očekávané hodnoty - ty jsou závislé na timing emu init),
 *   3. side-effect free invariant: dvě po sobě jdoucí volání bez jiné
 *      činnosti emu vrátí identický snapshot (= snapshot funkce sama
 *      nemění stav `g_gdg`).
 *
 * Per AddMzTest.cmake (CMAKE: mz_test_core_mz800) jsou EMU testy
 * MZARCH=800 only - test_gdg_mirror je proto MZ-800 only.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <string.h>

#include "mzarch/mz800/gdg/mz800_gdg_mirror.h"
#include "mzarch/gdg_raster_snapshot.h"
#include "mzarch/mz800/gdg/mz800_video.h"  /* VIDEO_SCREEN_HEIGHT, VIDEO_SCREEN_TICKS */

void setUp(void) { }
void tearDown(void) { }

/* ================================================================
 * SMOKE TESTY - gdg_mirror_snapshot (MZ-800)
 * ================================================================ */

/* mz800 mirror snapshot - volání nepadne a struktura je writable */
void test_gdg_mirror_snapshot_no_crash(void)
{
    st_GDG_MIRROR_MZ800 snap;
    /* preinicializovat hodnotou kterou snapshot přepíše */
    memset(&snap, 0xAA, sizeof(snap));

    gdg_mirror_snapshot(&snap);

    /* sanity: alespoň jeden field se přepsal (= 0xAA fill je pryč
     * pro typický field). regDMD je low-4-bit hodnota, takže
     * po snapshotu nesmí být 0xAA. */
    TEST_ASSERT_NOT_EQUAL(0xAA, snap.regDMD);
}

/* mz800 mirror snapshot - DMD dekódované bity jsou 0/1 (sanity rozsah) */
void test_gdg_mirror_snapshot_dmd_bits_sanity(void)
{
    st_GDG_MIRROR_MZ800 snap;
    gdg_mirror_snapshot(&snap);

    /* každý DMD bit je 0 nebo 1 (dekódováno z low-4-bit regDMD) */
    TEST_ASSERT_TRUE(snap.dmd_mz700 == 0 || snap.dmd_mz700 == 1);
    TEST_ASSERT_TRUE(snap.dmd_scrw640 == 0 || snap.dmd_scrw640 == 1);
    TEST_ASSERT_TRUE(snap.dmd_hicolor == 0 || snap.dmd_hicolor == 1);
    TEST_ASSERT_TRUE(snap.dmd_vbank == 0 || snap.dmd_vbank == 1);

    /* regDMD je low-4-bit hodnota, takže <= 0x0F */
    TEST_ASSERT_TRUE(snap.regDMD <= 0x0F);
}

/* mz800 mirror snapshot - paletové registry jsou 4-bit IGRB (<= 0x0F) */
void test_gdg_mirror_snapshot_palette_sanity(void)
{
    st_GDG_MIRROR_MZ800 snap;
    gdg_mirror_snapshot(&snap);

    TEST_ASSERT_TRUE(snap.regBOR <= 0x0F);
    TEST_ASSERT_TRUE(snap.regPAL0 <= 0x0F);
    TEST_ASSERT_TRUE(snap.regPAL1 <= 0x0F);
    TEST_ASSERT_TRUE(snap.regPAL2 <= 0x0F);
    TEST_ASSERT_TRUE(snap.regPAL3 <= 0x0F);
    /* PALGRP je 2-bit (0..3) */
    TEST_ASSERT_TRUE(snap.regPALGRP <= 3);
}

/* mz800 mirror snapshot - raster pozice v rozumném rozsahu */
void test_gdg_mirror_snapshot_raster_sanity(void)
{
    st_GDG_MIRROR_MZ800 snap;
    gdg_mirror_snapshot(&snap);

    /* beam_row je 0..VIDEO_SCREEN_HEIGHT-1 (= 311 pro MZ-800) */
    TEST_ASSERT_TRUE(snap.beam_row < VIDEO_SCREEN_HEIGHT);
    /* screen_ticks v rozsahu daného screenu */
    TEST_ASSERT_TRUE(snap.screen_ticks < VIDEO_SCREEN_TICKS);

    /* boolean signály jsou 0/1 */
    TEST_ASSERT_TRUE(snap.hbln == 0 || snap.hbln == 1);
    TEST_ASSERT_TRUE(snap.vbln == 0 || snap.vbln == 1);
    TEST_ASSERT_TRUE(snap.sts_hsync == 0 || snap.sts_hsync == 1);
    TEST_ASSERT_TRUE(snap.sts_vsync == 0 || snap.sts_vsync == 1);
    TEST_ASSERT_TRUE(snap.tempo == 0 || snap.tempo == 1);
    TEST_ASSERT_TRUE(snap.cksw == 0 || snap.cksw == 1);
}

/* mz800 mirror snapshot - vramctrl/hwscroll fieldy mají rozumný rozsah */
void test_gdg_mirror_snapshot_vramctrl_hwscroll_sanity(void)
{
    st_GDG_MIRROR_MZ800 snap;
    gdg_mirror_snapshot(&snap);

    /* WF/RF plane masky jsou 4-bit (bity 0..3) */
    TEST_ASSERT_TRUE(snap.vc_wf_plane <= 0x0F);
    TEST_ASSERT_TRUE(snap.vc_rf_plane <= 0x0F);
    TEST_ASSERT_TRUE(snap.vc_rf_search <= 0x0F);
    /* VBANK je 0 nebo 1 */
    TEST_ASSERT_TRUE(snap.vc_wfrf_vbank == 0 || snap.vc_wfrf_vbank == 1);
    /* WRITE FF modelu WAIT u VRAM - 0 nebo 1 */
    TEST_ASSERT_TRUE(snap.vc_vram_wrff_set == 0 || snap.vc_vram_wrff_set == 1);

    /* HW scroll enabled - 0 nebo 1 */
    TEST_ASSERT_TRUE(snap.hs_enabled == 0 || snap.hs_enabled == 1);
}

/* mz800 mirror snapshot - side-effect free: dvě po sobě jdoucí volání
 * vrátí identický snapshot, pokud mezi nimi neproběhla jiná emu akce */
void test_gdg_mirror_snapshot_side_effect_free(void)
{
    st_GDG_MIRROR_MZ800 snap1;
    st_GDG_MIRROR_MZ800 snap2;

    gdg_mirror_snapshot(&snap1);
    gdg_mirror_snapshot(&snap2);

    /* obě snapshoty musí být bit-identical (žádná mutace mezi voláními) */
    TEST_ASSERT_EQUAL_MEMORY(&snap1, &snap2, sizeof(snap1));
}

/* ================================================================
 * SMOKE TESTY - gdg_mirror_raster_snapshot (arch-independent shape)
 * ================================================================ */

/* raster snapshot - volání nepadne a fieldy se zapíší */
void test_gdg_raster_snapshot_no_crash(void)
{
    st_GDG_RASTER_SNAPSHOT snap;
    memset(&snap, 0xAA, sizeof(snap));

    gdg_mirror_raster_snapshot(&snap);

    /* ctc0_divider musí být přepsán na compile-time hodnotu (>0) */
    TEST_ASSERT_TRUE(snap.ctc0_divider > 0);
}

/* raster snapshot - fieldy v rozumném rozsahu */
void test_gdg_raster_snapshot_sanity(void)
{
    st_GDG_RASTER_SNAPSHOT snap;
    gdg_mirror_raster_snapshot(&snap);

    /* beam_row < VIDEO_SCREEN_HEIGHT (pro MZ-800 = 312) */
    TEST_ASSERT_TRUE(snap.beam_row < VIDEO_SCREEN_HEIGHT);
    /* screen_ticks v rámci screenu */
    TEST_ASSERT_TRUE(snap.screen_ticks < VIDEO_SCREEN_TICKS);

    /* boolean fieldy */
    TEST_ASSERT_TRUE(snap.hbln == 0 || snap.hbln == 1);
    TEST_ASSERT_TRUE(snap.vbln == 0 || snap.vbln == 1);
    TEST_ASSERT_TRUE(snap.sts_hsync == 0 || snap.sts_hsync == 1);
    TEST_ASSERT_TRUE(snap.sts_vsync == 0 || snap.sts_vsync == 1);
    TEST_ASSERT_TRUE(snap.tempo == 0 || snap.tempo == 1);

    /* tempo_divider 0..228 (per hlavička) */
    TEST_ASSERT_TRUE(snap.tempo_divider <= 228);

    /* MZ-800 má GDGCLK_CTC0_DIVIDER = 16 (compile-time konstanta) */
    TEST_ASSERT_EQUAL_UINT32(16, snap.ctc0_divider);
}

/* raster snapshot - side-effect free invariant */
void test_gdg_raster_snapshot_side_effect_free(void)
{
    st_GDG_RASTER_SNAPSHOT snap1;
    st_GDG_RASTER_SNAPSHOT snap2;

    gdg_mirror_raster_snapshot(&snap1);
    gdg_mirror_raster_snapshot(&snap2);

    TEST_ASSERT_EQUAL_MEMORY(&snap1, &snap2, sizeof(snap1));
}

/* === MAIN === */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    /* mz800 mirror snapshot */
    RUN_TEST(test_gdg_mirror_snapshot_no_crash);
    RUN_TEST(test_gdg_mirror_snapshot_dmd_bits_sanity);
    RUN_TEST(test_gdg_mirror_snapshot_palette_sanity);
    RUN_TEST(test_gdg_mirror_snapshot_raster_sanity);
    RUN_TEST(test_gdg_mirror_snapshot_vramctrl_hwscroll_sanity);
    RUN_TEST(test_gdg_mirror_snapshot_side_effect_free);

    /* arch-independent raster snapshot */
    RUN_TEST(test_gdg_raster_snapshot_no_crash);
    RUN_TEST(test_gdg_raster_snapshot_sanity);
    RUN_TEST(test_gdg_raster_snapshot_side_effect_free);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
