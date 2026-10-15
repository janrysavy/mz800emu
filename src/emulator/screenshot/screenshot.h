/**
 * @file   screenshot.h
 * @brief  Výřez a měřítko screenshotu z indexovaného framebufferu (čisté jádro).
 *
 * Modul převede INDEX8 obraz framebufferu (paleta 16 barev) na RGBA obrázek
 * podle zvolené oblasti a měřítka. Používá ho MCP (screenshot_save_to_file)
 * i GUI (menu Tools -> Save Screenshot).
 *
 * Geometrie framebufferu: jeden pixel framebufferu = jeden takt GDG
 * vodorovně a jeden obrazový řádek svisle. Plátno (canvas) má 640 pixelů
 * framebufferu na řádek; v režimech 320 bodů připadají na jeden bod dva
 * pixely framebufferu. Okno emulátoru zobrazuje framebuffer se zdvojenými
 * řádky (poměr stran jako na obrazovce).
 *
 * Režimy měřítka (st_SCREENSHOT_OPTS.scale_mode):
 * - SCREENSHOT_SCALE_FRAMEBUFFER: pixely framebufferu 1:1 (MZ-800 celý
 *   obraz 928x288) - výchozí chování MCP.
 * - SCREENSHOT_SCALE_NATIVE: body počítače 1:1. Když jsou v oblasti všechny
 *   dvojice sousedních pixelů framebufferu shodné (režim 320 bodů), šířka
 *   se zpoloviční (MZ-800 plátno 320x200, celý obraz 464x288), jinak zůstane
 *   (640x200, 928x288). Řádky 1:1.
 * - SCREENSHOT_SCALE_WINDOW: poměr stran jako v okně emulátoru, měřítko
 *   1..4; šířka = šířka oblasti * k / 2, výška = výška oblasti * k. Měřítko 2
 *   odpovídá oknu na 100 % (MZ-800 celý obraz 928x576). Měřítko 1 v režimu
 *   640 bodů vynechá každý druhý sloupec.
 *
 * Ořez (crop) je dostupný jen v režimech FRAMEBUFFER a NATIVE, souřadnice
 * jsou ve výstupních pixelech daného režimu.
 *
 * @par Vlákna
 * Funkce jsou čisté (bez globálního stavu), smí se volat z libovolného
 * vlákna; volající ručí za to, že vstupní buffer se během volání nemění.
 *
 * @par Licence: GPLv3
 */

#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Nejvyšší měřítko režimu SCREENSHOT_SCALE_WINDOW. */
#define SCREENSHOT_WINDOW_SCALE_MAX 4

/**
 * @brief Geometrie framebufferu platformy.
 *
 * Invarianty (ověřuje screenshot_render_rgba): plátno leží celé uvnitř
 * framebufferu, šířka framebufferu i plátna a levý okraj plátna jsou sudé
 * (dvojice pixelů bodu 320 začínají na sudém sloupci).
 */
typedef struct st_SCREENSHOT_GEOMETRY {
    unsigned fb_width;   /**< Šířka framebufferu [px] (VIDEO_DISPLAY_WIDTH, včetně borderu). */
    unsigned fb_height;  /**< Výška framebufferu [px] (VIDEO_DISPLAY_HEIGHT, včetně borderu). */
    unsigned canvas_x;   /**< Levý okraj plátna ve framebufferu [px] (VIDEO_BORDER_LEFT_WIDTH). */
    unsigned canvas_y;   /**< Horní okraj plátna ve framebufferu [px] (VIDEO_BORDER_TOP_HEIGHT). */
    unsigned canvas_w;   /**< Šířka plátna [px] (VIDEO_CANVAS_WIDTH). */
    unsigned canvas_h;   /**< Výška plátna [px] (VIDEO_CANVAS_HEIGHT). */
} st_SCREENSHOT_GEOMETRY;

/** @brief Oblast obrazu, která se ukládá. */
typedef enum en_SCREENSHOT_AREA {
    SCREENSHOT_AREA_FULL = 0, /**< Celý framebuffer včetně borderu. */
    SCREENSHOT_AREA_CANVAS,   /**< Jen plátno (obraz bez borderu). */
} en_SCREENSHOT_AREA;

/** @brief Režim měřítka - viz popis souboru. */
typedef enum en_SCREENSHOT_SCALE_MODE {
    SCREENSHOT_SCALE_FRAMEBUFFER = 0, /**< Pixely framebufferu 1:1. */
    SCREENSHOT_SCALE_NATIVE,          /**< Body počítače 1:1 (320 nebo 640 podle obsahu). */
    SCREENSHOT_SCALE_WINDOW,          /**< Poměr stran okna, měřítko window_scale. */
} en_SCREENSHOT_SCALE_MODE;

/** @brief Volby screenshotu. */
typedef struct st_SCREENSHOT_OPTS {
    en_SCREENSHOT_AREA area;             /**< Oblast obrazu. */
    en_SCREENSHOT_SCALE_MODE scale_mode; /**< Režim měřítka. */
    unsigned window_scale;               /**< Měřítko 1..SCREENSHOT_WINDOW_SCALE_MAX (jen SCALE_WINDOW). */
    bool crop;                           /**< true = uložit jen výřez crop_* (jen FRAMEBUFFER a NATIVE). */
    unsigned crop_x;                     /**< Levý okraj výřezu ve výstupních pixelech. */
    unsigned crop_y;                     /**< Horní okraj výřezu ve výstupních pixelech. */
    unsigned crop_w;                     /**< Šířka výřezu (> 0). */
    unsigned crop_h;                     /**< Výška výřezu (> 0). */
} st_SCREENSHOT_OPTS;

/**
 * @brief Výsledný obrázek.
 *
 * Vlastnictví: `rgba` alokuje screenshot_render_rgba() přes g_malloc,
 * uvolňuje volající přes screenshot_image_free() (nebo g_free).
 */
typedef struct st_SCREENSHOT_IMAGE {
    uint8_t *rgba;    /**< Pixely RGBA8888, řádky bez mezer (width * 4 bajty). */
    unsigned width;   /**< Šířka [px]. */
    unsigned height;  /**< Výška [px]. */
    bool hires;       /**< SCALE_NATIVE: true = ponechána šířka 640 bodů; jinak false. */
} st_SCREENSHOT_IMAGE;

/**
 * @brief Nastaví výchozí volby: celý framebuffer 1:1, bez ořezu (dnešní MCP).
 * @param opts Volby k inicializaci (nesmí být NULL).
 */
void screenshot_opts_init(st_SCREENSHOT_OPTS *opts);

/**
 * @brief Vyrobí RGBA obrázek z INDEX8 framebufferu.
 *
 * @param pixels   INDEX8 framebuffer o rozměrech geom->fb_width x fb_height;
 *                 použije se dolní nibble indexu (pixel & 0x0F).
 * @param pitch    Délka řádku `pixels` v bajtech (>= fb_width).
 * @param geom     Geometrie framebufferu.
 * @param palette  16 barev 0x00RRGGBB (display_get_default_color_schema()).
 * @param opts     Volby oblasti, měřítka a ořezu.
 * @param out      Výstup; při úspěchu out->rgba vlastní volající.
 * @param err      Buffer na anglický popis chyby, nebo NULL.
 * @param err_size Velikost `err`.
 * @return true = obrázek vyroben; false = neplatné volby nebo geometrie
 *         (popis v `err`, out->rgba = NULL).
 * @pre Žádný z ukazatelů kromě `err` není NULL.
 */
bool screenshot_render_rgba(const uint8_t *pixels, size_t pitch,
                            const st_SCREENSHOT_GEOMETRY *geom,
                            const uint32_t palette[16],
                            const st_SCREENSHOT_OPTS *opts,
                            st_SCREENSHOT_IMAGE *out,
                            char *err, size_t err_size);

/**
 * @brief Uvolní pixely obrázku a vynuluje strukturu (NULL bezpečné).
 * @param img Obrázek.
 */
void screenshot_image_free(st_SCREENSHOT_IMAGE *img);

/**
 * @brief Převede název oblasti ("full", "canvas") na enum.
 * @return true = známý název (výsledek v `out`), false = neznámý.
 */
bool screenshot_area_from_string(const char *s, en_SCREENSHOT_AREA *out);

/** @brief Vrátí název oblasti ("full" / "canvas"). */
const char *screenshot_area_to_string(en_SCREENSHOT_AREA area);

/**
 * @brief Převede název měřítka na režim a číselné měřítko.
 *
 * Přijímá "framebuffer", "native" a "1".."4" (okno).
 *
 * @param s     Název.
 * @param mode  Výstup: režim.
 * @param scale Výstup: měřítko okna (1..4), u ostatních režimů 1.
 * @return true = známý název, false = neznámý (výstupy beze změny).
 */
bool screenshot_scale_from_string(const char *s, en_SCREENSHOT_SCALE_MODE *mode, unsigned *scale);

/**
 * @brief Vrátí název měřítka ("framebuffer", "native", "1".."4").
 * @return Statický řetězec; pro neplatné měřítko okna "2".
 */
const char *screenshot_scale_to_string(en_SCREENSHOT_SCALE_MODE mode, unsigned scale);

#ifdef __cplusplus
}
#endif

#endif /* SCREENSHOT_H */
