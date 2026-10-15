/**
 * @file   screenshot_series.c
 * @brief  Série screenshotů: PNG každého (změněného) emulovaného snímku.
 *
 * Viz screenshot_series.h. Rozdělení práce:
 * - emu vlákno (screenshot_series_on_screen_done): kopie INDEX8 snímku do
 *   fronty; snímek úplně shodný s předchozím rovnou přeskočí (levné memcmp),
 *   hlídá limit `frames` a zpětný tlak,
 * - zapisovací vlákno (ss_writer_thread): vykreslení jádrem screenshotu,
 *   porovnání s posledním uloženým obrázkem (rozhoduje výsledek v oblasti,
 *   měřítku a ořezu série), PNG a index.csv.
 *
 * Konec série: stop (uživatel, limit snímků, limit souborů, chyba) nastaví
 * příznak aktivity na 0 a vloží do fronty zarážku; zapisovací vlákno dopíše
 * vše před ní a skončí. Vlákno se připojí (join) při dalším startu nebo
 * v screenshot_series_exit().
 *
 * @par Licence: GPLv3
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mzarch/mzarch_config.h"
#include "gdg/video.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/framebuffer.h"
#include "display.h"
#include "debugger/png_encode.h"

#include "screenshot_capture.h"
#include "screenshot_config.h"
#include "screenshot_series.h"

/** @brief Délka fronty, nad kterou emu vlákno čeká (zpětný tlak). */
#define SS_QUEUE_HIGH 8
/** @brief Délka fronty, pod kterou čekání skončí. */
#define SS_QUEUE_LOW 4
/** @brief Velikost snímku INDEX8 [B]. */
#define SS_FRAME_BYTES ((size_t)VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT)

/** @brief Položka fronty: jeden emulovaný snímek. */
typedef struct st_SS_ITEM {
    uint64_t frame;      /**< Pořadí snímku od startu série (0 = první). */
    uint64_t gdg_screen; /**< g_gdg.total_elapsed.screens v okamžiku snímku. */
    uint8_t pixels[];    /**< INDEX8 framebuffer, SS_FRAME_BYTES bajtů. */
} st_SS_ITEM;

/** @brief Zarážka konce série ve frontě (nealokuje se, neuvolňuje se). */
static st_SS_ITEM s_sentinel;

/**
 * @brief Stav modulu.
 *
 * Invarianty: `active` (g_ss_active) = 1 jen mezi startem a stop; po
 * vložení zarážky už emu vlákno nic nevloží (kontrola pod mutexem). Fronta
 * má vlastní referenci pro každé použití mimo mutex (zpětný tlak).
 */
static struct {
    GMutex mutex;                        /**< Chrání vše kromě polí jen pro emu / writer vlákno. */
    GThread *writer;                     /**< Zapisovací vlákno (NULL = žádné). */
    GAsyncQueue *queue;                  /**< Fronta st_SS_ITEM* (vlastní modul). */
    st_SCREENSHOT_SERIES_PARAMS params;  /**< Parametry běžící série. */
    st_SCREENSHOT_SERIES_STATUS status;  /**< Stav (běžící nebo poslední). */
    st_SCREENSHOT_GEOMETRY geom;         /**< Geometrie framebufferu. */
    uint32_t palette[16];                /**< Kopie palety displeje ze startu. */
    FILE *csv;                           /**< index.csv (jen writer po startu). */
    uint8_t *prev;                       /**< Emu vlákno: poslední předaný snímek (předfiltr). */
    bool prev_valid;                     /**< Emu vlákno: `prev` je platný. */
} g_ss;

/** @brief Série snímá (atomické čtení v háčku konce snímku). */
static gint g_ss_active = 0;


/** @brief Zapíše anglický text do `err` (je-li zadaný). */
static void ss_error(char *err, size_t err_size, const char *msg)
{
    if (err && err_size) g_strlcpy(err, msg, err_size);
}


/**
 * @brief Ukončí snímání (s důvodem); zarážku vloží jen jednou.
 * @param reason Anglický důvod pro status.stop_reason.
 * @pre Drží g_ss.mutex.
 */
static void ss_request_stop_locked(const char *reason)
{
    if (!g_atomic_int_get(&g_ss_active)) return;
    g_atomic_int_set(&g_ss_active, 0);
    g_ss.status.active = false;
    g_strlcpy(g_ss.status.stop_reason, reason, sizeof(g_ss.status.stop_reason));
    g_async_queue_push(g_ss.queue, &s_sentinel);
}


/*******************************************************************************
 *
 *                  Zapisovací vlákno
 *
 ******************************************************************************/

/**
 * @brief Zpracuje jeden snímek: vykreslí, porovná, zapíše PNG a řádek CSV.
 * @param it       Snímek.
 * @param prev     Poslední uložený obrázek (vstup/výstup, vlastní writer).
 * @return false = chyba (série se ukončí), true jinak.
 */
static bool ss_writer_frame(const st_SS_ITEM *it, st_SCREENSHOT_IMAGE *prev)
{
    char err[SCREENSHOT_SERIES_TEXT_MAX] = "";
    st_SCREENSHOT_IMAGE img;
    if (!screenshot_render_rgba(it->pixels, VIDEO_DISPLAY_WIDTH, &g_ss.geom, g_ss.palette,
                                &g_ss.params.opts, &img, err, sizeof(err))) {
        g_mutex_lock(&g_ss.mutex);
        g_strlcpy(g_ss.status.last_error, err, sizeof(g_ss.status.last_error));
        g_mutex_unlock(&g_ss.mutex);
        return false;
    }

    if (g_ss.params.only_changed && prev->rgba && prev->width == img.width && prev->height == img.height &&
        memcmp(prev->rgba, img.rgba, (size_t)img.width * img.height * 4) == 0) {
        screenshot_image_free(&img);
        g_mutex_lock(&g_ss.mutex);
        g_ss.status.unchanged++;
        g_mutex_unlock(&g_ss.mutex);
        return true;
    }

    char name[32];
    g_snprintf(name, sizeof(name), "frame_%07" G_GUINT64_FORMAT ".png", it->frame);
    char *path = g_build_filename(g_ss.status.dir, name, NULL);
    size_t png_len = 0;
    uint8_t *png = png_encode_rgba(img.rgba, img.width, img.height, &png_len);
    GError *werr = NULL;
    bool ok = png && png_len && g_file_set_contents(path, (const gchar *)png, (gssize)png_len, &werr);
    g_free(png);
    g_free(path);
    if (!ok) {
        g_mutex_lock(&g_ss.mutex);
        g_snprintf(g_ss.status.last_error, sizeof(g_ss.status.last_error), "Cannot write %s: %s", name,
                   werr ? werr->message : "PNG encode failed");
        g_mutex_unlock(&g_ss.mutex);
        if (werr) g_error_free(werr);
        screenshot_image_free(&img);
        return false;
    }
    fprintf(g_ss.csv, "%" G_GUINT64_FORMAT ",%s,%" G_GUINT64_FORMAT "\n", it->frame, name, it->gdg_screen);

    screenshot_image_free(prev);
    *prev = img;

    g_mutex_lock(&g_ss.mutex);
    g_ss.status.files++;
    if (g_ss.status.files >= g_ss.params.max_files) {
        ss_request_stop_locked("File limit reached");
    }
    g_mutex_unlock(&g_ss.mutex);
    return true;
}


/**
 * @brief Tělo zapisovacího vlákna: zpracovává frontu do zarážky.
 * @param data Nepoužito.
 * @return NULL.
 */
static gpointer ss_writer_thread(gpointer data)
{
    (void)data;
    st_SCREENSHOT_IMAGE prev = { 0 };
    bool failed = false;
    for (;;) {
        st_SS_ITEM *it = (st_SS_ITEM *)g_async_queue_pop(g_ss.queue);
        if (it == &s_sentinel) break;
        if (!failed && !ss_writer_frame(it, &prev)) {
            failed = true;
            g_mutex_lock(&g_ss.mutex);
            ss_request_stop_locked("Error");
            g_mutex_unlock(&g_ss.mutex);
        }
        g_free(it);
    }
    screenshot_image_free(&prev);
    fclose(g_ss.csv);
    g_ss.csv = NULL;
    g_mutex_lock(&g_ss.mutex);
    g_ss.status.writing = false;
    g_mutex_unlock(&g_ss.mutex);
    return NULL;
}


/**
 * @brief Připojí dokončené zapisovací vlákno a uvolní frontu.
 * @return false = vlákno ještě zapisuje (nic se nestalo).
 * @pre Nedrží g_ss.mutex; série neběží (active = 0).
 */
static bool ss_join_finished(void)
{
    g_mutex_lock(&g_ss.mutex);
    bool writing = g_ss.status.writing;
    GThread *t = g_ss.writer;
    g_mutex_unlock(&g_ss.mutex);
    if (writing) return false;
    if (t) {
        g_thread_join(t);
        g_mutex_lock(&g_ss.mutex);
        g_ss.writer = NULL;
        if (g_ss.queue) g_async_queue_unref(g_ss.queue);
        g_ss.queue = NULL;
        g_mutex_unlock(&g_ss.mutex);
    }
    return true;
}


/*******************************************************************************
 *
 *                  Start, stop, stav
 *
 ******************************************************************************/

/**
 * @brief Sestaví výchozí adresář série `<výstupní složka>/<platforma>_frames_RRRRMMDD_HHMMSS`.
 * @param out  Výstup.
 * @param size Velikost `out`.
 */
static void ss_default_dir(char *out, size_t size)
{
    char base[SCREENSHOT_SERIES_PATH_MAX];
    screenshot_resolve_output_dir(base, sizeof(base));
    char stamp[32];
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    char name[64];
    g_snprintf(name, sizeof(name), "%s_frames_%s", MZARCH_NAME, stamp);
    char *dir = g_build_filename(base, name, NULL);
    g_strlcpy(out, dir, size);
    g_free(dir);
}


bool screenshot_series_start(const st_SCREENSHOT_SERIES_PARAMS *p, char *err, size_t err_size)
{
    if (g_atomic_int_get(&g_ss_active)) {
        ss_error(err, err_size, "A frame series is already running");
        return false;
    }
    if (!ss_join_finished()) {
        ss_error(err, err_size, "The previous frame series is still being written");
        return false;
    }
    uint32_t *colormap = display_get_default_color_schema();
    if (!colormap) {
        ss_error(err, err_size, "Display not initialized");
        return false;
    }

    char dir[SCREENSHOT_SERIES_PATH_MAX];
    if (p->dir[0]) {
        g_strlcpy(dir, p->dir, sizeof(dir));
    } else {
        ss_default_dir(dir, sizeof(dir));
    }
    if (g_mkdir_with_parents(dir, 0755) != 0 || !g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        char msg[SCREENSHOT_SERIES_TEXT_MAX];
        g_snprintf(msg, sizeof(msg), "Cannot create directory '%s'", dir);
        ss_error(err, err_size, msg);
        return false;
    }
    char *csv_path = g_build_filename(dir, "index.csv", NULL);
    FILE *csv = g_fopen(csv_path, "wb"); /* binárně: LF na všech platformách */
    g_free(csv_path);
    if (!csv) {
        char msg[SCREENSHOT_SERIES_TEXT_MAX];
        g_snprintf(msg, sizeof(msg), "Cannot create index.csv in '%s'", dir);
        ss_error(err, err_size, msg);
        return false;
    }
    fprintf(csv, "frame,file,gdg_screen\n");

    g_mutex_lock(&g_ss.mutex);
    g_ss.params = *p;
    if (g_ss.params.max_files == 0) g_ss.params.max_files = SCREENSHOT_SERIES_MAX_FILES_DEFAULT;
    screenshot_geometry_current(&g_ss.geom);
    memcpy(g_ss.palette, colormap, sizeof(g_ss.palette));
    g_ss.csv = csv;
    memset(&g_ss.status, 0, sizeof(g_ss.status));
    g_strlcpy(g_ss.status.dir, dir, sizeof(g_ss.status.dir));
    g_ss.status.active = true;
    g_ss.status.writing = true;
    if (!g_ss.prev) g_ss.prev = (uint8_t *)g_malloc(SS_FRAME_BYTES);
    g_ss.prev_valid = false;
    g_ss.queue = g_async_queue_new();
    g_ss.writer = g_thread_new("screenshot-series", ss_writer_thread, NULL);
    g_atomic_int_set(&g_ss_active, 1);
    g_mutex_unlock(&g_ss.mutex);
    fprintf(stderr, "[screenshot] Frame series started: %s\n", dir);
    return true;
}


bool screenshot_series_stop(void)
{
    g_mutex_lock(&g_ss.mutex);
    bool was = g_atomic_int_get(&g_ss_active) != 0;
    ss_request_stop_locked("Stopped");
    g_mutex_unlock(&g_ss.mutex);
    return was;
}


void screenshot_series_get_status(st_SCREENSHOT_SERIES_STATUS *out)
{
    g_mutex_lock(&g_ss.mutex);
    *out = g_ss.status;
    g_mutex_unlock(&g_ss.mutex);
}


/*******************************************************************************
 *
 *                  Háček konce snímku (emu vlákno)
 *
 ******************************************************************************/

void screenshot_series_on_screen_done(void)
{
    if (!g_atomic_int_get(&g_ss_active)) return;

    const uint8_t *px = g_framebuffer.pixels;
    if (!px) return;

    GAsyncQueue *q = NULL;
    g_mutex_lock(&g_ss.mutex);
    if (!g_atomic_int_get(&g_ss_active)) {
        g_mutex_unlock(&g_ss.mutex);
        return;
    }
    uint64_t frame = g_ss.status.frames_seen++;
    if (g_ss.params.only_changed && g_ss.prev_valid && memcmp(g_ss.prev, px, SS_FRAME_BYTES) == 0) {
        /* Úplně stejný snímek: výsledek by byl stejný, writer ho nemusí vidět. */
        g_ss.status.unchanged++;
    } else {
        st_SS_ITEM *it = (st_SS_ITEM *)g_malloc(sizeof(st_SS_ITEM) + SS_FRAME_BYTES);
        it->frame = frame;
        it->gdg_screen = g_gdg.total_elapsed.screens;
        memcpy(it->pixels, px, SS_FRAME_BYTES);
        memcpy(g_ss.prev, px, SS_FRAME_BYTES);
        g_ss.prev_valid = true;
        g_async_queue_push(g_ss.queue, it);
    }
    if (g_ss.params.frames && g_ss.status.frames_seen >= g_ss.params.frames) {
        ss_request_stop_locked("Frame count reached");
    }
    q = g_async_queue_ref(g_ss.queue);
    g_mutex_unlock(&g_ss.mutex);

    /* Zpětný tlak: writer nestíhá -> počkat (žádný snímek se neztratí). */
    if (g_async_queue_length(q) > SS_QUEUE_HIGH) {
        while (g_atomic_int_get(&g_ss_active) && g_async_queue_length(q) >= SS_QUEUE_LOW) {
            g_usleep(1000);
        }
    }
    g_async_queue_unref(q);
}


void screenshot_series_exit(void)
{
    screenshot_series_stop();
    g_mutex_lock(&g_ss.mutex);
    GThread *t = g_ss.writer;
    g_mutex_unlock(&g_ss.mutex);
    if (t) {
        g_thread_join(t);
        g_mutex_lock(&g_ss.mutex);
        g_ss.writer = NULL;
        if (g_ss.queue) g_async_queue_unref(g_ss.queue);
        g_ss.queue = NULL;
        g_mutex_unlock(&g_ss.mutex);
    }
    g_free(g_ss.prev);
    g_ss.prev = NULL;
}
