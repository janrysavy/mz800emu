/* GPLv3. Register-write causality in the real MZ-800 raster renderer.
 * Direct component state setup, not guest execution or physical timing proof.
 * End-to-end native MZF reproduction is described in docs/en/gdg-scroll-causality.md.
 */
#include "mztest.h"
#include <string.h>
#include "memory/memory.h"
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/gdg/mz800_hwscroll.h"
#include "mzarch/mz800/gdg/mz800_framebuffer.h"

void setUp(void)
{
    framebuffer_init();
    g_gdg.regDMD = 2;
    g_gdg.regPALGRP = 0;
    g_gdg.regPAL0 = 0; g_gdg.regPAL1 = 1;
    g_gdg.regPAL2 = 2; g_gdg.regPAL3 = 3;
    g_gdg.beam_row = VIDEO_BEAM_CANVAS_FIRST_ROW + 20;
    g_gdg.total_elapsed.ticks = g_gdg.beam_row * VIDEO_SCREEN_WIDTH + 500;
    g_gdg.screen_need_update_from = 0;
    g_hwscroll.regSSA = 0; g_hwscroll.regSEA = 6720;
    g_hwscroll.regSW = 6720; g_hwscroll.regSOF = 8;
    g_hwscroll.enabled = 1;
    uint8_t *planes[] = {g_memoryVRAM_I, g_memoryVRAM_II,
                        g_memoryVRAM_III, g_memoryVRAM_IV};
    for (unsigned addr = 0; addr < 8192; addr++)
        for (unsigned plane = 0; plane < 4; plane++)
            planes[plane][addr] = ((addr >> 2) & (1u << plane)) ? 255 : 0;
}

void tearDown(void) { }

static void assert_old_prefix(unsigned old_sof, unsigned old_sea)
{
    const uint8_t *row = g_framebuffer.pixels + g_gdg.beam_row * VIDEO_DISPLAY_WIDTH
                       + VIDEO_BORDER_LEFT_WIDTH;
    /* Last complete byte at least two fetches before column 500. */
    for (unsigned x = 0; x < 288; x++) {
        unsigned addr = 20 * 40 + x / 16;
        if (addr < old_sea) addr = (addr + old_sof) % old_sea;
        TEST_ASSERT_EQUAL_UINT8((addr >> 2) & 15, row[x]);
    }
}

static void check_register(unsigned reg, unsigned value, unsigned old_sof)
{
    g_hwscroll.regSOF = old_sof;
    g_hwscroll.enabled = old_sof != 0;
    const st_GDG_TIMESTAMP clock = g_gdg.total_elapsed;
    hwscroll_set_reg(reg, value);
    assert_old_prefix(old_sof, 6720);
    TEST_ASSERT_EQUAL_MEMORY(&clock, &g_gdg.total_elapsed, sizeof(clock));
}

void test_low_offset_change_enable_disable(void)
{
    const unsigned pairs[][2] = {{0,1},{8,0},{8,2},{16,1}};
    for (unsigned i = 0; i < 4; i++) {
        setUp();
        check_register(1, pairs[i][1], pairs[i][0]);
    }
}

void test_high_offset_and_band_registers(void)
{
    const unsigned regs[][2] = {{2,1},{3,100},{4,5},{5,100}};
    for (unsigned i = 0; i < 4; i++) {
        setUp();
        check_register(regs[i][0], regs[i][1], 8);
    }
}

void test_direct_setters_preserve_elapsed_pixels(void)
{
    hwscroll_set_sof(2); assert_old_prefix(8,6720);
    setUp(); hwscroll_set_ssa(320); assert_old_prefix(8,6720);
    setUp(); hwscroll_set_sea(100); assert_old_prefix(8,6720);
    setUp(); hwscroll_set_sw(100); assert_old_prefix(8,6720);
}

void test_mz700_register_write_does_not_fill_native_pixels(void)
{
    g_gdg.regDMD = 8;
    hwscroll_set_reg(1,2);
    hwscroll_set_sof(1);
    TEST_ASSERT_EQUAL_UINT(0, g_gdg.screen_need_update_from);
}

int main(int argc, char **argv)
{
    mztest_parse_args(argc,argv); mztest_init(); UNITY_BEGIN();
    RUN_TEST(test_low_offset_change_enable_disable);
    RUN_TEST(test_high_offset_and_band_registers);
    RUN_TEST(test_direct_setters_preserve_elapsed_pixels);
    RUN_TEST(test_mz700_register_write_does_not_fill_native_pixels);
    int result = UNITY_END(); mztest_teardown(); return result;
}
