/**
 * @file test_replay_rewind.c
 * @brief Unit testy checkpointů záznamu vstupů: rewind při záznamu a seek
 *        při přehrávání (replay.c) bez GUI a bez hlavní smyčky.
 *
 * Hlavní smyčku simuluje test jako v test_replay_input.c: pauza přímo
 * (`g_emulator.paused`), konec snímku = `g_gdg.total_elapsed.screens++`
 * a replay_on_frame_boundary(), paused smyčka = replay_on_paused().
 *
 * Pokrývá:
 *   - checkpointy vznikají na startu a každých N snímků (stav `checkpoints`),
 *   - rewind při záznamu vrátí stav stroje (RAM) i snímek, zahodí události
 *     po checkpointu, zvýší `rerecord_count` a záznam pokračuje; opakovaný
 *     rewind jde o další checkpoint zpět; uložený soubor má zkrácené
 *     události a `rerecord_count`,
 *   - rewind přes zápis na médium (g_media_writes) se odmítne a stav se nemění,
 *   - rewind o N snímků a odmítnutí bez checkpointu (vypnuté checkpointy),
 *   - seek při přehrávání dopředu dohraje v MAX SPEED a zastaví na cíli
 *     (důvod pauzy replay_seek, rychlost vrácená), zpět načte checkpoint,
 *     zpět za zápis na médium restartuje přehrávání od začátku; přehrávání
 *     pak skončí bez rozjetí.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "emulator/replay.h"
#include "emulator/replay_format.h"
#include "emulator/media_writes.h"
#include "emulator/emulator.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/memory/memory.h"

/** @brief Dočasný adresář testu. */
static char *s_tmpdir = NULL;

/** @brief Adresa RAM, kterou test mění jako "program". */
#define TEST_ADDR 0x2000

void setUp(void)
{
    if (!s_tmpdir) s_tmpdir = g_dir_make_tmp("replay_rw_XXXXXX", NULL);
    g_emulator.paused = true;
    emulator_max_speed(false);
}

void tearDown(void)
{
}


/** @brief Cesta souboru v dočasném adresáři (g_free). */
static char *tmp_path(const char *name)
{
    return g_build_filename(s_tmpdir, name, NULL);
}


/** @brief Konec snímku v simulované hlavní smyčce. */
static void next_frame(void)
{
    g_gdg.total_elapsed.screens++;
    replay_on_frame_boundary();
}


/** @brief Stav záznamu / přehrávání. */
static st_REPLAY_STATUS status(void)
{
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    return st;
}


/** @brief Stisk klávesy ve sloupci 4 (bit podle snímku), aby vznikaly události. */
static void press_for_frame(uint32_t f)
{
    uint8_t m[10];
    memset(m, 0xFF, sizeof(m));
    m[4] = (uint8_t)~(1u << (f % 8));
    replay_host_keyboard_submit(m, -1, 0xFF);
}


/** @brief Spustí záznam s checkpointy každé 2 snímky. */
static void start_recording(const char *path, int interval)
{
    st_REPLAY_CKPT_CFG cfg = { interval, 10 };
    pio8255_vkbd_matrix_reset();
    pio8255_keyboard_matrix_reset();
    replay_request_record_ex(path, 1, "rewind test", &cfg);
    replay_on_paused();
    st_REPLAY_STATUS st = status();
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_RECORDING, st.mode);
}


/* Rewind při záznamu: stav, snímek, události, rerecord_count, soubor. */
void test_rewind_recording(void)
{
    char *path = tmp_path("rw.mzr");
    g_memory.RAM[TEST_ADDR] = 0;
    start_recording(path, 2);
    TEST_ASSERT_EQUAL_UINT32(1, status().checkpoints);

    /* Snímky 1-5: "program" mění RAM, klávesy dávají události. */
    for (uint32_t f = 1; f <= 5; f++)
    {
        press_for_frame(f);
        g_memory.RAM[TEST_ADDR] = (uint8_t)(0x10 + f - 1);
        next_frame();
    };
    st_REPLAY_STATUS st = status();
    TEST_ASSERT_EQUAL_UINT32(5, st.frame);
    TEST_ASSERT_EQUAL_UINT32(3, st.checkpoints); /* 0, 2, 4 */
    TEST_ASSERT_EQUAL_UINT32(0, st.checkpoint_first);
    TEST_ASSERT_EQUAL_UINT32(4, st.checkpoint_last);
    TEST_ASSERT_TRUE(st.checkpoint_bytes > 0);
    uint32_t events_at_5 = st.events;
    g_memory.RAM[TEST_ADDR] = 0x99; /* změna po snímku 5 */

    /* Rewind o jeden checkpoint: snímek 4, RAM jako na hranici 4. */
    replay_request_rewind(0);
    replay_on_paused();
    st = status();
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_UINT32(4, replay_current_frame());
    TEST_ASSERT_EQUAL_HEX8(0x13, g_memory.RAM[TEST_ADDR]);
    TEST_ASSERT_TRUE(replay_paused_at_boundary());
    TEST_ASSERT_EQUAL_UINT32(1, st.rerecord_count);
    TEST_ASSERT_TRUE(st.events < events_at_5);
    TEST_ASSERT_TRUE(g_emulator.paused);

    /* Další rewind o jeden checkpoint: snímek 2. */
    replay_request_rewind(0);
    replay_on_paused();
    st = status();
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_UINT32(2, replay_current_frame());
    TEST_ASSERT_EQUAL_HEX8(0x11, g_memory.RAM[TEST_ADDR]);
    TEST_ASSERT_EQUAL_UINT32(2, st.rerecord_count);
    TEST_ASSERT_EQUAL_UINT32(2, st.checkpoints); /* 0, 2 */

    /* Záznam pokračuje jinou cestou. */
    press_for_frame(7);
    next_frame();
    TEST_ASSERT_EQUAL_UINT32(3, replay_current_frame());

    /* Zápis na médium: rewind za něj se odmítne, stav beze změny. */
    g_media_writes++;
    replay_request_rewind(0);
    replay_on_paused();
    st = status();
    TEST_ASSERT_FALSE(st.request_ok);
    TEST_ASSERT_NOT_NULL(strstr(st.request_message, "written"));
    TEST_ASSERT_EQUAL_UINT32(3, replay_current_frame());
    TEST_ASSERT_EQUAL_UINT32(2, st.rerecord_count);

    replay_request_stop();
    replay_on_paused();
    TEST_ASSERT_EQUAL_INT(REPLAY_OUTCOME_RECORDED, status().outcome);

    char *err = NULL;
    st_REPLAY_DATA *d = replay_data_read(path, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, err ? err : "");
    TEST_ASSERT_EQUAL_UINT32(2, d->rerecord_count);
    TEST_ASSERT_EQUAL_UINT32(3, d->frames);
    for (guint i = 0; i < d->events->len; i++)
    {
        TEST_ASSERT_TRUE(g_array_index(d->events, st_REPLAY_EVENT, i).frame <= 3);
    };
    for (guint i = 0; i < d->hashes->len; i++)
    {
        TEST_ASSERT_TRUE(g_array_index(d->hashes, st_REPLAY_HASH_ROW, i).frame <= 3);
    };
    replay_data_free(d);
    g_free(err);
    g_remove(path);
    g_free(path);
}


/* Rewind o N snímků a bez checkpointů. */
void test_rewind_frames_and_off(void)
{
    char *path = tmp_path("rw2.mzr");
    start_recording(path, 2);
    for (uint32_t f = 1; f <= 7; f++) next_frame();
    /* Aspoň o 4 snímky zpět ze 7 = nejnovější checkpoint <= 3 = 2. */
    replay_request_rewind(4);
    replay_on_paused();
    TEST_ASSERT_TRUE_MESSAGE(status().request_ok, status().request_message);
    TEST_ASSERT_EQUAL_UINT32(2, replay_current_frame());
    replay_request_stop();
    replay_on_paused();

    start_recording(path, 0);
    TEST_ASSERT_EQUAL_UINT32(0, status().checkpoints);
    next_frame();
    replay_request_rewind(0);
    replay_on_paused();
    st_REPLAY_STATUS st = status();
    TEST_ASSERT_FALSE(st.request_ok);
    TEST_ASSERT_EQUAL_STRING("checkpoints are off", st.request_message);
    replay_request_stop();
    replay_on_paused();
    g_remove(path);
    g_free(path);
}


/* Seek při přehrávání: dopředu, zpět přes checkpoint, zpět přes restart. */
void test_seek_playback(void)
{
    char *path = tmp_path("seek.mzr");
    start_recording(path, 3);
    for (uint32_t f = 1; f <= 10; f++)
    {
        press_for_frame(f);
        next_frame();
    };
    replay_request_stop();
    replay_on_paused();
    TEST_ASSERT_EQUAL_INT(REPLAY_OUTCOME_RECORDED, status().outcome);

    st_REPLAY_CKPT_CFG cfg = { 3, 10 };
    replay_request_play_ex(path, true, &cfg);
    replay_on_paused();
    st_REPLAY_STATUS st = status();
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_EQUAL_INT(REPLAY_MODE_PLAYING, st.mode);

    /* Dopředu na 7: dohrávání v MAX SPEED. */
    replay_request_seek(7);
    replay_on_paused();
    st = status();
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_TRUE(st.seeking);
    TEST_ASSERT_EQUAL_UINT32(7, st.seek_target);
    TEST_ASSERT_FALSE(g_emulator.paused);
    TEST_ASSERT_TRUE(g_emulator.max_speed);
    while (!g_emulator.paused) next_frame();
    TEST_ASSERT_EQUAL_UINT32(7, replay_current_frame());
    TEST_ASSERT_EQUAL_INT(EMU_PAUSE_REASON_REPLAY_SEEK, g_emulator.pause_reason);
    TEST_ASSERT_FALSE(g_emulator.max_speed);
    TEST_ASSERT_FALSE(status().seeking);
    TEST_ASSERT_EQUAL_UINT32(3, status().checkpoints); /* 0, 3, 6 */

    /* Zpět na 6 = přesně checkpoint, bez dohrávání. */
    replay_request_seek(6);
    replay_on_paused();
    st = status();
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_FALSE(st.seeking);
    TEST_ASSERT_EQUAL_UINT32(6, replay_current_frame());
    TEST_ASSERT_TRUE(g_emulator.paused);

    /* Zpět na 4 po zápisu na médium: restart od začátku a dohrání. */
    g_media_writes++;
    replay_request_seek(4);
    replay_on_paused();
    st = status();
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_TRUE(st.seeking);
    while (!g_emulator.paused) next_frame();
    TEST_ASSERT_EQUAL_UINT32(4, replay_current_frame());

    /* Dohrát do konce: bez rozjetí. */
    g_emulator.paused = false;
    while (status().mode == REPLAY_MODE_PLAYING) next_frame();
    st = status();
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);
    g_emulator.paused = true;
    g_remove(path);
    g_free(path);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_rewind_recording);
    RUN_TEST(test_rewind_frames_and_off);
    RUN_TEST(test_seek_playback);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
