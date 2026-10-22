/**
 * @file test_replay_media.c
 * @brief Unit testy médií v záznamu vstupů (replay_media.c) bez GUI.
 *
 * Hlavní smyčku simuluje test jako test_replay_input.c (pauza přímo,
 * konec snímku = zvýšení `g_gdg.total_elapsed.screens` a volání
 * replay_on_frame_boundary()).
 *
 * Pokrývá:
 *   - záznam uloží obsah nasazeného DSK z paměti (i se změnou, která ještě
 *     není v souboru) a obsah disku IDE8,
 *   - přehrání nasadí kopie ze záznamu (DSK v režimu discard, IDE8 nad
 *     dočasným souborem), zápisy přehrání se do originálů nedostanou,
 *     cesta v INI se nezmění,
 *   - mechanika bez obrazu v záznamu je při přehrání bez obrazu a po konci
 *     přehrání se vrátí obraz podle INI,
 *   - Quick Disk: obraz z paměti (discard, i u odpojené mechaniky), kopie
 *     obsahu a zavření bez zápisu do INI,
 *   - operace kazety z oken (replay_host_cmt()): mimo záznam je provede
 *     paused smyčka, při záznamu se zapíší jako události `cmt` s polohou
 *     `p` (pauza na hranici) nebo `b` (hranice snímku za běhu), přehrání
 *     vloží kopii pásky ze záznamu a operace zopakuje.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "emulator/replay.h"
#include "emulator/replay_format.h"
#include "emulator/emulator.h"
#include "hw-generic/gdg/gdg.h"
#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "hw-generic/fdc/fdc.h"
#include "hw-generic/ide8/ide8.h"
#include "hw-generic/qdisk/qdisk.h"
#include "hw-generic/cmt/cmt.h"
#include "fs_layer.h"
#include "libs/dsk/dsk_tools.h"
#include "generic_driver/memory_driver.h"
#include "emulator/media_writes.h"

/** @brief Dočasný adresář testu. */
static char *s_tmpdir = NULL;

void setUp(void)
{
    g_emulator.paused = true;
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


/** @brief Zapíše malý MZ-BASIC DSK (2 stopy, 16 x 256 B) do souboru. */
static void write_test_dsk(const char *path)
{
    st_HANDLER h;
    memset(&h, 0, sizeof(h));
    TEST_ASSERT_NOT_NULL(generic_driver_open_memory(&h, &g_memory_driver_realloc, 1024));
    size_t desc_size = dsk_tools_compute_description_size(1);
    st_DSK_DESCRIPTION *desc = (st_DSK_DESCRIPTION *)g_malloc0(desc_size);
    desc->tracks = 2;
    desc->sides = 1;
    desc->count_rules = 1;
    dsk_tools_assign_description(desc, 0, 0, 16, DSK_SECTOR_SIZE_256, DSK_SEC_ORDER_NORMAL, NULL, 0xFF);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, dsk_tools_create_image(&h, desc));
    g_free(desc);
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const char *)h.spec.memspec.ptr, (gssize)h.spec.memspec.size, NULL));
    generic_driver_close(&h);
}


/** @brief Cesta obrazu mechaniky FDC0 v INI (klíč `wd279x_fddN_dskpath`). */
static const char *cfg_dskpath(unsigned drive)
{
    CFGMOD *cmod = cfgroot_get_module_by_name(g_cfgmain, (char *)fdc_cfg_section_name(&g_fdc[0]));
    TEST_ASSERT_NOT_NULL(cmod);
    return cfgmodule_get_element_text_value_by_name(cmod, (char *)fdc_dskpath_keyname(&g_fdc[0], drive));
}


/** @brief Ukazatel na paměťový obraz mechaniky (musí být cached / discard). */
static uint8_t *drive_mem(unsigned drive)
{
    st_FDDrive *drv = &g_fdc[0].drive[drive];
    TEST_ASSERT_TRUE(drv->mounted);
    TEST_ASSERT_EQUAL_INT(HANDLER_TYPE_MEMORY, drv->handler.type);
    return drv->handler.spec.memspec.ptr;
}


/** @brief "Zápis programu" do obrazu: bajt + příznak změny (jako WRITE SECTOR). */
static void drive_poke(unsigned drive, size_t off, uint8_t value)
{
    drive_mem(drive)[off] = value;
    g_fdc[0].drive[drive].handler.spec.memspec.updated = 1;
}


/** @brief Bajt souboru na pozici @p off. */
static uint8_t file_byte(const char *path, size_t off)
{
    gchar *data = NULL;
    gsize len = 0;
    TEST_ASSERT_TRUE(g_file_get_contents(path, &data, &len, NULL));
    TEST_ASSERT_TRUE(off < len);
    uint8_t b = (uint8_t)data[off];
    g_free(data);
    return b;
}


/** @brief Zapíše bajt na pozici @p off disku IDE8 (přes jeho otevřený soubor). */
static void ide8_poke(int drive, uint32_t off, uint8_t value)
{
    st_IDE8_DRIVE *d = &g_ide8.drive[drive];
    TEST_ASSERT_TRUE(d->connected == IDE8_STATE_CONNECTED);
    FS_LAYER_FSEEK(d->fp, off);
    unsigned wl = 0;
    TEST_ASSERT_EQUAL_INT(FS_LAYER_FR_OK, FS_LAYER_FWRITE(d->fp, &value, 1, &wl));
    FS_LAYER_FSYNC(d->fp);
}


/** @brief Bajt na pozici @p off disku IDE8. */
static uint8_t ide8_peek(int drive, uint32_t off)
{
    st_IDE8_DRIVE *d = &g_ide8.drive[drive];
    FS_LAYER_FSEEK(d->fp, off);
    uint8_t value = 0;
    unsigned rl = 0;
    TEST_ASSERT_EQUAL_INT(FS_LAYER_FR_OK, FS_LAYER_FREAD(d->fp, &value, 1, &rl));
    return value;
}


/* Záznam uloží obsah DSK a IDE8, přehrání pracuje nad kopiemi. */
void test_media_recorded_and_played_on_copies(void)
{
    char *dsk = tmp_path("a.dsk");
    char *dsk2 = tmp_path("b.dsk");
    char *hdd = tmp_path("hdd.img");
    char *mzr = tmp_path("media.mzr");
    write_test_dsk(dsk);
    write_test_dsk(dsk2);
    uint8_t zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    TEST_ASSERT_TRUE(g_file_set_contents(hdd, (const char *)zeros, sizeof(zeros), NULL));

    const size_t off = 0x200; /* uvnitř obrazu, mimo hlavičku disku */
    fdc_cfg_set_storage_mode(&g_fdc[0], 1, "cached");
    fdc_mount_dskfile(&g_fdc[0], 1, dsk);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[0], 1));
    uint8_t orig = drive_mem(1)[off];
    /* Změna jen v paměti (cached, ještě ne v souboru). */
    drive_poke(1, off, (uint8_t)(orig ^ 0x5A));

    /* Disk připojený podle INI (hdd0_filepath), jako při startu emulátoru. */
    st_IDE8_DRIVE *hd = &g_ide8.drive[0];
    char *saved_filepath = hd->filepath;
    hd->filepath = g_strdup(hdd);
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, ide8_drive_open_image(hd, hd->filepath));
    ide8_poke(0, 100, 0x11);

    /* --- záznam --- */
    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);

    /* "Zápis programu" během záznamu - do záznamu se už nedostane. */
    drive_poke(1, off, 0xC3);
    ide8_poke(0, 100, 0x22);
    next_frame();
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    char *err = NULL;
    st_REPLAY_DATA *d = replay_data_read(mzr, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, err ? err : "");
    const st_REPLAY_MEDIA *m = replay_data_find_media(d, "fdc0.1");
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(m->data);
    TEST_ASSERT_EQUAL_HEX8(orig ^ 0x5A, m->data[off]);
    TEST_ASSERT_EQUAL_STRING(dsk, m->name);
    m = replay_data_find_media(d, "ide8.0");
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT64(sizeof(zeros), m->size);
    TEST_ASSERT_EQUAL_HEX8(0x11, m->data[100]);
    TEST_ASSERT_NULL(replay_data_find_media(d, "fdc0.2"));
    replay_data_free(d);
    g_free(err);

    /* Před přehráním: do mechaniky 2 jiný obraz (v záznamu nebyl). */
    fdc_mount_dskfile(&g_fdc[0], 2, dsk2);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[0], 2));

    /* --- přehrání --- */
    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    /* Obsah ze záznamu (stav při startu), discard, INI beze změny. */
    TEST_ASSERT_EQUAL_HEX8(orig ^ 0x5A, drive_mem(1)[off]);
    TEST_ASSERT_EQUAL_INT(FDC_STORAGE_DISCARD, g_fdc[0].drive[1].storage_mode);
    TEST_ASSERT_EQUAL_STRING(dsk, cfg_dskpath(1));
    TEST_ASSERT_FALSE(fdc_test_drive_id_mounted(&g_fdc[0], 2));
    TEST_ASSERT_EQUAL_HEX8(0x11, ide8_peek(0, 100));

    /* Zápisy přehrání. */
    drive_poke(1, off, 0x99);
    ide8_poke(0, 100, 0x33);
    next_frame();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);

    /* Po konci: originály podle INI, zápisy přehrání v nich nejsou. Změny
     * ze záznamu (cached) se při odpojení před přehráním zapsaly. */
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[0], 1));
    TEST_ASSERT_EQUAL_STRING(dsk, g_fdc[0].drive[1].filename);
    TEST_ASSERT_EQUAL_INT(FDC_STORAGE_CACHED, g_fdc[0].drive[1].storage_mode);
    TEST_ASSERT_EQUAL_HEX8(0xC3, file_byte(dsk, off));
    TEST_ASSERT_EQUAL_HEX8(0xC3, drive_mem(1)[off]);
    TEST_ASSERT_TRUE(fdc_test_drive_id_mounted(&g_fdc[0], 2));
    TEST_ASSERT_EQUAL_STRING(dsk2, g_fdc[0].drive[2].filename);
    TEST_ASSERT_EQUAL_HEX8(0x22, ide8_peek(0, 100));
    TEST_ASSERT_EQUAL_HEX8(0x22, file_byte(hdd, 100));

    fdc_umount(&g_fdc[0], 1);
    fdc_umount(&g_fdc[0], 2);
    ide8_drive_close_image(hd);
    g_free(hd->filepath);
    hd->filepath = saved_filepath;
    g_remove(dsk);
    g_remove(dsk2);
    g_remove(hdd);
    g_remove(mzr);
    g_free(dsk);
    g_free(dsk2);
    g_free(hdd);
    g_free(mzr);
}


/* Zápis sektoru IDE8 přes registry zvýší počítadlo zápisů na média
 * (checkpoint pak nejde použít); čtení ho nezmění. */
void test_media_writes_counted_ide8(void)
{
    char *hdd = tmp_path("cnt.img");
    uint8_t zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    TEST_ASSERT_TRUE(g_file_set_contents(hdd, (const char *)zeros, sizeof(zeros), NULL));
    st_IDE8_DRIVE *hd = &g_ide8.drive[0];
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, ide8_drive_open_image(hd, hdd));

    /* Sektor 1 adresovaný LBA, jeden, master. */
    ide8_write_byte(IDE8_ADDR_HEAD, (uint8_t)(0xA0 | IDE8_HEAD_BUSMODEMASK));
    ide8_write_byte(IDE8_ADDR_SCOUNT, 1);
    ide8_write_byte(IDE8_ADDR_SECTOR, 1);
    ide8_write_byte(IDE8_ADDR_LCYL, 0);
    ide8_write_byte(IDE8_ADDR_HCYL, 0);
    uint32_t before = g_media_writes;
    ide8_write_byte(IDE8_ADDR_COMMAND, IDE8_CMD_SECTOR_READ);
    for (int i = 0; i < IDE8_SECTOR_SIZE; i++) (void)ide8_read_byte(IDE8_ADDR_DATA);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, g_media_writes, "reading must not count");

    ide8_write_byte(IDE8_ADDR_SCOUNT, 1);
    ide8_write_byte(IDE8_ADDR_SECTOR, 1);
    ide8_write_byte(IDE8_ADDR_COMMAND, IDE8_CMD_SECTOR_WRITE);
    for (int i = 0; i < IDE8_SECTOR_SIZE && hd->cmd == IDE8_CMD_SECTOR_WRITE; i++)
    {
        ide8_write_byte(IDE8_ADDR_DATA, 0x5A);
    };
    TEST_ASSERT_EQUAL_UINT32(before + 1, g_media_writes);
    /* Sektor 1 v LBA = blok 0 (ide8_drive_assign_registers odečte 1). */
    TEST_ASSERT_EQUAL_HEX8(0x5A, ide8_peek(0, 0));

    ide8_drive_close_image(hd);
    g_remove(hdd);
    g_free(hdd);
}


/* Quick Disk: nasazení z paměti, kopie obsahu a zavření bez INI. */
void test_qdisk_buffer_mount(void)
{
    uint8_t img[300];
    for (int i = 0; i < 300; i++) img[i] = (uint8_t)(i * 3);
    unsigned saved_connected = g_qdisk.connected;
    unsigned saved_type = g_qdisk.type;
    g_qdisk.type = QDISK_TYPE_IMAGE;

    /* Funguje i s odpojenou mechanikou (přehrání ji nasazuje před snapshotem). */
    g_qdisk.connected = 0;
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, qdisk_mount_image_buffer(img, sizeof(img), 1, "x.mzq"));
    TEST_ASSERT_TRUE(g_qdisk.handler_valid);
    TEST_ASSERT_EQUAL_INT(QDISK_STORAGE_DISCARD, g_qdisk.storage_mode);
    TEST_ASSERT_TRUE(g_qdisk.readonly);
    TEST_ASSERT_TRUE(g_qdisk.status & QDSTS_IMG_READY);

    size_t size = 0;
    uint8_t *copy = qdisk_copy_image(&size);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_size_t(sizeof(img), size);
    TEST_ASSERT_EQUAL_MEMORY(img, copy, sizeof(img));
    g_free(copy);

    qdisk_umount_keep_cfg();
    TEST_ASSERT_FALSE(g_qdisk.handler_valid);
    TEST_ASSERT_EQUAL_UINT(QDSTS_NO_DISC, g_qdisk.status);
    TEST_ASSERT_NULL(qdisk_copy_image(&size));

    g_qdisk.connected = saved_connected;
    g_qdisk.type = saved_type;
}


/** @brief Zapíše malé MZF (tělo 64 B na 1200h). */
static void write_test_mzf(const char *path)
{
    uint8_t buf[128 + 64];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x01;
    memcpy(buf + 1, "TEST\r", 5);
    buf[0x12] = 64;
    buf[0x15] = 0x12;
    buf[0x17] = 0x12;
    for (int i = 0; i < 64; i++) buf[128 + i] = (uint8_t)i;
    TEST_ASSERT_TRUE(g_file_set_contents(path, (const char *)buf, sizeof(buf), NULL));
}


/** @brief Počet a poslední událost `cmt` v souboru záznamu. */
static int cmt_events(const char *mzr, const st_REPLAY_EVENT **last, st_REPLAY_DATA **out)
{
    char *err = NULL;
    st_REPLAY_DATA *d = replay_data_read(mzr, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, err ? err : "");
    g_free(err);
    int n = 0;
    for (guint i = 0; i < d->events->len; i++)
    {
        const st_REPLAY_EVENT *e = &g_array_index(d->events, st_REPLAY_EVENT, i);
        if (e->kind != REPLAY_EV_CMT) continue;
        n++;
        if (last) *last = e;
    };
    *out = d;
    return n;
}


/* Operace kazety z oken: mimo záznam provede paused smyčka, při záznamu
 * události cmt (p / b), přehrání pásku vloží z kopie a operace zopakuje. */
void test_cmt_window_ops_recorded(void)
{
    char *mzf = tmp_path("t.mzf");
    char *mzr = tmp_path("cmt.mzr");
    write_test_mzf(mzf);
    cmt_eject();
    TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, cmt_open_file_by_extension(mzf));

    /* Mimo záznam: provede až emu vlákno (paused smyčka). */
    replay_host_cmt(REPLAY_CMT_PLAY, 0, NULL);
    TEST_ASSERT_TRUE(CMT_TEST_STOP);
    replay_on_paused();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    replay_host_cmt(REPLAY_CMT_STOP_BUTTON, 0, NULL);
    replay_on_paused();
    TEST_ASSERT_TRUE(CMT_TEST_STOP);

    /* Záznam: play v pauze na hranici (p), pauza za běhu na hranici (b). */
    replay_request_record(mzr, 0, NULL);
    replay_on_paused();
    st_REPLAY_STATUS st;
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    replay_host_cmt(REPLAY_CMT_PLAY, 0, NULL);
    replay_on_paused();
    TEST_ASSERT_TRUE(CMT_TEST_PLAY);
    g_emulator.paused = false;
    replay_host_cmt(REPLAY_CMT_PAUSE, 1, NULL);
    TEST_ASSERT_FALSE(CMT_TEST_PAUSED);
    next_frame();
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    next_frame();
    g_emulator.paused = true;
    replay_request_stop();
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_RECORDED, st.outcome, st.outcome_message);

    const st_REPLAY_EVENT *last = NULL;
    st_REPLAY_DATA *d = NULL;
    TEST_ASSERT_EQUAL_INT(2, cmt_events(mzr, &last, &d));
    TEST_ASSERT_EQUAL_STRING("pause", last->cmd);
    TEST_ASSERT_EQUAL_INT32(1, last->arg);
    TEST_ASSERT_EQUAL_INT(REPLAY_AT_BOUNDARY, last->at);
    TEST_ASSERT_EQUAL_UINT32(1, last->frame);
    const st_REPLAY_MEDIA *m = replay_data_find_media(d, "cmt");
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(m->data);
    TEST_ASSERT_EQUAL_UINT64(128 + 64, m->size);
    TEST_ASSERT_NOT_NULL(d->cmt_json);
    replay_data_free(d);

    /* Přehrání bez vložené pásky: páska z kopie, stav podle záznamu. */
    cmt_eject();
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);
    replay_request_play(mzr, true);
    replay_on_paused();
    replay_get_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.request_ok, st.request_message);
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_EQUAL_STRING("t.mzf", cmt_get_ui_base_filename());
    /* Okna jsou při přehrávání zamčená; operace z UI se zahodí. */
    replay_host_cmt(REPLAY_CMT_EJECT, 0, NULL);
    next_frame();
    TEST_ASSERT_TRUE(CMT_TEST_FILLED);
    TEST_ASSERT_TRUE(CMT_TEST_PAUSED);
    next_frame();
    replay_get_status(&st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(REPLAY_OUTCOME_ENDED, st.outcome, st.outcome_message);
    TEST_ASSERT_FALSE(st.deviated);
    /* Po konci: před přehráním nebyla vložená žádná páska. */
    TEST_ASSERT_FALSE(CMT_TEST_FILLED);

    g_remove(mzf);
    g_remove(mzr);
    g_free(mzf);
    g_free(mzr);
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    replay_init();
    s_tmpdir = g_dir_make_tmp("replay_media_XXXXXX", NULL);

    UNITY_BEGIN();
    RUN_TEST(test_media_recorded_and_played_on_copies);
    RUN_TEST(test_qdisk_buffer_mount);
    RUN_TEST(test_media_writes_counted_ide8);
    RUN_TEST(test_cmt_window_ops_recorded);
    int result = UNITY_END();

    g_rmdir(s_tmpdir);
    g_free(s_tmpdir);
    mztest_teardown();
    return result;
}
