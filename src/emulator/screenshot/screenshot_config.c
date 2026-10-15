/**
 * @file   screenshot_config.c
 * @brief  Nastavení screenshotu z GUI a INI modul `[SCREENSHOT]`.
 *
 * Vzor videorec_config.c: každý prvek má propagate callback (INI ->
 * g_screenshot_settings) a save callback (g_screenshot_settings -> INI).
 * Neplatné hodnoty se při propagaci nahradí výchozími.
 *
 * @par Licence: GPLv3
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <time.h>

#include "screenshot_config.h"
#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"
#include "libs/sdlapp/sdlapp.h"

/** Aplikační kontext (main.c); z něj se bere home_dir pro výchozí adresář. */
extern SdlApp *g_sdlapp;

/** @brief Výchozí měřítko z GUI: okno na 100 %. */
#define SCREENSHOT_DEFAULT_WINDOW_SCALE 2

st_SCREENSHOT_SETTINGS g_screenshot_settings = {
    SCREENSHOT_AREA_FULL, SCREENSHOT_SCALE_WINDOW, SCREENSHOT_DEFAULT_WINDOW_SCALE, "", true
};


/* ========================================================================= */
/*                       Propagate a save callbacky                          */
/* ========================================================================= */

/** @brief INI -> area (neznámé jméno -> full). @param e Prvek. @param data Nepoužito. */
static void propagatecfg_area(void *e, void *data)
{
    (void)data;
    en_SCREENSHOT_AREA a = SCREENSHOT_AREA_FULL;
    char *name = cfgelement_get_text_value((st_CFGELEMENT *)e);
    if (!name || !screenshot_area_from_string(name, &a)) a = SCREENSHOT_AREA_FULL;
    g_screenshot_settings.area = a;
}

/** @brief area -> INI. @param e Prvek. @param data Nepoužito. */
static void savecfg_area(void *e, void *data)
{
    (void)data;
    cfgelement_set_text_value((st_CFGELEMENT *)e, screenshot_area_to_string(g_screenshot_settings.area));
}

/** @brief INI -> scale (native nebo 1..4; jinak 2). @param e Prvek. @param data Nepoužito. */
static void propagatecfg_scale(void *e, void *data)
{
    (void)data;
    en_SCREENSHOT_SCALE_MODE m;
    unsigned s;
    char *name = cfgelement_get_text_value((st_CFGELEMENT *)e);
    if (!name || !screenshot_scale_from_string(name, &m, &s) || m == SCREENSHOT_SCALE_FRAMEBUFFER) {
        m = SCREENSHOT_SCALE_WINDOW;
        s = SCREENSHOT_DEFAULT_WINDOW_SCALE;
    }
    g_screenshot_settings.scale_mode = m;
    g_screenshot_settings.window_scale = s;
}

/** @brief scale -> INI. @param e Prvek. @param data Nepoužito. */
static void savecfg_scale(void *e, void *data)
{
    (void)data;
    cfgelement_set_text_value((st_CFGELEMENT *)e,
                              screenshot_scale_to_string(g_screenshot_settings.scale_mode,
                                                         g_screenshot_settings.window_scale));
}

/** @brief INI -> output_dir. @param e Prvek. @param data Nepoužito. */
static void propagatecfg_output_dir(void *e, void *data)
{
    (void)data;
    char *dir = cfgelement_get_text_value((st_CFGELEMENT *)e);
    g_strlcpy(g_screenshot_settings.output_dir, dir ? dir : "", sizeof(g_screenshot_settings.output_dir));
}

/** @brief output_dir -> INI. @param e Prvek. @param data Nepoužito. */
static void savecfg_output_dir(void *e, void *data)
{
    (void)data;
    cfgelement_set_text_value((st_CFGELEMENT *)e, g_screenshot_settings.output_dir);
}

/** @brief INI -> series_only_changed. @param e Prvek. @param data Nepoužito. */
static void propagatecfg_series_only_changed(void *e, void *data)
{
    (void)data;
    g_screenshot_settings.series_only_changed = cfgelement_get_bool_value((st_CFGELEMENT *)e) != 0;
}

/** @brief series_only_changed -> INI. @param e Prvek. @param data Nepoužito. */
static void savecfg_series_only_changed(void *e, void *data)
{
    (void)data;
    cfgelement_set_bool_value((st_CFGELEMENT *)e, g_screenshot_settings.series_only_changed ? 1 : 0);
}


void screenshot_config_init(void)
{
    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "SCREENSHOT");
    CFGELM *elm;

    elm = cfgmodule_register_new_element(cmod, "area", CFGENTYPE_TEXT, "full");
    cfgelement_set_propagate_cb(elm, propagatecfg_area, NULL);
    cfgelement_set_save_cb(elm, savecfg_area, NULL);

    elm = cfgmodule_register_new_element(cmod, "scale", CFGENTYPE_TEXT, "2");
    cfgelement_set_propagate_cb(elm, propagatecfg_scale, NULL);
    cfgelement_set_save_cb(elm, savecfg_scale, NULL);

    elm = cfgmodule_register_new_element(cmod, "output_dir", CFGENTYPE_TEXT, "");
    cfgelement_set_propagate_cb(elm, propagatecfg_output_dir, NULL);
    cfgelement_set_save_cb(elm, savecfg_output_dir, NULL);

    elm = cfgmodule_register_new_element(cmod, "series_only_changed", CFGENTYPE_BOOL, 1);
    cfgelement_set_propagate_cb(elm, propagatecfg_series_only_changed, NULL);
    cfgelement_set_save_cb(elm, savecfg_series_only_changed, NULL);

    /* Načíst sekci z INI a propagovat (vzor videorec_config_init). */
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}


/* ========================================================================= */
/*                       Volby, adresář, jméno souboru                       */
/* ========================================================================= */

void screenshot_settings_to_opts(const st_SCREENSHOT_SETTINGS *s, st_SCREENSHOT_OPTS *opts)
{
    screenshot_opts_init(opts);
    opts->area = s->area;
    opts->scale_mode = s->scale_mode;
    opts->window_scale = s->window_scale;
}


bool screenshot_resolve_output_dir(char *out, size_t size)
{
    if (g_screenshot_settings.output_dir[0]) {
        g_strlcpy(out, g_screenshot_settings.output_dir, size);
        (void)g_mkdir_with_parents(out, 0755);
        return g_file_test(out, G_FILE_TEST_IS_DIR);
    }
    if (g_sdlapp && g_sdlapp->paths && g_sdlapp->paths->home_dir) {
        char *dir = g_build_filename(g_sdlapp->paths->home_dir, "screenshots", NULL);
        bool ok = (g_mkdir_with_parents(dir, 0755) == 0) && g_file_test(dir, G_FILE_TEST_IS_DIR);
        if (ok) g_strlcpy(out, dir, size);
        g_free(dir);
        if (ok) return true;
    }
    const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    g_strlcpy(out, (pictures && g_file_test(pictures, G_FILE_TEST_IS_DIR)) ? pictures : ".", size);
    return true;
}


void screenshot_make_filename(const char *dir, char *out, size_t size)
{
    char stamp[32];
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);

    for (unsigned n = 1; n < 1000; n++) {
        char name[96];
        if (n == 1) {
            g_snprintf(name, sizeof(name), "%s_%s.png", MZARCH_NAME, stamp);
        } else {
            g_snprintf(name, sizeof(name), "%s_%s_%u.png", MZARCH_NAME, stamp, n);
        }
        char *path = g_build_filename(dir, name, NULL);
        g_strlcpy(out, path, size);
        bool exists = g_file_test(path, G_FILE_TEST_EXISTS);
        g_free(path);
        if (!exists) return;
    }
}
