/*
 * test_snapshot_mgr.c — unit testy pro snapshot manager
 *
 * Testuje: snapshot_register_component, snapshot_save, snapshot_load,
 *          snapshot_read_metadata, snapshot_result_to_string,
 *          snapshot_hash_components
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <string.h>

#include "emulator/snapshot/snapshot.h"
#include "emulator/snapshot/snapshot_mgr.h"
#include "emulator/snapshot/snapshot_io.h"
#include "emulator/emulator.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/pio8255/pio8255.h"
#include "hw-generic/gdg/gdg.h"
#include "mzarch/mzarch.h"

static const char *TMP_FILE = "tests/data/tmp/test_mgr.mzs";

void setUp(void) { }
void tearDown(void)
{
    remove(TMP_FILE);
}

/* ================================================================
 * SMOKE TESTY
 * ================================================================ */

/* snapshot_init registruje komponenty — ověřit, že registrace proběhla */
void test_mgr_components_registered(void)
{
    /* snapshot_init() se volá v mztest_init() —
     * ověříme, že se zaregistrovalo >0 komponent */
    TEST_ASSERT_GREATER_THAN(0, snapshot_mgr_get_component_count());
}

/* Komponenta "videorec" je registrovaná a v pořadí až za komponentou "audio"
 * (load "videorec" smí počítat s resetovaným audio logem). */
void test_videorec_component_registered(void)
{
    int n = snapshot_mgr_get_component_count();
    int i_audio = -1, i_vr = -1;
    for (int i = 0; i < n; i++) {
        const char *name = snapshot_mgr_get_component_name(i);
        TEST_ASSERT_NOT_NULL(name);
        if (strcmp(name, "audio") == 0) i_audio = i;
        if (strcmp(name, "videorec") == 0) i_vr = i;
    }
    TEST_ASSERT_NULL(snapshot_mgr_get_component_name(-1));
    TEST_ASSERT_NULL(snapshot_mgr_get_component_name(n));
    TEST_ASSERT_TRUE_MESSAGE(i_audio >= 0, "component 'audio' not registered");
    TEST_ASSERT_TRUE_MESSAGE(i_vr >= 0, "component 'videorec' not registered");
    TEST_ASSERT_GREATER_THAN(i_audio, i_vr);
}

/* Bez nahrávání snapshot neobsahuje videorec/state.bin. */
void test_videorec_state_absent_when_idle(void)
{
    g_emulator.paused = true;
    uint8_t *data = NULL;
    size_t size = 0;
    en_SNAPSHOT_RESULT res = snapshot_save_to_buffer("videorec idle", &data, &size);
    g_emulator.paused = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res, snapshot_result_to_string(res));

    snapshot_io_t *io = snapshot_io_open_read_buffer(data, size);
    TEST_ASSERT_NOT_NULL(io);
    TEST_ASSERT_TRUE(snapshot_io_entry_exists(io, "manifest.xml"));
    TEST_ASSERT_FALSE(snapshot_io_entry_exists(io, "videorec/state.bin"));
    snapshot_io_close(io);
    g_free(data);
}

/* snapshot_result_to_string vrací nenulový string pro každý kód */
void test_mgr_result_to_string(void)
{
    TEST_ASSERT_NOT_NULL(snapshot_result_to_string(SNAPSHOT_OK));
    TEST_ASSERT_NOT_NULL(snapshot_result_to_string(SNAPSHOT_ERR_IO));
    TEST_ASSERT_NOT_NULL(snapshot_result_to_string(SNAPSHOT_ERR_ZIP));
    TEST_ASSERT_NOT_NULL(snapshot_result_to_string(SNAPSHOT_ERR_NOT_PAUSED));
}

/* ================================================================
 * UNIT TESTY
 * ================================================================ */

/* save + load roundtrip — základní test celého pipeline */
void test_mgr_save_load_roundtrip(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* zapsat vzor do RAM */
    for (int i = 0; i < 256; i++) {
        g_memory.RAM[0x1000 + i] = (uint8_t)i;
    }

    /* uložit zálohu */
    uint8_t backup[256];
    memcpy(backup, &g_memory.RAM[0x1000], 256);

    /* emulace musí být v pauze pro save/load */
    g_emulator.paused = true;

    /* save */
    en_SNAPSHOT_RESULT res = snapshot_save(TMP_FILE, "test roundtrip");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res,
        snapshot_result_to_string(res));

    /* zničit data v RAM */
    memset(&g_memory.RAM[0x1000], 0xFF, 256);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_memory.RAM[0x1000]);

    /* load */
    res = snapshot_load(TMP_FILE);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res,
        snapshot_result_to_string(res));

    /* ověřit obnovení */
    TEST_ASSERT_EQUAL_MEMORY(backup, &g_memory.RAM[0x1000], 256);

    g_emulator.paused = false;
}

/* hash komponent: stejný stav = stejný hash, změna stavu = jiný hash,
 * pořadí jmen ani neznámé jméno výsledek nemění */
void test_mgr_hash_components(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *names[] = { "pio8255", "ctc8253" };
    const char *reversed[] = { "ctc8253", "pio8255", "no-such-component" };
    char h1[65], h2[65], h3[65];

    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_hash_components(names, 2, h1, sizeof(h1)));
    TEST_ASSERT_EQUAL_INT(64, (int)strlen(h1));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_hash_components(reversed, 3, h2, sizeof(h2)));
    TEST_ASSERT_EQUAL_STRING(h1, h2);

    uint8_t saved = g_pio8255.keyboard_matrix[3];
    g_pio8255.keyboard_matrix[3] = (uint8_t)(saved ^ 0x10);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_hash_components(names, 2, h3, sizeof(h3)));
    g_pio8255.keyboard_matrix[3] = saved;
    TEST_ASSERT_NOT_EQUAL(0, strcmp(h1, h3));

    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_hash_components(names, 2, h3, sizeof(h3)));
    TEST_ASSERT_EQUAL_STRING(h1, h3);

    /* malý výstup */
    char small[8];
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_ERR_IO, snapshot_hash_components(names, 2, small, sizeof(small)));
}


/* hash komponent nevidí stav řízení a vykreslení (for_hash), vidí výslednou
 * klávesovou matici bez ohledu na to, který zdroj klávesu drží */
void test_mgr_hash_components_canonical(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *names[] = { "mzarch", "gdg", "pio8255" };
    char base[65], h[65];
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_hash_components(names, 3, base, sizeof(base)));

    /* plánovaná událost hlavní smyčky (např. čekající pauza) */
    st_EMUEVENT saved_ev = g_mzarch_main.event;
    g_mzarch_main.event.event_name = MZEVENT_BREAK_EMULATION_PAUSED;
    g_mzarch_main.event.ticks = 0;
    /* průběh vykreslení obrazu */
    unsigned saved_r = g_gdg.screen_is_already_rendered_at_beam_pos;
    unsigned saved_b = g_gdg.last_updated_border_pixel;
    g_gdg.screen_is_already_rendered_at_beam_pos = saved_r + 77;
    g_gdg.last_updated_border_pixel = saved_b + 5;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_hash_components(names, 3, h, sizeof(h)));
    g_mzarch_main.event = saved_ev;
    g_gdg.screen_is_already_rendered_at_beam_pos = saved_r;
    g_gdg.last_updated_border_pixel = saved_b;
    TEST_ASSERT_EQUAL_STRING(base, h);

    /* klávesa ve vkbd_matrix (HID) = táž klávesa v keyboard_matrix */
    uint8_t saved_km = g_pio8255.keyboard_matrix[4];
    uint8_t saved_vk = g_pio8255.vkbd_matrix[4];
    g_pio8255.keyboard_matrix[4] = 0xFF;
    g_pio8255.vkbd_matrix[4] = 0x7F;
    char via_vkbd[65], via_kbd[65];
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_hash_components(names, 3, via_vkbd, sizeof(via_vkbd)));
    g_pio8255.keyboard_matrix[4] = 0x7F;
    g_pio8255.vkbd_matrix[4] = 0xFF;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_hash_components(names, 3, via_kbd, sizeof(via_kbd)));
    g_pio8255.keyboard_matrix[4] = saved_km;
    g_pio8255.vkbd_matrix[4] = saved_vk;
    TEST_ASSERT_EQUAL_STRING(via_vkbd, via_kbd);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(base, via_kbd));
}

/* save bez pauzy → ERR_NOT_PAUSED */
void test_mgr_save_requires_pause(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_emulator.paused = false;
    g_emulator.snapshot_safepoint = false;
    en_SNAPSHOT_RESULT res = snapshot_save(TMP_FILE, "should fail");
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_ERR_NOT_PAUSED, res);
}

/* 0019 KUS 2 - test (c): snapshot z pokračujícího BP přes dedikovaný
 * safe-point flag funguje i bez paused (= guard akceptuje snapshot_safepoint).
 * Toto je 0018 regrese-check: BP-action snapshot z continuing BP. */
void test_mgr_save_via_snapshot_safepoint(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* Emulace běží (= paused false), ale BP-action otevřel safe-point. */
    g_emulator.paused = false;
    g_emulator.snapshot_safepoint = true;

    en_SNAPSHOT_RESULT res = snapshot_save(TMP_FILE, "continuing BP snapshot");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res,
        snapshot_result_to_string(res));

    g_emulator.snapshot_safepoint = false;
}

/* 0019 KUS 2 - test (b): snapshot_safepoint je samostatný kanál a NEsmí
 * ovlivnit EMULATOR_TEST_PAUSED (= běhový wait-loop ho nevidí, actual_frames
 * tak zůstane správný i během snapshot-BP). */
void test_mgr_safepoint_does_not_set_paused(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_emulator.paused = false;
    g_emulator.snapshot_safepoint = true;

    /* Wait-loop v dispatch.c testuje EMULATOR_TEST_PAUSED (= g_emulator.paused),
     * ne snapshot_safepoint. Safe-point tedy nesmí "prosáknout" do paused. */
    TEST_ASSERT_FALSE(EMULATOR_TEST_PAUSED);
    TEST_ASSERT_TRUE(EMULATOR_TEST_SNAPSHOT_SAFEPOINT);

    g_emulator.snapshot_safepoint = false;
}

/* load bez pauzy → ERR_NOT_PAUSED */
void test_mgr_load_requires_pause(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* nejprve vytvořit validní snapshot */
    g_emulator.paused = true;
    snapshot_save(TMP_FILE, "for load test");

    /* zkusit load bez pauzy */
    g_emulator.paused = false;
    en_SNAPSHOT_RESULT res = snapshot_load(TMP_FILE);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_ERR_NOT_PAUSED, res);
}

/* load neexistujícího souboru → chyba */
void test_mgr_load_nonexistent(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_emulator.paused = true;
    en_SNAPSHOT_RESULT res = snapshot_load("tests/data/tmp/neexistuje.mzs");
    TEST_ASSERT_NOT_EQUAL(SNAPSHOT_OK, res);
    g_emulator.paused = false;
}

/* metadata — přečtení metadat z uloženého snapshotu */
void test_mgr_read_metadata(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    g_emulator.paused = true;
    en_SNAPSHOT_RESULT res = snapshot_save(TMP_FILE, "metadata test popis");
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, res);
    g_emulator.paused = false;

    st_SNAPSHOT_METADATA metadata;
    memset(&metadata, 0, sizeof(metadata));

    res = snapshot_read_metadata(TMP_FILE, &metadata);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, res);

    /* ověřit metadata */
    TEST_ASSERT_EQUAL_STRING("metadata test popis", metadata.description);
    TEST_ASSERT_EQUAL_INT(MZARCH, metadata.architecture);
    TEST_ASSERT_EQUAL_INT(1, metadata.format_version);

    /* checksum nesmí být prázdný */
    TEST_ASSERT_GREATER_THAN(0, strlen(metadata.checksum));
}

/* metadata z neexistujícího souboru → chyba */
void test_mgr_read_metadata_nonexistent(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    st_SNAPSHOT_METADATA metadata;
    en_SNAPSHOT_RESULT res = snapshot_read_metadata("tests/data/tmp/neexistuje.mzs", &metadata);
    TEST_ASSERT_NOT_EQUAL(SNAPSHOT_OK, res);
}

/* vícenásobný save→load */
void test_mgr_multiple_save_load(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_FULL);

    g_emulator.paused = true;

    for (int round = 0; round < 5; round++) {
        /* zapsat unikátní vzor */
        uint8_t pattern = (uint8_t)(round * 37 + 11);
        for (int i = 0; i < 64; i++) {
            g_memory.RAM[0x2000 + i] = pattern;
        }

        TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_save(TMP_FILE, "multi"));

        /* zničit */
        memset(&g_memory.RAM[0x2000], 0, 64);

        /* obnovit */
        TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_load(TMP_FILE));

        /* ověřit */
        TEST_ASSERT_EQUAL_HEX8(pattern, g_memory.RAM[0x2000]);
        TEST_ASSERT_EQUAL_HEX8(pattern, g_memory.RAM[0x2000 + 63]);
    }

    g_emulator.paused = false;
}

/* === MAIN === */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    /* smoke */
    RUN_TEST(test_mgr_components_registered);
    RUN_TEST(test_mgr_result_to_string);
    RUN_TEST(test_videorec_component_registered);
    RUN_TEST(test_videorec_state_absent_when_idle);

    /* unit */
    RUN_TEST(test_mgr_save_load_roundtrip);
    RUN_TEST(test_mgr_save_requires_pause);
    RUN_TEST(test_mgr_save_via_snapshot_safepoint);
    RUN_TEST(test_mgr_safepoint_does_not_set_paused);
    RUN_TEST(test_mgr_load_requires_pause);
    RUN_TEST(test_mgr_load_nonexistent);
    RUN_TEST(test_mgr_read_metadata);
    RUN_TEST(test_mgr_read_metadata_nonexistent);

    /* full */
    RUN_TEST(test_mgr_multiple_save_load);
    RUN_TEST(test_mgr_hash_components);
    RUN_TEST(test_mgr_hash_components_canonical);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
