/**
 * @file   screenshot_capture.c
 * @brief  Screenshot aktuální platformy: geometrie framebufferu a PNG.
 *
 * Viz screenshot_capture.h. Soubor se překládá pro každou platformu
 * (MZARCH), geometrii bere z VIDEO_* maker.
 *
 * @par Licence: GPLv3
 */

#include <glib.h>
#include <string.h>

#include "mzarch/mzarch_config.h"
#include "gdg/video.h"
#include "display.h"
#include "debugger/png_encode.h"

#include "screenshot_capture.h"


void screenshot_geometry_current(st_SCREENSHOT_GEOMETRY *g)
{
    g->fb_width  = VIDEO_DISPLAY_WIDTH;
    g->fb_height = VIDEO_DISPLAY_HEIGHT;
    g->canvas_x  = VIDEO_BORDER_LEFT_WIDTH;
    g->canvas_y  = VIDEO_BORDER_TOP_HEIGHT;
    g->canvas_w  = VIDEO_CANVAS_WIDTH;
    g->canvas_h  = VIDEO_CANVAS_HEIGHT;
}


bool screenshot_encode_png(const uint8_t *pixels, size_t pitch,
                           const st_SCREENSHOT_OPTS *opts,
                           st_SCREENSHOT_PNG *out,
                           char *err, size_t err_size)
{
    memset(out, 0, sizeof(*out));

    uint32_t *colormap = display_get_default_color_schema();
    if (!colormap) {
        if (err && err_size) g_strlcpy(err, "Display not initialized", err_size);
        return false;
    }

    st_SCREENSHOT_GEOMETRY geom;
    screenshot_geometry_current(&geom);

    st_SCREENSHOT_IMAGE img;
    if (!screenshot_render_rgba(pixels, pitch, &geom, colormap, opts, &img, err, err_size)) {
        out->bad_opts = true;
        return false;
    }

    size_t len = 0;
    uint8_t *png = png_encode_rgba(img.rgba, img.width, img.height, &len);
    out->width = img.width;
    out->height = img.height;
    out->hires = img.hires;
    screenshot_image_free(&img);
    if (!png || len == 0) {
        g_free(png);
        if (err && err_size) g_strlcpy(err, "PNG encode failed", err_size);
        return false;
    }
    out->png = png;
    out->png_len = len;
    return true;
}
