/**
 * @file   screenshot_capture.h
 * @brief  Screenshot aktuální platformy: geometrie framebufferu a PNG.
 *
 * Tenká vrstva nad čistým jádrem screenshot.h: doplní geometrii překládané
 * platformy (VIDEO_* makra), paletu displeje a zakóduje PNG. Odkud se
 * berou pixely, rozhoduje volající:
 * - MCP (dbgapi, emu vlákno): poslední hotový snímek nebo živý framebuffer,
 * - GUI (UI vlákno): surface, který okno emulátoru právě zobrazuje.
 *
 * @par Licence: GPLv3
 */

#ifndef SCREENSHOT_CAPTURE_H
#define SCREENSHOT_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "screenshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Vyplní geometrii framebufferu překládané platformy.
 * @param g Výstup (nesmí být NULL).
 */
void screenshot_geometry_current(st_SCREENSHOT_GEOMETRY *g);

/**
 * @brief Výsledek screenshot_encode_png().
 *
 * Vlastnictví: `png` alokuje screenshot_encode_png() (glib), uvolňuje
 * volající přes g_free().
 */
typedef struct st_SCREENSHOT_PNG {
    uint8_t *png;     /**< PNG stream. */
    size_t png_len;   /**< Délka PNG streamu v bajtech. */
    unsigned width;   /**< Šířka obrázku [px]. */
    unsigned height;  /**< Výška obrázku [px]. */
    bool hires;       /**< Měřítko native ponechalo šířku 640 bodů. */
    bool bad_opts;    /**< Při selhání: true = neplatné volby (ořez mimo obraz, měřítko), ne chyba displeje. */
} st_SCREENSHOT_PNG;

/**
 * @brief Z INDEX8 framebufferu aktuální platformy vyrobí PNG podle voleb.
 *
 * Barvy bere z palety displeje (display_get_default_color_schema()).
 *
 * @param pixels   INDEX8 framebuffer VIDEO_DISPLAY_WIDTH x VIDEO_DISPLAY_HEIGHT.
 * @param pitch    Délka řádku `pixels` v bajtech.
 * @param opts     Volby oblasti, měřítka a ořezu.
 * @param out      Výstup; při úspěchu out->png vlastní volající.
 * @param err      Buffer na anglický popis chyby, nebo NULL.
 * @param err_size Velikost `err`.
 * @return true = PNG vyrobeno; false = displej neinicializovaný, neplatné
 *         volby nebo chyba kódování (popis v `err`, out->png = NULL).
 * @pre `pixels` se během volání nemění (volající drží příslušný zámek
 *      nebo je ve vlákně, které buffer zapisuje).
 */
bool screenshot_encode_png(const uint8_t *pixels, size_t pitch,
                           const st_SCREENSHOT_OPTS *opts,
                           st_SCREENSHOT_PNG *out,
                           char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* SCREENSHOT_CAPTURE_H */
