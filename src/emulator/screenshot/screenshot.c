/**
 * @file   screenshot.c
 * @brief  Výřez a měřítko screenshotu z indexovaného framebufferu (čisté jádro).
 *
 * Popis režimů a geometrie viz screenshot.h. Modul je na MZARCH nezávislý
 * (geometrii dostává parametrem), takže jde testovat pro všechny platformy
 * (tests/screenshot/test_screenshot.c).
 *
 * @par Licence: GPLv3
 */

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "screenshot.h"


/** @brief Obdélník ve framebufferu [px]. */
typedef struct {
    unsigned x, y, w, h;
} st_SS_RECT;


void screenshot_opts_init(st_SCREENSHOT_OPTS *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->area = SCREENSHOT_AREA_FULL;
    opts->scale_mode = SCREENSHOT_SCALE_FRAMEBUFFER;
    opts->window_scale = 1;
}


/** @brief Zapíše anglický popis chyby do `err` (je-li zadaný). */
static void ss_error(char *err, size_t err_size, const char *msg)
{
    if (err && err_size) {
        g_strlcpy(err, msg, err_size);
    }
}


/**
 * @brief Zjistí, zda oblast obsahuje body šířky 640 (některá dvojice
 *        sousedních pixelů framebufferu se liší).
 * @pre rect.x a rect.w jsou sudé.
 */
static bool ss_rect_is_hires(const uint8_t *pixels, size_t pitch, const st_SS_RECT *r)
{
    for (unsigned y = 0; y < r->h; y++) {
        const uint8_t *row = pixels + (size_t)(r->y + y) * pitch + r->x;
        for (unsigned x = 0; x < r->w; x += 2) {
            if ((row[x] & 0x0F) != (row[x + 1] & 0x0F)) {
                return true;
            }
        }
    }
    return false;
}


bool screenshot_render_rgba(const uint8_t *pixels, size_t pitch,
                            const st_SCREENSHOT_GEOMETRY *geom,
                            const uint32_t palette[16],
                            const st_SCREENSHOT_OPTS *opts,
                            st_SCREENSHOT_IMAGE *out,
                            char *err, size_t err_size)
{
    memset(out, 0, sizeof(*out));

    /* Geometrie: plátno uvnitř framebufferu, sudé šířky a levý okraj. */
    if (geom->fb_width == 0 || geom->fb_height == 0 || pitch < geom->fb_width ||
        geom->canvas_x + geom->canvas_w > geom->fb_width ||
        geom->canvas_y + geom->canvas_h > geom->fb_height ||
        (geom->fb_width & 1) || (geom->canvas_w & 1) || (geom->canvas_x & 1)) {
        ss_error(err, err_size, "Invalid framebuffer geometry");
        return false;
    }

    st_SS_RECT r;
    if (opts->area == SCREENSHOT_AREA_CANVAS) {
        r.x = geom->canvas_x;
        r.y = geom->canvas_y;
        r.w = geom->canvas_w;
        r.h = geom->canvas_h;
    } else if (opts->area == SCREENSHOT_AREA_FULL) {
        r.x = 0;
        r.y = 0;
        r.w = geom->fb_width;
        r.h = geom->fb_height;
    } else {
        ss_error(err, err_size, "Unknown screenshot area");
        return false;
    }

    /* Výstupní mřížka: out (x, y) -> zdroj (r.x + x * num_x / den_x, r.y + y / den_y). */
    unsigned out_w, out_h;
    unsigned num_x = 1, den_x = 1, den_y = 1;
    bool hires = false;
    switch (opts->scale_mode) {
    case SCREENSHOT_SCALE_FRAMEBUFFER:
        out_w = r.w;
        out_h = r.h;
        break;
    case SCREENSHOT_SCALE_NATIVE:
        hires = ss_rect_is_hires(pixels, pitch, &r);
        if (!hires) {
            num_x = 2;
        }
        out_w = r.w / num_x;
        out_h = r.h;
        break;
    case SCREENSHOT_SCALE_WINDOW:
        if (opts->window_scale < 1 || opts->window_scale > SCREENSHOT_WINDOW_SCALE_MAX) {
            ss_error(err, err_size, "Window scale must be 1 to 4");
            return false;
        }
        if (opts->crop) {
            ss_error(err, err_size, "Crop requires scale 'framebuffer' or 'native'");
            return false;
        }
        num_x = 2;
        den_x = opts->window_scale;
        den_y = opts->window_scale;
        out_w = r.w * opts->window_scale / 2;
        out_h = r.h * opts->window_scale;
        break;
    default:
        ss_error(err, err_size, "Unknown screenshot scale");
        return false;
    }

    /* Ořez ve výstupních pixelech. */
    unsigned off_x = 0, off_y = 0;
    if (opts->crop) {
        if (opts->crop_w == 0 || opts->crop_h == 0 ||
            opts->crop_x >= out_w || opts->crop_y >= out_h ||
            opts->crop_w > out_w - opts->crop_x || opts->crop_h > out_h - opts->crop_y) {
            char msg[128];
            g_snprintf(msg, sizeof(msg), "Crop rectangle must lie within the %ux%u image", out_w, out_h);
            ss_error(err, err_size, msg);
            return false;
        }
        off_x = opts->crop_x;
        off_y = opts->crop_y;
        out_w = opts->crop_w;
        out_h = opts->crop_h;
    }

    uint8_t *rgba = (uint8_t *)g_malloc((gsize)out_w * out_h * 4);
    uint8_t *dst = rgba;
    for (unsigned y = 0; y < out_h; y++) {
        const uint8_t *row = pixels + (size_t)(r.y + (y + off_y) / den_y) * pitch + r.x;
        for (unsigned x = 0; x < out_w; x++) {
            uint32_t rgb = palette[row[(x + off_x) * num_x / den_x] & 0x0F];
            dst[0] = (uint8_t)((rgb >> 16) & 0xFF); /* R */
            dst[1] = (uint8_t)((rgb >> 8) & 0xFF);  /* G */
            dst[2] = (uint8_t)(rgb & 0xFF);         /* B */
            dst[3] = 0xFF;                          /* A */
            dst += 4;
        }
    }

    out->rgba = rgba;
    out->width = out_w;
    out->height = out_h;
    out->hires = (opts->scale_mode == SCREENSHOT_SCALE_NATIVE) && hires;
    return true;
}


void screenshot_image_free(st_SCREENSHOT_IMAGE *img)
{
    if (img) {
        g_free(img->rgba);
        memset(img, 0, sizeof(*img));
    }
}


bool screenshot_area_from_string(const char *s, en_SCREENSHOT_AREA *out)
{
    if (!s) return false;
    if (g_ascii_strcasecmp(s, "full") == 0) {
        *out = SCREENSHOT_AREA_FULL;
        return true;
    }
    if (g_ascii_strcasecmp(s, "canvas") == 0) {
        *out = SCREENSHOT_AREA_CANVAS;
        return true;
    }
    return false;
}


const char *screenshot_area_to_string(en_SCREENSHOT_AREA area)
{
    return (area == SCREENSHOT_AREA_CANVAS) ? "canvas" : "full";
}


bool screenshot_scale_from_string(const char *s, en_SCREENSHOT_SCALE_MODE *mode, unsigned *scale)
{
    if (!s) return false;
    if (g_ascii_strcasecmp(s, "framebuffer") == 0) {
        *mode = SCREENSHOT_SCALE_FRAMEBUFFER;
        *scale = 1;
        return true;
    }
    if (g_ascii_strcasecmp(s, "native") == 0) {
        *mode = SCREENSHOT_SCALE_NATIVE;
        *scale = 1;
        return true;
    }
    if (s[0] >= '1' && s[0] <= ('0' + SCREENSHOT_WINDOW_SCALE_MAX) && s[1] == '\0') {
        *mode = SCREENSHOT_SCALE_WINDOW;
        *scale = (unsigned)(s[0] - '0');
        return true;
    }
    return false;
}


const char *screenshot_scale_to_string(en_SCREENSHOT_SCALE_MODE mode, unsigned scale)
{
    static const char *const window[SCREENSHOT_WINDOW_SCALE_MAX] = { "1", "2", "3", "4" };
    switch (mode) {
    case SCREENSHOT_SCALE_FRAMEBUFFER:
        return "framebuffer";
    case SCREENSHOT_SCALE_NATIVE:
        return "native";
    case SCREENSHOT_SCALE_WINDOW:
    default:
        return (scale >= 1 && scale <= SCREENSHOT_WINDOW_SCALE_MAX) ? window[scale - 1] : "2";
    }
}
