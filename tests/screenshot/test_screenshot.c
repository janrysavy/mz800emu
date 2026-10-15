/**
 * @file test_screenshot.c
 * @brief Testy čistého jádra screenshotu (src/emulator/screenshot/screenshot.c).
 *
 * Framebuffer je syntetický s geometrií MZ-800 (928x288, plátno 640x200 na
 * 154,46) a MZ-700 (704x232, plátno na 32,16). Ověřuje se rozměr výstupu pro
 * všechny oblasti a měřítka, výběr zdrojových pixelů (paleta), rozpoznání
 * režimu 320/640 v měřítku native, ořez a odmítnutí neplatných voleb.
 *
 * Licence: GPLv3
 */

#include "unity.h"

#include <glib.h>
#include <string.h>

#include "emulator/screenshot/screenshot.h"

/** @brief Geometrie MZ-800 (mz800_video.h). */
static const st_SCREENSHOT_GEOMETRY s_mz800 = { 928, 288, 154, 46, 640, 200 };
/** @brief Geometrie MZ-700 / MZ-1500 (mz700_video_pal.h, mz1500_video.h). */
static const st_SCREENSHOT_GEOMETRY s_mz700 = { 704, 232, 32, 16, 640, 200 };

/** @brief Paleta: barva indexu i = 0x00iiiiii (R = G = B = i), snadno ověřitelná. */
static uint32_t s_palette[16];

/** @brief Framebuffer dost velký pro obě geometrie. */
static uint8_t s_fb[928 * 288];


void setUp(void)
{
    for (unsigned i = 0; i < 16; i++) {
        s_palette[i] = (i << 16) | (i << 8) | i;
    }
    memset(s_fb, 0, sizeof(s_fb));
}

void tearDown(void) { }


/** @brief Vyplní framebuffer: border index 1, plátno v bodech 320 (dvojice). */
static void fill_lores(const st_SCREENSHOT_GEOMETRY *g)
{
    for (unsigned y = 0; y < g->fb_height; y++) {
        for (unsigned x = 0; x < g->fb_width; x++) {
            uint8_t v = 1;
            if (y >= g->canvas_y && y < g->canvas_y + g->canvas_h &&
                x >= g->canvas_x && x < g->canvas_x + g->canvas_w) {
                unsigned px = (x - g->canvas_x) / 2;
                v = (uint8_t)(2 + ((px + (y - g->canvas_y)) % 8));
            }
            s_fb[y * g->fb_width + x] = v;
        }
    }
}

/** @brief Index pixelu výstupu (R kanál = index palety). */
static unsigned out_index(const st_SCREENSHOT_IMAGE *img, unsigned x, unsigned y)
{
    return img->rgba[((size_t)y * img->width + x) * 4];
}

/** @brief Vyrobí obrázek s danými volbami, test selže při chybě. */
static void render(const st_SCREENSHOT_GEOMETRY *g, const st_SCREENSHOT_OPTS *o, st_SCREENSHOT_IMAGE *img)
{
    char err[128] = "";
    TEST_ASSERT_TRUE_MESSAGE(screenshot_render_rgba(s_fb, g->fb_width, g, s_palette, o, img, err, sizeof(err)), err);
    TEST_ASSERT_NOT_NULL(img->rgba);
}

/** @brief Ověří rozměr výstupu pro oblast a měřítko. */
static void check_size(const st_SCREENSHOT_GEOMETRY *g, en_SCREENSHOT_AREA area,
                       const char *scale, unsigned w, unsigned h)
{
    st_SCREENSHOT_OPTS o;
    st_SCREENSHOT_IMAGE img;
    screenshot_opts_init(&o);
    o.area = area;
    TEST_ASSERT_TRUE(screenshot_scale_from_string(scale, &o.scale_mode, &o.window_scale));
    render(g, &o, &img);
    char msg[96];
    g_snprintf(msg, sizeof(msg), "area %s scale %s", screenshot_area_to_string(area), scale);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(w, img.width, msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(h, img.height, msg);
    screenshot_image_free(&img);
}


/** Rozměry MZ-800 pro všechna měřítka (plátno v bodech 320). */
void test_sizes_mz800(void)
{
    fill_lores(&s_mz800);
    check_size(&s_mz800, SCREENSHOT_AREA_FULL, "framebuffer", 928, 288);
    check_size(&s_mz800, SCREENSHOT_AREA_CANVAS, "framebuffer", 640, 200);
    check_size(&s_mz800, SCREENSHOT_AREA_FULL, "native", 464, 288);
    check_size(&s_mz800, SCREENSHOT_AREA_CANVAS, "native", 320, 200);
    check_size(&s_mz800, SCREENSHOT_AREA_FULL, "1", 464, 288);
    check_size(&s_mz800, SCREENSHOT_AREA_FULL, "2", 928, 576);
    check_size(&s_mz800, SCREENSHOT_AREA_FULL, "3", 1392, 864);
    check_size(&s_mz800, SCREENSHOT_AREA_FULL, "4", 1856, 1152);
    check_size(&s_mz800, SCREENSHOT_AREA_CANVAS, "2", 640, 400);
}

/** Rozměry MZ-700 / MZ-1500. */
void test_sizes_mz700(void)
{
    fill_lores(&s_mz700);
    check_size(&s_mz700, SCREENSHOT_AREA_FULL, "framebuffer", 704, 232);
    check_size(&s_mz700, SCREENSHOT_AREA_CANVAS, "native", 320, 200);
    check_size(&s_mz700, SCREENSHOT_AREA_FULL, "2", 704, 464);
}

/** Native plátno: bod (x, y) = barva bodu 320, výřez začíná na plátně. */
void test_native_canvas_pixels(void)
{
    fill_lores(&s_mz800);
    st_SCREENSHOT_OPTS o;
    st_SCREENSHOT_IMAGE img;
    screenshot_opts_init(&o);
    o.area = SCREENSHOT_AREA_CANVAS;
    o.scale_mode = SCREENSHOT_SCALE_NATIVE;
    render(&s_mz800, &o, &img);
    TEST_ASSERT_FALSE(img.hires);
    for (unsigned y = 0; y < 200; y += 37) {
        for (unsigned x = 0; x < 320; x += 13) {
            TEST_ASSERT_EQUAL_UINT(2 + ((x + y) % 8), out_index(&img, x, y));
        }
    }
    screenshot_image_free(&img);
}

/** Jediná odlišná dvojice v plátně přepne native na 640 bodů. */
void test_native_detects_hires(void)
{
    fill_lores(&s_mz800);
    s_fb[(46 + 100) * 928 + 154 + 301] = 15; /* liché místo dvojice v plátně */
    st_SCREENSHOT_OPTS o;
    st_SCREENSHOT_IMAGE img;
    screenshot_opts_init(&o);
    o.area = SCREENSHOT_AREA_CANVAS;
    o.scale_mode = SCREENSHOT_SCALE_NATIVE;
    render(&s_mz800, &o, &img);
    TEST_ASSERT_TRUE(img.hires);
    TEST_ASSERT_EQUAL_UINT(640, img.width);
    TEST_ASSERT_EQUAL_UINT(15, out_index(&img, 301, 100));
    screenshot_image_free(&img);
}

/** Měřítko okna 2: sloupec framebufferu 1:1, řádky zdvojené; 3: obě dimenze 1,5x a 3x. */
void test_window_scale_mapping(void)
{
    fill_lores(&s_mz800);
    st_SCREENSHOT_OPTS o;
    st_SCREENSHOT_IMAGE img;
    screenshot_opts_init(&o);
    o.scale_mode = SCREENSHOT_SCALE_WINDOW;
    o.window_scale = 2;
    render(&s_mz800, &o, &img);
    for (unsigned y = 0; y < 576; y += 31) {
        for (unsigned x = 0; x < 928; x += 17) {
            TEST_ASSERT_EQUAL_UINT(s_fb[(y / 2) * 928 + x], out_index(&img, x, y));
        }
    }
    screenshot_image_free(&img);

    o.window_scale = 3;
    render(&s_mz800, &o, &img);
    for (unsigned y = 0; y < 864; y += 29) {
        for (unsigned x = 0; x < 1392; x += 19) {
            TEST_ASSERT_EQUAL_UINT(s_fb[(y / 3) * 928 + (x * 2) / 3], out_index(&img, x, y));
        }
    }
    screenshot_image_free(&img);
}

/** Ořez native: výstup = výřez, souřadnice ve výstupních pixelech. */
void test_native_crop(void)
{
    fill_lores(&s_mz800);
    st_SCREENSHOT_OPTS o;
    st_SCREENSHOT_IMAGE img;
    screenshot_opts_init(&o);
    o.area = SCREENSHOT_AREA_CANVAS;
    o.scale_mode = SCREENSHOT_SCALE_NATIVE;
    o.crop = true;
    o.crop_x = 32;
    o.crop_y = 4;
    o.crop_w = 256;
    o.crop_h = 192;
    render(&s_mz800, &o, &img);
    TEST_ASSERT_EQUAL_UINT(256, img.width);
    TEST_ASSERT_EQUAL_UINT(192, img.height);
    TEST_ASSERT_EQUAL_UINT(2 + ((32 + 4) % 8), out_index(&img, 0, 0));
    TEST_ASSERT_EQUAL_UINT(2 + ((32 + 255 + 4 + 191) % 8), out_index(&img, 255, 191));
    screenshot_image_free(&img);
}

/** Neplatné volby: ořez mimo obraz, ořez s měřítkem okna, měřítko 5. */
void test_invalid_options(void)
{
    fill_lores(&s_mz800);
    st_SCREENSHOT_OPTS o;
    st_SCREENSHOT_IMAGE img;
    char err[128];

    screenshot_opts_init(&o);
    o.area = SCREENSHOT_AREA_CANVAS;
    o.crop = true;
    o.crop_x = 600;
    o.crop_w = 100;
    o.crop_h = 10;
    TEST_ASSERT_FALSE(screenshot_render_rgba(s_fb, 928, &s_mz800, s_palette, &o, &img, err, sizeof(err)));
    TEST_ASSERT_NULL(img.rgba);

    screenshot_opts_init(&o);
    o.scale_mode = SCREENSHOT_SCALE_WINDOW;
    o.window_scale = 2;
    o.crop = true;
    o.crop_w = 10;
    o.crop_h = 10;
    TEST_ASSERT_FALSE(screenshot_render_rgba(s_fb, 928, &s_mz800, s_palette, &o, &img, err, sizeof(err)));

    screenshot_opts_init(&o);
    o.scale_mode = SCREENSHOT_SCALE_WINDOW;
    o.window_scale = 5;
    TEST_ASSERT_FALSE(screenshot_render_rgba(s_fb, 928, &s_mz800, s_palette, &o, &img, err, sizeof(err)));
}

/** Převody názvů oblasti a měřítka. */
void test_string_conversions(void)
{
    en_SCREENSHOT_AREA a;
    en_SCREENSHOT_SCALE_MODE m;
    unsigned s;
    TEST_ASSERT_TRUE(screenshot_area_from_string("Canvas", &a));
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_AREA_CANVAS, a);
    TEST_ASSERT_FALSE(screenshot_area_from_string("border", &a));
    TEST_ASSERT_TRUE(screenshot_scale_from_string("4", &m, &s));
    TEST_ASSERT_EQUAL_INT(SCREENSHOT_SCALE_WINDOW, m);
    TEST_ASSERT_EQUAL_UINT(4, s);
    TEST_ASSERT_FALSE(screenshot_scale_from_string("5", &m, &s));
    TEST_ASSERT_FALSE(screenshot_scale_from_string("12", &m, &s));
    TEST_ASSERT_EQUAL_STRING("native", screenshot_scale_to_string(SCREENSHOT_SCALE_NATIVE, 1));
    TEST_ASSERT_EQUAL_STRING("3", screenshot_scale_to_string(SCREENSHOT_SCALE_WINDOW, 3));
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sizes_mz800);
    RUN_TEST(test_sizes_mz700);
    RUN_TEST(test_native_canvas_pixels);
    RUN_TEST(test_native_detects_hires);
    RUN_TEST(test_window_scale_mapping);
    RUN_TEST(test_native_crop);
    RUN_TEST(test_invalid_options);
    RUN_TEST(test_string_conversions);
    return UNITY_END();
}
