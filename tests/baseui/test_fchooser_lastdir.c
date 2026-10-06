/**
 * @file test_fchooser_lastdir.c
 * @brief Testy paměti naposledy použitých adresářů dialogu pro výběr souboru.
 *
 * Testuje baseui_fchooser_lastdir (čisté C, bez ImGui):
 *   - adresář per kategorie, společný poslední adresář a náhrada CMT <-> MZF,
 *   - přeskočení neexistujících adresářů a ignorování prázdných vstupů,
 *   - seznam posledních adresářů (pořadí, bez duplicit, limit, normalizace),
 *   - pravidla baseui_fchooser_lastdir_resolve() pro výchozí umístění dialogu,
 *   - záložky a přenos celé paměti přes INI (uložení a nové načtení).
 *
 * Adresáře se vytvářejí v dočasném adresáři (g_dir_make_tmp) a po testu mažou.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>

#include "baseui/baseui_fchooser_lastdir.h"
#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"

/** @brief Počet dočasných adresářů vytvořených pro každý test. */
#define TEST_DIRS 12

/** @brief Dočasné adresáře (setUp vytvoří, tearDown smaže). */
static char *s_dir[TEST_DIRS];

/** @brief Dočasný soubor uvnitř s_dir[0] (pro pravidlo "path je soubor"). */
static char *s_file;

void setUp(void)
{
    baseui_fchooser_lastdir_reset();
    for (int i = 0; i < TEST_DIRS; i++)
    {
        GError *err = NULL;
        s_dir[i] = g_dir_make_tmp("mztest_lastdir_XXXXXX", &err);
        TEST_ASSERT_NOT_NULL_MESSAGE(s_dir[i], err ? err->message : "g_dir_make_tmp");
    }
    s_file = g_build_filename(s_dir[0], "list.bpt", NULL);
    TEST_ASSERT_TRUE(g_file_set_contents(s_file, "x", 1, NULL));
}

void tearDown(void)
{
    g_remove(s_file);
    g_free(s_file);
    s_file = NULL;
    for (int i = 0; i < TEST_DIRS; i++)
    {
        g_rmdir(s_dir[i]);
        g_free(s_dir[i]);
        s_dir[i] = NULL;
    }
    baseui_fchooser_lastdir_reset();
}

/** @brief Ověří výsledek baseui_fchooser_lastdir_get() a uvolní ho. */
static void assert_get(baseui_fchooser_category_t cat, const char *expected)
{
    char *got = baseui_fchooser_lastdir_get(cat);
    if (expected == NULL)
        TEST_ASSERT_NULL(got);
    else
        TEST_ASSERT_EQUAL_STRING(expected, got);
    g_free(got);
}

/* Bez zapamatovaného adresáře nic nevrací. */
void test_lastdir_empty_returns_null(void)
{
    assert_get(BASEUI_FCHOOSER_CAT_CMT, NULL);
    assert_get(BASEUI_FCHOOSER_CAT_GENERIC, NULL);
}

/* Kategorie si pamatují vlastní adresář, ostatní dostanou společný poslední. */
void test_lastdir_category_and_global(void)
{
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_CMT, s_dir[0]);
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_DSK, s_dir[1]);

    assert_get(BASEUI_FCHOOSER_CAT_CMT, s_dir[0]);
    assert_get(BASEUI_FCHOOSER_CAT_DSK, s_dir[1]);
    assert_get(BASEUI_FCHOOSER_CAT_ROM, s_dir[1]);     /* společný poslední */
    assert_get(BASEUI_FCHOOSER_CAT_GENERIC, s_dir[1]);
}

/* CMT a MZF se zastupují navzájem dřív, než přijde na řadu společný. */
void test_lastdir_cmt_mzf_fallback(void)
{
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_MZF, s_dir[0]);
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_DSK, s_dir[1]);

    assert_get(BASEUI_FCHOOSER_CAT_CMT, s_dir[0]);
}

/* GENERIC mění jen společné hodnoty, žádnou kategorii. */
void test_lastdir_generic_does_not_set_category(void)
{
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_ROM, s_dir[0]);
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_GENERIC, s_dir[1]);

    assert_get(BASEUI_FCHOOSER_CAT_ROM, s_dir[0]);
    assert_get(BASEUI_FCHOOSER_CAT_DSK, s_dir[1]);
}

/* Neexistující adresář (odpojený disk) se přeskočí. */
void test_lastdir_missing_dir_is_skipped(void)
{
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_DSK, s_dir[1]);
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_CMT, s_dir[0]);
    g_remove(s_file);
    TEST_ASSERT_EQUAL_INT(0, g_rmdir(s_dir[0]));

    /* CMT ani společný (oba s_dir[0]) neexistují, MZF nemá nic -> NULL. */
    assert_get(BASEUI_FCHOOSER_CAT_CMT, NULL);
    assert_get(BASEUI_FCHOOSER_CAT_DSK, s_dir[1]);
}

/* NULL, prázdný řetězec a "." se nezapamatují. */
void test_lastdir_ignores_empty_input(void)
{
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_CMT, NULL);
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_CMT, "");
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_CMT, ".");

    assert_get(BASEUI_FCHOOSER_CAT_CMT, NULL);
    char **recent = baseui_fchooser_lastdir_get_recent();
    TEST_ASSERT_EQUAL_UINT(0, g_strv_length(recent));
    g_strfreev(recent);
}

/* Seznam posledních: nejnovější první, bez duplicit, koncový oddělovač se ignoruje. */
void test_lastdir_recent_order_and_dedupe(void)
{
    char *with_sep = g_strconcat(s_dir[0], G_DIR_SEPARATOR_S, NULL);

    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_CMT, s_dir[0]);
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_DSK, s_dir[1]);
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_ROM, with_sep);

    char **recent = baseui_fchooser_lastdir_get_recent();
    TEST_ASSERT_EQUAL_UINT(2, g_strv_length(recent));
    TEST_ASSERT_EQUAL_STRING(s_dir[0], recent[0]);
    TEST_ASSERT_EQUAL_STRING(s_dir[1], recent[1]);
    g_strfreev(recent);

    /* uložená cesta je bez koncového oddělovače */
    assert_get(BASEUI_FCHOOSER_CAT_ROM, s_dir[0]);
    g_free(with_sep);
}

/* Seznam posledních má nejvýše BASEUI_FCHOOSER_RECENT_MAX položek. */
void test_lastdir_recent_limit(void)
{
    for (int i = 0; i < TEST_DIRS; i++)
        baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_GENERIC, s_dir[i]);

    char **recent = baseui_fchooser_lastdir_get_recent();
    TEST_ASSERT_EQUAL_UINT(BASEUI_FCHOOSER_RECENT_MAX, g_strv_length(recent));
    TEST_ASSERT_EQUAL_STRING(s_dir[TEST_DIRS - 1], recent[0]);
    TEST_ASSERT_EQUAL_STRING(s_dir[TEST_DIRS - BASEUI_FCHOOSER_RECENT_MAX], recent[BASEUI_FCHOOSER_RECENT_MAX - 1]);
    g_strfreev(recent);
}

/** @brief Výsledek resolve pro přehledné porovnání. */
typedef struct
{
    char *path;
    char *fileName;
    char *filePathName;
} resolved_t;

static resolved_t resolve(baseui_fchooser_category_t cat, const char *path, const char *fileName, const char *filePathName)
{
    resolved_t r;
    baseui_fchooser_lastdir_resolve(cat, path, fileName, filePathName, &r.path, &r.fileName, &r.filePathName);
    return r;
}

static void resolved_free(resolved_t *r)
{
    g_free(r->path);
    g_free(r->fileName);
    g_free(r->filePathName);
}

/* 1. filePathName s existujícím adresářem má přednost a nemění se. */
void test_resolve_filepathname_with_existing_dir(void)
{
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_DSK, s_dir[1]);
    char *fpn = g_build_filename(s_dir[0], "disk.dsk", NULL);

    resolved_t r = resolve(BASEUI_FCHOOSER_CAT_DSK, NULL, NULL, fpn);
    TEST_ASSERT_EQUAL_STRING(fpn, r.filePathName);
    TEST_ASSERT_NULL(r.path);
    TEST_ASSERT_NULL(r.fileName);
    resolved_free(&r);
    g_free(fpn);
}

/* filePathName jen se jménem: adresář z paměti, jméno jako výchozí název. */
void test_resolve_name_only_uses_memory(void)
{
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_CMT, s_dir[0]);

    resolved_t r = resolve(BASEUI_FCHOOSER_CAT_CMT, NULL, NULL, "newfile.wav");
    TEST_ASSERT_NULL(r.filePathName);
    TEST_ASSERT_EQUAL_STRING(s_dir[0], r.path);
    TEST_ASSERT_EQUAL_STRING("newfile.wav", r.fileName);
    resolved_free(&r);
}

/* Prázdné filePathName (CMT po startu) a cesta "." -> paměť. */
void test_resolve_empty_and_dot_use_memory(void)
{
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_CMT, s_dir[0]);

    resolved_t r = resolve(BASEUI_FCHOOSER_CAT_CMT, NULL, NULL, "");
    TEST_ASSERT_EQUAL_STRING(s_dir[0], r.path);
    TEST_ASSERT_NULL(r.fileName);
    resolved_free(&r);

    r = resolve(BASEUI_FCHOOSER_CAT_CMT, ".", "dump.bin", NULL);
    TEST_ASSERT_EQUAL_STRING(s_dir[0], r.path);
    TEST_ASSERT_EQUAL_STRING("dump.bin", r.fileName);
    resolved_free(&r);
}

/* 2. Explicitní existující adresář volajícího má přednost před pamětí. */
void test_resolve_explicit_dir_wins(void)
{
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_SNAPSHOT, s_dir[0]);

    resolved_t r = resolve(BASEUI_FCHOOSER_CAT_SNAPSHOT, s_dir[1], NULL, NULL);
    TEST_ASSERT_EQUAL_STRING(s_dir[1], r.path);
    resolved_free(&r);
}

/* 2b. path je existující soubor: jeho adresář a jméno. */
void test_resolve_path_is_file(void)
{
    resolved_t r = resolve(BASEUI_FCHOOSER_CAT_DBG_LISTS, s_file, NULL, NULL);
    TEST_ASSERT_EQUAL_STRING(s_dir[0], r.path);
    TEST_ASSERT_EQUAL_STRING("list.bpt", r.fileName);
    resolved_free(&r);
}

/* Neexistující adresář ve filePathName: jméno zůstane, adresář z paměti. */
void test_resolve_missing_dir_in_filepathname(void)
{
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_HDD, s_dir[2]);
    char *fpn = g_build_filename(s_dir[0], "no_such_dir", "hdd.img", NULL);

    resolved_t r = resolve(BASEUI_FCHOOSER_CAT_HDD, NULL, NULL, fpn);
    TEST_ASSERT_NULL(r.filePathName);
    TEST_ASSERT_EQUAL_STRING(s_dir[2], r.path);
    TEST_ASSERT_EQUAL_STRING("hdd.img", r.fileName);
    resolved_free(&r);
    g_free(fpn);
}

/* 4. Bez paměti a bez vstupu -> pracovní adresář ".". */
void test_resolve_nothing_gives_cwd(void)
{
    resolved_t r = resolve(BASEUI_FCHOOSER_CAT_ROM, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_STRING(".", r.path);
    TEST_ASSERT_NULL(r.fileName);
    TEST_ASSERT_NULL(r.filePathName);
    resolved_free(&r);
}

/* Záložky se uloží a vrátí beze změny; NULL = prázdné. */
void test_bookmarks_roundtrip(void)
{
    char *bm = baseui_fchooser_lastdir_get_bookmarks();
    TEST_ASSERT_EQUAL_STRING("", bm);
    g_free(bm);

    baseui_fchooser_lastdir_set_bookmarks("###B###games##C:/games");
    bm = baseui_fchooser_lastdir_get_bookmarks();
    TEST_ASSERT_EQUAL_STRING("###B###games##C:/games", bm);
    g_free(bm);

    baseui_fchooser_lastdir_set_bookmarks(NULL);
    bm = baseui_fchooser_lastdir_get_bookmarks();
    TEST_ASSERT_EQUAL_STRING("", bm);
    g_free(bm);
}

/* Celá paměť přežije uložení INI a nové načtení (jako restart emulátoru). */
void test_ini_roundtrip(void)
{
    char *ini = g_build_filename(s_dir[TEST_DIRS - 1], "test.ini", NULL);
    st_CFGROOT *saved_root = g_cfgmain;

    /* Jako start emulátoru: registrace (INI ještě neexistuje), pak práce
     * s dialogy, nakonec uložení při ukončení. */
    g_cfgmain = cfgroot_new(ini);
    baseui_fchooser_lastdir_config_init();

    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_CMT, s_dir[0]);
    baseui_fchooser_lastdir_remember(BASEUI_FCHOOSER_CAT_DSK, s_dir[1]);
    baseui_fchooser_lastdir_set_bookmarks("###B###games##C:/games");

    cfgroot_save(g_cfgmain);
    cfgroot_destroy(g_cfgmain);

    /* "Restart": prázdná paměť, nové načtení ze stejného INI. */
    baseui_fchooser_lastdir_reset();
    assert_get(BASEUI_FCHOOSER_CAT_CMT, NULL);
    g_cfgmain = cfgroot_new(ini);
    baseui_fchooser_lastdir_config_init();
    cfgroot_destroy(g_cfgmain);
    g_cfgmain = saved_root;

    assert_get(BASEUI_FCHOOSER_CAT_CMT, s_dir[0]);
    assert_get(BASEUI_FCHOOSER_CAT_DSK, s_dir[1]);
    assert_get(BASEUI_FCHOOSER_CAT_ROM, s_dir[1]);
    char **recent = baseui_fchooser_lastdir_get_recent();
    TEST_ASSERT_EQUAL_UINT(2, g_strv_length(recent));
    TEST_ASSERT_EQUAL_STRING(s_dir[1], recent[0]);
    TEST_ASSERT_EQUAL_STRING(s_dir[0], recent[1]);
    g_strfreev(recent);
    char *bm = baseui_fchooser_lastdir_get_bookmarks();
    TEST_ASSERT_EQUAL_STRING("###B###games##C:/games", bm);
    g_free(bm);

    g_remove(ini);
    g_free(ini);
}

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_lastdir_empty_returns_null);
    RUN_TEST(test_lastdir_category_and_global);
    RUN_TEST(test_lastdir_cmt_mzf_fallback);
    RUN_TEST(test_lastdir_generic_does_not_set_category);
    RUN_TEST(test_lastdir_missing_dir_is_skipped);
    RUN_TEST(test_lastdir_ignores_empty_input);
    RUN_TEST(test_lastdir_recent_order_and_dedupe);
    RUN_TEST(test_lastdir_recent_limit);
    RUN_TEST(test_resolve_filepathname_with_existing_dir);
    RUN_TEST(test_resolve_name_only_uses_memory);
    RUN_TEST(test_resolve_empty_and_dot_use_memory);
    RUN_TEST(test_resolve_explicit_dir_wins);
    RUN_TEST(test_resolve_path_is_file);
    RUN_TEST(test_resolve_missing_dir_in_filepathname);
    RUN_TEST(test_resolve_nothing_gives_cwd);
    RUN_TEST(test_bookmarks_roundtrip);
    RUN_TEST(test_ini_roundtrip);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
