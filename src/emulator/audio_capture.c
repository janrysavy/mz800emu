/**
 * @file   audio_capture.c
 * @brief  Zachycení zvuku do složky - implementace (viz audio_capture.h).
 *
 * Události kanálů a zápisy do PSG se zapisují do CSV hned v emu vlákně
 * (stdio s velkým bufferem). reference.wav renderuje videorec_audio
 * (stejný renderer jako video záznam v emulovaném čase) po každém
 * zvukovém snímku do horizontu, do kterého jsou všechny změny doručené.
 * sdl.wav dostává hotové bloky SDL cesty z háčku v iface_audio_wait_for_data().
 *
 * @par Licence: GPLv3
 */

#include "audio_capture.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mzarch/mzarch_config.h"
#include "audio.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/video.h"
#include "hw-generic/psg/psg.h"
#include "iface/iface_audio.h"
#include "libs/sdlapp/sdlapp.h"

/* Globální instance aplikace (main.c) - domovský adresář pro výchozí složku. */
extern SdlApp *g_sdlapp;
#include "videorec/videorec_audio.h"
#include "cfgmain.h"
#include "emulator.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"


gint g_audio_capture_flags = 0;

/** @brief Velikost stdio bufferu CSV souborů [B]. */
#define AC_CSV_BUFFER (256u * 1024u)
/** @brief Vzorků (stereo párů) na jedno volání rendereru. */
#define AC_RENDER_CHUNK 2048u
/**
 * @brief Zahřátí rendereru reference.wav před začátkem [takty GDG] = 100 ms.
 *
 * GDGCLK_BASE je dělitelný 10 na všech platformách (snímek x 50 nebo 60),
 * 100 ms je tedy přesně 4410 vzorků a celý počet taktů.
 */
#define AC_WARMUP_TICKS ((uint64_t) GDGCLK_BASE / 10u)
/**
 * @brief Zvukové snímky rozjezdu SDL cesty před začátkem (jen se sdl.wav).
 *
 * Bez zvukového zařízení (headless) SDL cesta mimo zachycení neběží a její
 * filtry a parkování začínají ze studeného stavu - trvalá úroveň (CTC0 = 15)
 * by první snímky hrála naplno. Během rozjezdu se bloky vyrábějí a zahazují;
 * 6 snímků (100 ms při 60 Hz) pokryje parkování 45 ms + doznění 20 ms.
 */
#define AC_SDL_WARMUP_FRAMES 6u

/** @brief Otevřený WAV soubor (16 bit PCM stereo, AUDIO_CAPTURE_WAV_RATE). */
typedef struct st_AC_WAV {
    FILE *fp;          /**< Soubor, NULL = nezapisuje se. */
    uint64_t samples;  /**< Zapsané stereo páry (pro hlavičku při zavření). */
} st_AC_WAV;

/**
 * @brief Stav modulu (emu vlákno; sdl a sdl_samples/sdl_blocks pod sdl_mutex).
 *
 * @invariant active => (origin, frame, last_horizon) platí pro běžící zachycení.
 * @invariant pending && active se nevylučují jen během audio_capture_on_frame().
 */
static struct {
    bool pending;                    /**< Start naplánovaný na konec snímku. */
    bool active;                     /**< Zachytává se. */
    st_AUDIO_CAPTURE_PARAMS params;  /**< Parametry posledního startu. */
    char dir[AUDIO_CAPTURE_PATH_MAX]; /**< Složka. */
    FILE *events;                    /**< events.csv, nebo NULL. */
    FILE *psg;                       /**< psg_writes.csv, nebo NULL. */
    st_AC_WAV ref;                   /**< reference.wav. */
    st_AC_WAV sdl;                   /**< sdl.wav (pod sdl_mutex). */
    uint64_t sdl_blocks;             /**< Zapsané bloky SDL cesty (pod sdl_mutex). */
    bool sdl_writing;                /**< Bloky SDL cesty se zapisují (false = rozjezd, zahazují se; pod sdl_mutex). */
    unsigned warmup_frames;          /**< Zbývající snímky rozjezdu SDL cesty před začátkem. */
    st_VIDEOREC_AUDIO renderer;      /**< Renderer reference.wav (platí jen při active && ref.fp). */
    bool renderer_init;              /**< renderer je inicializovaný (k uvolnění). */
    uint64_t origin;                 /**< Čas začátku zachycení [takty GDG]. */
    uint64_t last_horizon;           /**< Horizont posledního snímku. */
    uint64_t frame;                  /**< Pořadí aktuálního snímku od začátku (0 = první). */
    uint64_t n_events;               /**< Řádky events.csv. */
    uint64_t n_psg;                  /**< Řádky psg_writes.csv. */
    char started[32];                /**< Čas startu (ISO 8601, místní). */
    char stop_reason[AUDIO_CAPTURE_TEXT_MAX]; /**< Důvod konce. */
    char last_error[AUDIO_CAPTURE_TEXT_MAX];  /**< Poslední chyba. */
} s_ac;

/**
 * @brief Zámek nad celým s_ac.
 *
 * Veřejné funkce ho berou na vstupu a volají vnitřní `ac_*` funkce, které
 * předpokládají, že je zamčený. Háčky emu vlákna ho berou jen během
 * zachycení (audio_capture_wanted()), mimo zachycení stojí háček jedno
 * atomické čtení.
 */
static GMutex s_mutex;

/** @brief Po návratu z háčku ukončit emulátor (frames dosaženo s quit_after_stop). */
static bool s_quit_now;

st_AUDIO_CAPTURE_SETTINGS g_audio_capture_settings = { "", true, true, true, true };


/** @brief Název kanálu audio logu pro CSV ("ctc0", "psg0_0".."psg1_3"). */
static const char *ac_channel_name(unsigned src_id)
{
    static const char *const names[9] = {
        "ctc0", "psg0_0", "psg0_1", "psg0_2", "psg0_3", "psg1_0", "psg1_1", "psg1_2", "psg1_3"
    };
    return (src_id < 9) ? names[src_id] : "?";
}


/** @brief Zapíše text chyby do bufferu volajícího a do s_ac.last_error. */
static void ac_error(char *err, size_t err_size, const char *msg)
{
    g_strlcpy(s_ac.last_error, msg, sizeof(s_ac.last_error));
    if (err && err_size) g_strlcpy(err, msg, err_size);
}


/** @brief Takty od začátku zachycení na µs jako text s desetinnou tečkou. */
static void ac_time_us(uint64_t rel_ticks, char *buf, size_t size)
{
    double us = (double) rel_ticks * 1000000.0 / (double) GDGCLK_BASE;
    g_ascii_formatd(buf, (int) size, "%.3f", us);
}


/** @brief Zapíše 44bajtovou hlavičku WAV (PCM 16 bit stereo) pro `samples` párů. */
static void ac_wav_header(FILE *fp, uint64_t samples)
{
    uint32_t data = (uint32_t) ((samples * 4u > 0xFFFFFFF0u) ? 0xFFFFFFF0u : samples * 4u);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t riff = 36u + data;
    for (int i = 0; i < 4; i++) h[4 + i] = (uint8_t) (riff >> (8 * i));
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fmt_len = 16, rate = AUDIO_CAPTURE_WAV_RATE, byte_rate = AUDIO_CAPTURE_WAV_RATE * 4u;
    for (int i = 0; i < 4; i++) h[16 + i] = (uint8_t) (fmt_len >> (8 * i));
    h[20] = 1; h[21] = 0;           /* PCM */
    h[22] = 2; h[23] = 0;           /* stereo */
    for (int i = 0; i < 4; i++) h[24 + i] = (uint8_t) (rate >> (8 * i));
    for (int i = 0; i < 4; i++) h[28 + i] = (uint8_t) (byte_rate >> (8 * i));
    h[32] = 4; h[33] = 0;           /* block align */
    h[34] = 16; h[35] = 0;          /* bitů na vzorek */
    memcpy(h + 36, "data", 4);
    for (int i = 0; i < 4; i++) h[40 + i] = (uint8_t) (data >> (8 * i));
    fwrite(h, 1, sizeof(h), fp);
}


/**
 * @brief Otevře WAV v adresáři zachycení (s dočasnou hlavičkou).
 * @return false = soubor nejde vytvořit.
 */
static bool ac_wav_open(st_AC_WAV *w, const char *dir, const char *name)
{
    char *path = g_build_filename(dir, name, NULL);
    w->fp = g_fopen(path, "wb");
    g_free(path);
    w->samples = 0;
    if (!w->fp) return false;
    ac_wav_header(w->fp, 0);
    return true;
}


/** @brief Zapíše stereo páry int16 (little endian) do WAV. */
static void ac_wav_write(st_AC_WAV *w, const int16_t *lr, size_t frames)
{
    if (!w->fp || frames == 0) return;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    fwrite(lr, sizeof(int16_t), frames * 2u, w->fp);
#else
    for (size_t i = 0; i < frames * 2u; i++) {
        uint8_t b[2] = { (uint8_t) lr[i], (uint8_t) ((uint16_t) lr[i] >> 8) };
        fwrite(b, 1, 2, w->fp);
    }
#endif
    w->samples += frames;
}


/** @brief Dopíše hlavičku WAV a zavře ho. @return false = chyba zápisu. */
static bool ac_wav_close(st_AC_WAV *w)
{
    if (!w->fp) return true;
    bool ok = !ferror(w->fp);
    if (fseek(w->fp, 0, SEEK_SET) == 0) ac_wav_header(w->fp, w->samples);
    else ok = false;
    if (fclose(w->fp) != 0) ok = false;
    w->fp = NULL;
    return ok;
}


/**
 * @brief Otevře CSV s hlavičkou a velkým bufferem.
 * @return Soubor, nebo NULL.
 */
static FILE *ac_csv_open(const char *dir, const char *name, const char *header)
{
    char *path = g_build_filename(dir, name, NULL);
    FILE *fp = g_fopen(path, "wb"); /* binárně: LF na všech platformách */
    g_free(path);
    if (!fp) return NULL;
    setvbuf(fp, NULL, _IOFBF, AC_CSV_BUFFER);
    fputs(header, fp);
    return fp;
}


/** @brief Zavře CSV. @return false = chyba zápisu. */
static bool ac_csv_close(FILE **fp)
{
    if (!*fp) return true;
    bool ok = !ferror(*fp);
    if (fclose(*fp) != 0) ok = false;
    *fp = NULL;
    return ok;
}


/** @brief Escapuje řetězec do JSON (uvozovky, zpětné lomítko, řídicí znaky). */
static void ac_json_str(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; *s; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '"' || c == '\\') { fputc('\\', fp); fputc(c, fp); }
        else if (c < 0x20) fprintf(fp, "\\u%04x", c);
        else fputc(c, fp);
    }
    fputc('"', fp);
}


/** @brief Zapíše capture.json s metadaty a konečnými počty. */
static void ac_write_json(void)
{
    char *path = g_build_filename(s_ac.dir, "capture.json", NULL);
    FILE *fp = g_fopen(path, "wb");
    g_free(path);
    if (!fp) {
        g_strlcpy(s_ac.last_error, "Cannot write capture.json", sizeof(s_ac.last_error));
        return;
    }
    fprintf(fp, "{\n  \"platform\": ");
    ac_json_str(fp, MZARCH_NAME);
    fprintf(fp, ",\n  \"started\": ");
    ac_json_str(fp, s_ac.started);
    fprintf(fp, ",\n  \"gdg_clk_hz\": %llu,\n  \"frames_per_sec\": %u,\n  \"wav_rate\": %u,\n",
            (unsigned long long) GDGCLK_BASE, (unsigned) VIDEO_SCREENS_PER_SEC, AUDIO_CAPTURE_WAV_RATE);
    fprintf(fp, "  \"channels\": [");
    for (unsigned i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) {
        fprintf(fp, "%s", i ? ", " : "");
        ac_json_str(fp, ac_channel_name(i));
    }
    fprintf(fp, "],\n  \"stereo_at_end\": %s,\n",
#if HAVE_PSG == 2
            g_psg_module.stereo ? "true" : "false"
#else
            "false"
#endif
    );
    uint64_t sdl_blocks = s_ac.sdl_blocks, sdl_samples = s_ac.sdl.samples;
    fprintf(fp, "  \"frames\": %llu,\n  \"events\": %llu,\n  \"psg_writes\": %llu,\n"
                "  \"reference_samples\": %llu,\n  \"sdl_blocks\": %llu,\n  \"sdl_samples\": %llu,\n",
            (unsigned long long) s_ac.frame, (unsigned long long) s_ac.n_events,
            (unsigned long long) s_ac.n_psg, (unsigned long long) s_ac.ref.samples,
            (unsigned long long) sdl_blocks, (unsigned long long) sdl_samples);
    fprintf(fp, "  \"stop_reason\": ");
    ac_json_str(fp, s_ac.stop_reason);
    fprintf(fp, ",\n  \"last_error\": ");
    ac_json_str(fp, s_ac.last_error);
    fprintf(fp, "\n}\n");
    fclose(fp);
}


/** @brief Zavře soubory, uvolní renderer, zapíše capture.json a nastaví důvod konce. */
static void ac_finish(const char *reason)
{
    g_atomic_int_set(&g_audio_capture_flags, 0);
    s_ac.pending = false;
    s_ac.active = false;
    g_strlcpy(s_ac.stop_reason, reason, sizeof(s_ac.stop_reason));

    bool ok = ac_csv_close(&s_ac.events);
    ok = ac_csv_close(&s_ac.psg) && ok;
    ok = ac_wav_close(&s_ac.ref) && ok;
    ok = ac_wav_close(&s_ac.sdl) && ok;
    s_ac.sdl_writing = false;
    if (!ok) g_strlcpy(s_ac.last_error, "Write error while closing the capture files", sizeof(s_ac.last_error));
    if (s_ac.renderer_init) {
        videorec_audio_free(&s_ac.renderer);
        s_ac.renderer_init = false;
    }
    ac_write_json();
    if (s_ac.params.quit_after_stop && strcmp(reason, "Frame count reached") == 0) {
        s_quit_now = true;
    }
}


/** @brief Sestaví výchozí složku `<výstupní složka zachycení>/<platforma>_audio_RRRRMMDD_HHMMSS`. */
static void ac_default_dir(char *out, size_t size)
{
    char base[AUDIO_CAPTURE_PATH_MAX];
    audio_capture_resolve_output_dir(base, sizeof(base));
    char name[64];
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    g_snprintf(name, sizeof(name), "%s_audio_%s", MZARCH_NAME, stamp);
    char *dir = g_build_filename(base, name, NULL);
    g_strlcpy(out, dir, size);
    g_free(dir);
}


/** @brief audio_capture_start() se zamčeným s_mutex. */
static bool ac_start(const st_AUDIO_CAPTURE_PARAMS *p, char *err, size_t err_size)
{
    if (s_ac.pending || s_ac.active) {
        ac_error(err, err_size, "An audio capture is already running");
        return false;
    }
    if (!p->events && !p->psg_writes && !p->reference_wav && !p->sdl_wav) {
        ac_error(err, err_size, "No output selected (events, psg_writes, reference_wav, sdl_wav)");
        return false;
    }

    char dir[AUDIO_CAPTURE_PATH_MAX];
    if (p->dir[0]) g_strlcpy(dir, p->dir, sizeof(dir));
    else ac_default_dir(dir, sizeof(dir));
    if (g_mkdir_with_parents(dir, 0755) != 0 || !g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        char msg[AUDIO_CAPTURE_TEXT_MAX];
        g_snprintf(msg, sizeof(msg), "Cannot create directory '%s'", dir);
        ac_error(err, err_size, msg);
        return false;
    }

    FILE *ev = NULL, *ps = NULL;
    st_AC_WAV ref = { NULL, 0 }, sdl = { NULL, 0 };
    bool ok = true;
    if (p->events) ok = ( ev = ac_csv_open(dir, "events.csv", "ticks,time_us,frame,channel,level\n") ) != NULL;
    if (ok && p->psg_writes) ok = ( ps = ac_csv_open(dir, "psg_writes.csv",
                                                     "ticks,time_us,frame,psg,byte,channel,reg,value\n") ) != NULL;
    if (ok && p->reference_wav) ok = ac_wav_open(&ref, dir, "reference.wav");
    if (ok && p->sdl_wav) ok = ac_wav_open(&sdl, dir, "sdl.wav");
    if (!ok) {
        ac_csv_close(&ev);
        ac_csv_close(&ps);
        ac_wav_close(&ref);
        ac_wav_close(&sdl);
        char msg[AUDIO_CAPTURE_TEXT_MAX];
        g_snprintf(msg, sizeof(msg), "Cannot create the capture files in '%s'", dir);
        ac_error(err, err_size, msg);
        return false;
    }

    s_ac.params = *p;
    g_strlcpy(s_ac.dir, dir, sizeof(s_ac.dir));
    s_ac.events = ev;
    s_ac.psg = ps;
    s_ac.ref = ref;
    s_ac.sdl = sdl;
    s_ac.sdl_blocks = 0;
    s_ac.sdl_writing = false;
    s_ac.warmup_frames = sdl.fp ? AC_SDL_WARMUP_FRAMES : 0;
    s_ac.frame = 0;
    s_ac.n_events = 0;
    s_ac.n_psg = 0;
    s_ac.stop_reason[0] = '\0';
    s_ac.last_error[0] = '\0';
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(s_ac.started, sizeof(s_ac.started), "%Y-%m-%dT%H:%M:%S", &tmv);
    s_ac.pending = true;
    s_ac.active = false;
    /* SDL cesta běží už během rozjezdu (bloky se zahazují do ac_begin()). */
    g_atomic_int_set(&g_audio_capture_flags,
                     AUDIO_CAPTURE_FLAG_ON | (sdl.fp ? AUDIO_CAPTURE_FLAG_SDL : 0));
    return true;
}


/** @brief audio_capture_stop() se zamčeným s_mutex. */
static bool ac_stop(void)
{
    if (!s_ac.pending && !s_ac.active) return false;
    ac_finish("Stopped");
    return true;
}


/** @brief audio_capture_get_status() se zamčeným s_mutex. */
static void ac_get_status(st_AUDIO_CAPTURE_STATUS *out)
{
    memset(out, 0, sizeof(*out));
    out->pending = s_ac.pending;
    out->active = s_ac.active;
    out->frames = s_ac.frame;
    out->events = s_ac.n_events;
    out->psg_writes = s_ac.n_psg;
    out->reference_samples = s_ac.ref.samples;
    out->sdl_blocks = s_ac.sdl_blocks;
    out->sdl_samples = s_ac.sdl.samples;
    g_strlcpy(out->dir, s_ac.dir, sizeof(out->dir));
    g_strlcpy(out->stop_reason, s_ac.stop_reason, sizeof(out->stop_reason));
    g_strlcpy(out->last_error, s_ac.last_error, sizeof(out->last_error));
}


/** @brief Zapíše řádek events.csv. */
static void ac_write_event(uint64_t rel, unsigned src_id, uint8_t value)
{
    if (!s_ac.events) return;
    char t[32];
    ac_time_us(rel, t, sizeof(t));
    fprintf(s_ac.events, "%llu,%s,%llu,%s,%u\n", (unsigned long long) rel, t,
            (unsigned long long) s_ac.frame, ac_channel_name(src_id), (unsigned) (value & 0x0F));
    s_ac.n_events++;
}


/** @brief audio_capture_on_change() se zamčeným s_mutex. */
static void ac_on_change(unsigned src_id, uint8_t value, uint64_t ticks)
{
    if (!s_ac.active) return;
    if (ticks < s_ac.origin) {
        ac_finish("Emulation timeline restarted (reset or snapshot load)");
        return;
    }
    ac_write_event(ticks - s_ac.origin, src_id, value);
    if (s_ac.ref.fp && src_id < AUDIO_SRC_CHANNELS_COUNT) {
        if (videorec_audio_event(&s_ac.renderer, src_id, value, ticks) != 0) {
            g_strlcpy(s_ac.last_error, "Reference renderer: out of memory", sizeof(s_ac.last_error));
        }
    }
}


/** @brief audio_capture_on_psg_write() se zamčeným s_mutex. */
static void ac_on_psg_write(uint64_t ticks, unsigned mask, uint8_t value)
{
#if HAVE_PSG == 0
    (void) ticks; (void) mask; (void) value;
#else
    if (!s_ac.active || !s_ac.psg) return;
    if (ticks < s_ac.origin) {
        ac_finish("Emulation timeline restarted (reset or snapshot load)");
        return;
    }
    /* Dekódování ze stavu cílového PSG po zápisu (při zápisu do obou PSG
     * ukazuje PSG0): latch_cs/latch_attn určují kanál a registr. */
    const st_PSG *psg = &g_psg_module.psg[(mask & 0x01) ? 0 : 1];
    unsigned cs = psg->latch_cs;
    const st_PSG_CHANNEL *ch = &psg->channel[cs];
    const char *reg;
    unsigned val;
    if (psg->latch_attn) {
        reg = "volume";
        val = (unsigned) ch->attn;
    } else if (ch->type == PSG_CHTYPE_TONE) {
        if (value & 0x80) {
            /* Latch bajt tónu nastaví jen spodní 4 bity, dělič se změní až datovým bajtem. */
            reg = "tone_lo";
            val = ch->tone.latch_divider;
        } else {
            reg = "tone";
            val = ch->tone.divider;
        }
    } else {
        reg = "noise";
        val = ((unsigned) ch->noise.type << 2) | (unsigned) ch->noise.div_type;
    }
    uint64_t rel = ticks - s_ac.origin;
    char t[32];
    ac_time_us(rel, t, sizeof(t));
    fprintf(s_ac.psg, "%llu,%s,%llu,%u,%02X,%u,%s,%u\n", (unsigned long long) rel, t,
            (unsigned long long) s_ac.frame, mask, (unsigned) value, cs, reg, val);
    s_ac.n_psg++;
#endif
}


/** @brief Začne naplánované zachycení v horizontu `horizon`. */
static void ac_begin(uint64_t horizon)
{
    s_ac.pending = false;
    s_ac.active = true;
    s_ac.origin = horizon;
    s_ac.last_horizon = horizon;
    s_ac.frame = 0;

    uint8_t values[AUDIO_SRC_CHANNELS_COUNT];
    for (unsigned i = 0; i < AUDIO_SRC_CHANNELS_COUNT; i++) {
        values[i] = (uint8_t) (g_audio.log ? (g_audio.log->src[i]->last_value & 0x0F) : 0);
        /* Výchozí úrovně všech kanálů, aby byl events.csv sám o sobě úplný. */
        ac_write_event(0, i, values[i]);
    }
    if (s_ac.ref.fp) {
        float level[VIDEOREC_AUDIO_MAX_CHANNELS][VIDEOREC_AUDIO_LEVELS];
        memset(level, 0, sizeof(level));
        iface_audio_build_videorec_levels(level, VIDEOREC_AUDIO_MAX_CHANNELS);
        /* Zahřátí: renderer začne AC_WARMUP_TICKS (100 ms) před začátkem se stejnými
         * úrovněmi a tyto vzorky zahodí. Trvalé úrovně (typicky CTC0 = 15)
         * tak začnou zaparkované jako v SDL cestě, ne naplno. Vzorky
         * reference.wav pak začínají přesně v `horizon`, protože
         * AC_WARMUP_TICKS je celý počet vzorků. */
        uint64_t warm = (horizon >= AC_WARMUP_TICKS) ? AC_WARMUP_TICKS : 0;
        videorec_audio_init(&s_ac.renderer, GDGCLK_BASE, AUDIO_CAPTURE_WAV_RATE, AUDIO_SRC_CHANNELS_COUNT,
                            (const float (*)[VIDEOREC_AUDIO_LEVELS]) level, values, horizon - warm, true);
        s_ac.renderer_init = true;
        if (warm) {
            int16_t discard[AC_RENDER_CHUNK * 2u];
            while (videorec_audio_render(&s_ac.renderer, horizon, discard, AC_RENDER_CHUNK) == AC_RENDER_CHUNK) {
            }
        }
    }
    s_ac.sdl_writing = (s_ac.sdl.fp != NULL);
}


/** @brief audio_capture_on_frame() se zamčeným s_mutex. */
static void ac_on_frame(uint64_t horizon_ticks)
{
    if (s_ac.pending) {
        if (s_ac.warmup_frames > 0) {
            s_ac.warmup_frames--;
            return;
        }
        ac_begin(horizon_ticks);
        return;
    }
    if (!s_ac.active) return;
    if (horizon_ticks < s_ac.last_horizon) {
        ac_finish("Emulation timeline restarted (reset or snapshot load)");
        return;
    }
    s_ac.last_horizon = horizon_ticks;

    if (s_ac.ref.fp) {
#if HAVE_PSG == 2
        videorec_audio_set_stereo(&s_ac.renderer, g_psg_module.stereo);
#endif
        int16_t buf[AC_RENDER_CHUNK * 2u];
        size_t n;
        do {
            n = videorec_audio_render(&s_ac.renderer, horizon_ticks, buf, AC_RENDER_CHUNK);
            ac_wav_write(&s_ac.ref, buf, n);
        } while (n == AC_RENDER_CHUNK);
    }

    s_ac.frame++;
    uint64_t limit = s_ac.params.frames ? s_ac.params.frames : AUDIO_CAPTURE_MAX_FRAMES_SAFETY;
    if ((s_ac.events && ferror(s_ac.events)) || (s_ac.psg && ferror(s_ac.psg))
        || (s_ac.ref.fp && ferror(s_ac.ref.fp))) {
        g_strlcpy(s_ac.last_error, "Write error (disk full?)", sizeof(s_ac.last_error));
        ac_finish("Error");
    } else if (s_ac.frame >= limit) {
        ac_finish(s_ac.params.frames ? "Frame count reached" : "Safety limit reached");
    }
}


void audio_capture_sdl_output(const float *stereo, size_t frames)
{
    if (!audio_capture_wants_sdl() || !stereo || frames == 0) return;
    int16_t buf[AC_RENDER_CHUNK * 2u];
    g_mutex_lock(&s_mutex);
    if (s_ac.sdl.fp && s_ac.sdl_writing) {
        size_t done = 0;
        while (done < frames) {
            size_t n = frames - done;
            if (n > AC_RENDER_CHUNK) n = AC_RENDER_CHUNK;
            for (size_t i = 0; i < n * 2u; i++) {
                float v = stereo[done * 2u + i] * 32767.0f;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                buf[i] = (int16_t) v;
            }
            ac_wav_write(&s_ac.sdl, buf, n);
            done += n;
        }
        s_ac.sdl_blocks++;
    }
    g_mutex_unlock(&s_mutex);
}


/** @brief Ukončí emulátor, pokud o to požádal ac_finish() (volat mimo zámek). */
static void ac_quit_if_requested(void)
{
    if (!s_quit_now) return;
    s_quit_now = false;
    fprintf(stderr, "[audio_capture] Requested frame count reached, quitting the emulator\n");
    emulator_quit(EXIT_SUCCESS); /* stejný kontext jako videorec --record-frames; nevrátí se */
}


bool audio_capture_start(const st_AUDIO_CAPTURE_PARAMS *p, char *err, size_t err_size)
{
    g_mutex_lock(&s_mutex);
    bool ok = ac_start(p, err, err_size);
    g_mutex_unlock(&s_mutex);
    return ok;
}


bool audio_capture_stop(void)
{
    g_mutex_lock(&s_mutex);
    bool ok = ac_stop();
    g_mutex_unlock(&s_mutex);
    return ok;
}


void audio_capture_get_status(st_AUDIO_CAPTURE_STATUS *out)
{
    g_mutex_lock(&s_mutex);
    ac_get_status(out);
    g_mutex_unlock(&s_mutex);
}


void audio_capture_on_change(unsigned src_id, uint8_t value, uint64_t ticks)
{
    g_mutex_lock(&s_mutex);
    ac_on_change(src_id, value, ticks);
    g_mutex_unlock(&s_mutex);
}


void audio_capture_on_psg_write(uint64_t ticks, unsigned mask, uint8_t value)
{
    g_mutex_lock(&s_mutex);
    ac_on_psg_write(ticks, mask, value);
    g_mutex_unlock(&s_mutex);
}


void audio_capture_on_frame(uint64_t horizon_ticks)
{
    g_mutex_lock(&s_mutex);
    ac_on_frame(horizon_ticks);
    g_mutex_unlock(&s_mutex);
    ac_quit_if_requested();
}


void audio_capture_exit(void)
{
    g_mutex_lock(&s_mutex);
    if (s_ac.pending || s_ac.active) ac_finish("Emulator exit");
    s_quit_now = false;
    g_mutex_unlock(&s_mutex);
}


bool audio_capture_resolve_output_dir(char *out, size_t size)
{
    if (g_audio_capture_settings.output_dir[0]) {
        g_strlcpy(out, g_audio_capture_settings.output_dir, size);
        (void) g_mkdir_with_parents(out, 0755);
        return g_file_test(out, G_FILE_TEST_IS_DIR);
    }
    if (g_sdlapp && g_sdlapp->paths && g_sdlapp->paths->home_dir) {
        char *dir = g_build_filename(g_sdlapp->paths->home_dir, "audio_captures", NULL);
        bool ok = (g_mkdir_with_parents(dir, 0755) == 0) && g_file_test(dir, G_FILE_TEST_IS_DIR);
        if (ok) g_strlcpy(out, dir, size);
        g_free(dir);
        if (ok) return true;
    }
    g_strlcpy(out, ".", size);
    return true;
}


void audio_capture_params_from_settings(st_AUDIO_CAPTURE_PARAMS *p)
{
    audio_capture_params_init(p);
    p->events = g_audio_capture_settings.events;
    p->psg_writes = g_audio_capture_settings.psg_writes;
    p->reference_wav = g_audio_capture_settings.reference_wav;
    p->sdl_wav = g_audio_capture_settings.sdl_wav;
}


/* ========================================================================= */
/*                     INI [AUDIO_CAPTURE] - výběr souborů                   */
/* ========================================================================= */

/** @brief Ukazatel na položku g_audio_capture_settings podle jména prvku INI. */
static bool *ac_setting_ptr(const char *name)
{
    if (!strcmp(name, "events")) return &g_audio_capture_settings.events;
    if (!strcmp(name, "psg_writes")) return &g_audio_capture_settings.psg_writes;
    if (!strcmp(name, "reference_wav")) return &g_audio_capture_settings.reference_wav;
    return &g_audio_capture_settings.sdl_wav;
}

/** @brief INI -> output_dir. @param e Prvek. @param data Nepoužito. */
static void ac_propagatecfg_dir(void *e, void *data)
{
    (void) data;
    char *dir = cfgelement_get_text_value((st_CFGELEMENT *) e);
    g_strlcpy(g_audio_capture_settings.output_dir, dir ? dir : "", sizeof(g_audio_capture_settings.output_dir));
}

/** @brief output_dir -> INI. @param e Prvek. @param data Nepoužito. */
static void ac_savecfg_dir(void *e, void *data)
{
    (void) data;
    cfgelement_set_text_value((st_CFGELEMENT *) e, g_audio_capture_settings.output_dir);
}

/** @brief INI -> nastavení. @param e Prvek. @param data Jméno prvku. */
static void ac_propagatecfg(void *e, void *data)
{
    *ac_setting_ptr((const char *) data) = cfgelement_get_bool_value((st_CFGELEMENT *) e) != 0;
}

/** @brief Nastavení -> INI. @param e Prvek. @param data Jméno prvku. */
static void ac_savecfg(void *e, void *data)
{
    cfgelement_set_bool_value((st_CFGELEMENT *) e, *ac_setting_ptr((const char *) data) ? 1 : 0);
}


void audio_capture_config_init(void)
{
    static const char *const names[4] = { "events", "psg_writes", "reference_wav", "sdl_wav" };
    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "AUDIO_CAPTURE");
    CFGELM *dir = cfgmodule_register_new_element(cmod, "output_dir", CFGENTYPE_TEXT, "");
    cfgelement_set_propagate_cb(dir, ac_propagatecfg_dir, NULL);
    cfgelement_set_save_cb(dir, ac_savecfg_dir, NULL);
    for (int i = 0; i < 4; i++) {
        CFGELM *elm = cfgmodule_register_new_element(cmod, (char *) names[i], CFGENTYPE_BOOL, 1);
        cfgelement_set_propagate_cb(elm, ac_propagatecfg, (void *) names[i]);
        cfgelement_set_save_cb(elm, ac_savecfg, (void *) names[i]);
    }
    /* Načíst sekci z INI a propagovat (vzor screenshot_config_init). */
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}
