/**
 * @file test_screenshot_series.c
 * @brief Test série screenshotů (src/emulator/screenshot/screenshot_series.c).
 *
 * Konce emulovaných snímků se simulují přímým voláním
 * screenshot_series_on_screen_done() nad g_framebuffer.pixels s řízeným
 * obsahem. Ověřuje se:
 *  - jen změněné snímky: A A B B B A -> frame 0, 2, 5 (3 soubory, 3 nezměněné),
 *  - všechny snímky (only_changed = false),
 *  - limit `frames` ukončí sérii,
 *  - změna mimo oblast série (border při area = canvas) se nepočítá,
 *  - index.csv má hlavičku a řádek na každý soubor.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "gdg/video.h"
#include "hw-generic/gdg/framebuffer.h"
#include "emulator/screenshot/screenshot_series.h"

#define FB_BYTES ((size_t)VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT)

static char *s_dir = NULL;

void setUp(void)
{
    GError *e = NULL;
    s_dir = g_dir_make_tmp("mztest-series-XXXXXX", &e);
    TEST_ASSERT_NOT_NULL(s_dir);
}

/** @brief Smaže adresář série (jen soubory, bez podadresářů). */
void tearDown(void)
{
    GDir *d = g_dir_open(s_dir, 0, NULL);
    if (d) {
        const gchar *n;
        while ((n = g_dir_read_name(d)) != NULL) {
            char *p = g_build_filename(s_dir, n, NULL);
            g_remove(p);
            g_free(p);
        }
        g_dir_close(d);
    }
    g_rmdir(s_dir);
    g_free(s_dir);
}

/** @brief Vyplní framebuffer: border index `border`, plátno index `canvas`. */
static void fill(uint8_t border, uint8_t canvas)
{
    memset(g_framebuffer.pixels, border, FB_BYTES);
    for (unsigned y = 0; y < VIDEO_CANVAS_HEIGHT; y++) {
        memset(g_framebuffer.pixels + (size_t)(VIDEO_BORDER_TOP_HEIGHT + y) * VIDEO_DISPLAY_WIDTH + VIDEO_BORDER_LEFT_WIDTH,
               canvas, VIDEO_CANVAS_WIDTH);
    }
}

/** @brief Počká, až writer dopíše (max 5 s), a vrátí stav. */
static void wait_done(st_SCREENSHOT_SERIES_STATUS *st)
{
    for (int i = 0; i < 500; i++) {
        screenshot_series_get_status(st);
        if (!st->active && !st->writing) return;
        g_usleep(10000);
    }
    TEST_FAIL_MESSAGE("frame series writer did not finish");
}

/** @brief Ověří existenci souboru snímku. */
static bool frame_exists(unsigned n)
{
    char name[32];
    g_snprintf(name, sizeof(name), "frame_%07u.png", n);
    char *p = g_build_filename(s_dir, name, NULL);
    bool ok = g_file_test(p, G_FILE_TEST_EXISTS);
    g_free(p);
    return ok;
}

/** @brief Spustí sérii v s_dir s danými volbami. */
static void start(bool only_changed, uint64_t frames, en_SCREENSHOT_AREA area)
{
    st_SCREENSHOT_SERIES_PARAMS p;
    screenshot_series_params_init(&p);
    g_strlcpy(p.dir, s_dir, sizeof(p.dir));
    p.only_changed = only_changed;
    p.frames = frames;
    p.opts.area = area;
    char err[256] = "";
    TEST_ASSERT_TRUE_MESSAGE(screenshot_series_start(&p, err, sizeof(err)), err);
}


/** Jen změněné snímky: A A B B B A -> soubory 0, 2, 5. */
void test_only_changed_frames(void)
{
    start(true, 0, SCREENSHOT_AREA_FULL);
    const uint8_t seq[] = { 1, 1, 2, 2, 2, 1 };
    for (size_t i = 0; i < sizeof(seq); i++) {
        fill(0, seq[i]);
        screenshot_series_on_screen_done();
    }
    TEST_ASSERT_TRUE(screenshot_series_stop());
    st_SCREENSHOT_SERIES_STATUS st;
    wait_done(&st);
    TEST_ASSERT_EQUAL_UINT64(6, st.frames_seen);
    TEST_ASSERT_EQUAL_UINT64(3, st.files);
    TEST_ASSERT_EQUAL_UINT64(3, st.unchanged);
    TEST_ASSERT_TRUE(frame_exists(0));
    TEST_ASSERT_FALSE(frame_exists(1));
    TEST_ASSERT_TRUE(frame_exists(2));
    TEST_ASSERT_TRUE(frame_exists(5));
    TEST_ASSERT_EQUAL_STRING("Stopped", st.stop_reason);

    char *csv_path = g_build_filename(s_dir, "index.csv", NULL);
    char *csv = NULL;
    TEST_ASSERT_TRUE(g_file_get_contents(csv_path, &csv, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING_LEN("frame,file,gdg_screen\n", csv, 22);
    TEST_ASSERT_NOT_NULL(strstr(csv, "\n2,frame_0000002.png,"));
    TEST_ASSERT_NOT_NULL(strstr(csv, "\n5,frame_0000005.png,"));
    g_free(csv);
    g_free(csv_path);
}

/** Všechny snímky: 4 stejné -> 4 soubory. */
void test_all_frames(void)
{
    start(false, 0, SCREENSHOT_AREA_FULL);
    fill(0, 3);
    for (int i = 0; i < 4; i++) screenshot_series_on_screen_done();
    screenshot_series_stop();
    st_SCREENSHOT_SERIES_STATUS st;
    wait_done(&st);
    TEST_ASSERT_EQUAL_UINT64(4, st.files);
    TEST_ASSERT_EQUAL_UINT64(0, st.unchanged);
}

/** Limit `frames` ukončí sérii; další konec snímku už nic nedělá. */
void test_frame_limit(void)
{
    start(false, 3, SCREENSHOT_AREA_FULL);
    fill(0, 4);
    for (int i = 0; i < 5; i++) screenshot_series_on_screen_done();
    st_SCREENSHOT_SERIES_STATUS st;
    wait_done(&st);
    TEST_ASSERT_EQUAL_UINT64(3, st.frames_seen);
    TEST_ASSERT_EQUAL_UINT64(3, st.files);
    TEST_ASSERT_EQUAL_STRING("Frame count reached", st.stop_reason);
    TEST_ASSERT_FALSE(screenshot_series_stop());
}

/** Oblast canvas: změna jen borderu se nepočítá jako změna. */
void test_change_outside_area_ignored(void)
{
    start(true, 0, SCREENSHOT_AREA_CANVAS);
    fill(5, 6);
    screenshot_series_on_screen_done();
    fill(7, 6);
    screenshot_series_on_screen_done();
    fill(7, 8);
    screenshot_series_on_screen_done();
    screenshot_series_stop();
    st_SCREENSHOT_SERIES_STATUS st;
    wait_done(&st);
    TEST_ASSERT_EQUAL_UINT64(2, st.files);
    TEST_ASSERT_TRUE(frame_exists(0));
    TEST_ASSERT_FALSE(frame_exists(1));
    TEST_ASSERT_TRUE(frame_exists(2));
}

/** Druhý start během běžící série se odmítne. */
void test_second_start_rejected(void)
{
    start(true, 0, SCREENSHOT_AREA_FULL);
    st_SCREENSHOT_SERIES_PARAMS p;
    screenshot_series_params_init(&p);
    g_strlcpy(p.dir, s_dir, sizeof(p.dir));
    char err[256] = "";
    TEST_ASSERT_FALSE(screenshot_series_start(&p, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "already running"));
    screenshot_series_stop();
    st_SCREENSHOT_SERIES_STATUS st;
    wait_done(&st);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();
    UNITY_BEGIN();
    RUN_TEST(test_only_changed_frames);
    RUN_TEST(test_all_frames);
    RUN_TEST(test_frame_limit);
    RUN_TEST(test_change_outside_area_ignored);
    RUN_TEST(test_second_start_rejected);
    int result = UNITY_END();
    screenshot_series_exit();
    mztest_teardown();
    return result;
}
