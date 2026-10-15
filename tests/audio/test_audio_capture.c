/**
 * @file   test_audio_capture.c
 * @brief  Unit testy zachycení zvuku (audio_capture.h) nad emu core.
 *
 * Háčky se volají přímo, jako je volá zvuková cesta: audio_capture_on_frame()
 * na konci zvukového snímku, audio_capture_on_change() při změně kanálu,
 * audio_capture_on_psg_write() po zápisu do PSG a audio_capture_sdl_output()
 * s blokem SDL cesty.
 *
 * Ověřuje:
 * - naplánovaný start začne až na konci snímku, rozjezd SDL cesty se sdl.wav,
 * - obsah events.csv a psg_writes.csv (relativní takty, snímek, dekódování),
 * - hlavičky WAV a počty vzorků, bloky SDL během rozjezdu se zahodí,
 * - konec po `frames`, při skoku času zpět a stopem; capture.json,
 * - chyby startu (už běží, žádný výstup, nelze vytvořit složku).
 *
 * @par Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "audio_capture.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/psg/psg.h"

/** @brief Dočasná složka testu. */
static char *s_dir;
/** @brief Délka snímku v taktech GDG. */
static const uint64_t T_FRAME = (uint64_t) GDGCLK_BASE / VIDEO_SCREENS_PER_SEC;
/** @brief Začátek zachycení v testech (větší než zahřátí rendereru 100 ms). */
static const uint64_t T0 = (uint64_t) GDGCLK_BASE;


/** @brief Smaže soubory ve složce a složku. */
static void rm_dir(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *n;
    while ((n = g_dir_read_name(d)) != NULL) {
        char *p = g_build_filename(dir, n, NULL);
        g_remove(p);
        g_free(p);
    }
    g_dir_close(d);
    g_rmdir(dir);
}


/** @brief Obsah souboru ve složce zachycení (volající uvolní g_free). */
static char *read_file(const char *name, gsize *len)
{
    char *p = g_build_filename(s_dir, name, NULL);
    char *text = NULL;
    TEST_ASSERT_TRUE_MESSAGE(g_file_get_contents(p, &text, len, NULL), name);
    g_free(p);
    return text;
}


/** @brief Počet řádků textu (bez hlavičky CSV). */
static unsigned data_lines(const char *text)
{
    unsigned n = 0;
    for (const char *c = strchr(text, '\n'); c && c[1]; c = strchr(c + 1, '\n')) n++;
    return n;
}


/** @brief Parametry s danými výstupy do s_dir. */
static st_AUDIO_CAPTURE_PARAMS params(bool sdl, uint64_t frames)
{
    st_AUDIO_CAPTURE_PARAMS p;
    audio_capture_params_init(&p);
    g_strlcpy(p.dir, s_dir, sizeof(p.dir));
    p.sdl_wav = sdl;
    p.frames = frames;
    return p;
}


void setUp(void)
{
    s_dir = g_dir_make_tmp("mztest-audiocap-XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL(s_dir);
}


void tearDown(void)
{
    audio_capture_stop();
    rm_dir(s_dir);
    g_free(s_dir);
}


/** @brief Start bez sdl.wav začne na konci nejbližšího snímku; CSV s relativními časy. */
void test_audio_capture_start_events_psg(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    char err[128];
    TEST_ASSERT_TRUE(audio_capture_start(&p, err, sizeof(err)));
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    TEST_ASSERT_TRUE(st.pending);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_TRUE(audio_capture_wanted());
    TEST_ASSERT_FALSE(audio_capture_wants_sdl());

    /* Změna před začátkem se nezapíše. */
    audio_capture_on_change(0, 15, T0 - 10);
    audio_capture_on_frame(T0);
    audio_capture_get_status(&st);
    TEST_ASSERT_TRUE(st.active);
    TEST_ASSERT_EQUAL_UINT64(AUDIO_SRC_CHANNELS_COUNT, st.events); /* výchozí úrovně */

    audio_capture_on_change(1, 13, T0 + 100);
    /* Zápis hlasitosti kanálu 1 PSG0: 1011 0010 = latch, kanál 1, attn, 2. */
    g_psg_module.psg[0].latch_cs = 1;
    g_psg_module.psg[0].latch_attn = 1;
    g_psg_module.psg[0].channel[1].attn = 2;
    audio_capture_on_psg_write(T0 + 200, PSG_CH_LEFT, 0xB2);
    audio_capture_on_frame(T0 + T_FRAME);
    audio_capture_on_change(0, 0, T0 + T_FRAME + 5);
    TEST_ASSERT_TRUE(audio_capture_stop());

    audio_capture_get_status(&st);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_EQUAL_STRING("Stopped", st.stop_reason);
    TEST_ASSERT_EQUAL_UINT64(1, st.frames);
    TEST_ASSERT_EQUAL_UINT64(AUDIO_SRC_CHANNELS_COUNT + 2, st.events);
    TEST_ASSERT_EQUAL_UINT64(1, st.psg_writes);

    char *ev = read_file("events.csv", NULL);
    TEST_ASSERT_EQUAL_UINT(AUDIO_SRC_CHANNELS_COUNT + 2, data_lines(ev));
    TEST_ASSERT_NOT_NULL(strstr(ev, "ticks,time_us,frame,channel,level\n0,0.000,0,ctc0,0\n"));
    TEST_ASSERT_NOT_NULL(strstr(ev, "\n100,"));
    TEST_ASSERT_NOT_NULL(strstr(ev, ",0,psg0_0,13\n"));
    char line[64];
    g_snprintf(line, sizeof(line), "\n%llu,", (unsigned long long) (T_FRAME + 5));
    TEST_ASSERT_NOT_NULL(strstr(ev, line));
    TEST_ASSERT_NOT_NULL(strstr(ev, ",1,ctc0,0\n"));
    g_free(ev);

    char *ps = read_file("psg_writes.csv", NULL);
    TEST_ASSERT_NOT_NULL(strstr(ps, "ticks,time_us,frame,psg,byte,channel,reg,value\n200,"));
    TEST_ASSERT_NOT_NULL(strstr(ps, ",0,1,B2,1,volume,2\n"));
    g_free(ps);

    char *js = read_file("capture.json", NULL);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"stop_reason\": \"Stopped\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"frames\": 1,"));
    g_free(js);
}


/** @brief reference.wav: hlavička PCM 44100 Hz stereo, vzorky za celý snímek. */
void test_audio_capture_reference_wav(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 2);
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    audio_capture_on_frame(T0);
    audio_capture_on_change(2, 15, T0 + 1000);
    audio_capture_on_frame(T0 + T_FRAME);
    audio_capture_on_frame(T0 + 2 * T_FRAME);  /* frames = 2 => konec */
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_EQUAL_STRING("Frame count reached", st.stop_reason);
    uint64_t per_frame = AUDIO_CAPTURE_WAV_RATE / VIDEO_SCREENS_PER_SEC;
    /* Renderer zapíše úplné vzorky do horizontu (případně o 1 méně kvůli zaokrouhlení hranice). */
    TEST_ASSERT_UINT64_WITHIN(1, 2 * per_frame, st.reference_samples);

    gsize len = 0;
    char *wav = read_file("reference.wav", &len);
    TEST_ASSERT_EQUAL_UINT64(44 + st.reference_samples * 4, len);
    TEST_ASSERT_EQUAL_MEMORY("RIFF", wav, 4);
    TEST_ASSERT_EQUAL_MEMORY("WAVEfmt ", wav + 8, 8);
    TEST_ASSERT_EQUAL_UINT8(2, (uint8_t) wav[22]);                    /* stereo */
    uint32_t rate = (uint8_t) wav[24] | ((uint8_t) wav[25] << 8) | ((uint32_t) (uint8_t) wav[26] << 16);
    TEST_ASSERT_EQUAL_UINT32(AUDIO_CAPTURE_WAV_RATE, rate);
    uint32_t data = (uint8_t) wav[40] | ((uint8_t) wav[41] << 8) | ((uint32_t) (uint8_t) wav[42] << 16);
    TEST_ASSERT_EQUAL_UINT32(st.reference_samples * 4, data);
    /* Kanál psg0_1 na úrovni 15 => nenulový signál ve druhém snímku. */
    bool nonzero = false;
    for (gsize i = 44 + per_frame * 4; i < len; i++) if (wav[i]) { nonzero = true; break; }
    TEST_ASSERT_TRUE(nonzero);
    g_free(wav);
}


/** @brief sdl.wav: rozjezd 6 snímků, bloky během něj se zahodí, pak se zapisují. */
void test_audio_capture_sdl_warmup(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(true, 0);
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    TEST_ASSERT_TRUE(audio_capture_wants_sdl());
    float block[2 * 10];
    for (int i = 0; i < 20; i++) block[i] = 0.5f;

    st_AUDIO_CAPTURE_STATUS st;
    for (int f = 0; f < 6; f++) {
        audio_capture_on_frame(T0 + (uint64_t) f * T_FRAME);
        audio_capture_sdl_output(block, 10);
        audio_capture_get_status(&st);
        TEST_ASSERT_TRUE(st.pending);
    }
    TEST_ASSERT_EQUAL_UINT64(0, st.sdl_blocks);
    audio_capture_on_frame(T0 + 6 * T_FRAME);
    audio_capture_sdl_output(block, 10);
    audio_capture_get_status(&st);
    TEST_ASSERT_TRUE(st.active);
    TEST_ASSERT_EQUAL_UINT64(1, st.sdl_blocks);
    TEST_ASSERT_EQUAL_UINT64(10, st.sdl_samples);
    TEST_ASSERT_TRUE(audio_capture_stop());
    TEST_ASSERT_FALSE(audio_capture_wants_sdl());
    audio_capture_sdl_output(block, 10);   /* po stop se nic nezapíše */

    gsize len = 0;
    char *wav = read_file("sdl.wav", &len);
    TEST_ASSERT_EQUAL_UINT64(44 + 10 * 4, len);
    int16_t s0 = (int16_t) ((uint8_t) wav[44] | ((uint8_t) wav[45] << 8));
    TEST_ASSERT_INT_WITHIN(1, 16383, s0);
    g_free(wav);
}


/** @brief Skok času zpět (reset, snapshot) zachycení ukončí. */
void test_audio_capture_timeline_restart(void)
{
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    TEST_ASSERT_TRUE(audio_capture_start(&p, NULL, 0));
    audio_capture_on_frame(T0);
    audio_capture_on_frame(T0 + T_FRAME);
    audio_capture_on_frame(1000);
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_FALSE(audio_capture_wanted());
    TEST_ASSERT_NOT_NULL(strstr(st.stop_reason, "timeline restarted"));
    TEST_ASSERT_FALSE(audio_capture_stop());
}


/** @brief Chyby startu: už běží, žádný výstup, nelze vytvořit složku. */
void test_audio_capture_start_errors(void)
{
    char err[AUDIO_CAPTURE_TEXT_MAX];
    st_AUDIO_CAPTURE_PARAMS p = params(false, 0);
    TEST_ASSERT_TRUE(audio_capture_start(&p, err, sizeof(err)));
    TEST_ASSERT_FALSE(audio_capture_start(&p, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "already running"));
    TEST_ASSERT_TRUE(audio_capture_stop());

    st_AUDIO_CAPTURE_PARAMS none = params(false, 0);
    none.events = none.psg_writes = none.reference_wav = false;
    TEST_ASSERT_FALSE(audio_capture_start(&none, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "No output"));

    /* Složka uvnitř souboru nejde vytvořit. */
    char *file = g_build_filename(s_dir, "afile", NULL);
    TEST_ASSERT_TRUE(g_file_set_contents(file, "x", 1, NULL));
    st_AUDIO_CAPTURE_PARAMS bad = params(false, 0);
    char *sub = g_build_filename(file, "sub", NULL);
    g_strlcpy(bad.dir, sub, sizeof(bad.dir));
    TEST_ASSERT_FALSE(audio_capture_start(&bad, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "Cannot create directory"));
    TEST_ASSERT_FALSE(audio_capture_wanted());
    g_free(sub);
    g_free(file);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_audio_capture_start_events_psg);
    RUN_TEST(test_audio_capture_reference_wav);
    RUN_TEST(test_audio_capture_sdl_warmup);
    RUN_TEST(test_audio_capture_timeline_restart);
    RUN_TEST(test_audio_capture_start_errors);
    return UNITY_END();
}
