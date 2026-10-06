/**
 * @file baseui_fchooser_lastdir.c
 * @brief Paměť naposledy použitých adresářů dialogu pro výběr souboru.
 *
 * Implementace API z baseui_fchooser_lastdir.h. Stav drží statická struktura
 * chráněná jedním GMutexem. Do INI se zapisuje přes save callbacky elementů
 * sekce [FILECHOOSER], načítá se jednou v baseui_fchooser_lastdir_config_init().
 */

#include <string.h>
#include <glib.h>

#include "baseui_fchooser_lastdir.h"

#include "cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

/**
 * @brief Stav paměti adresářů.
 *
 * Všechny řetězce jsou alokované GLibem (g_strdup) nebo NULL; vlastní je
 * tato struktura. Přístup jen pod @c mutex.
 *
 * Invarianty: @c recent obsahuje nejvýše BASEUI_FCHOOSER_RECENT_MAX
 * nenulových položek, nenulové jsou souvisle od indexu 0, bez duplicit.
 */
static struct
{
    GMutex mutex;
    char *category_dir[BASEUI_FCHOOSER_CAT_COUNT]; /**< adresář per kategorie (index GENERIC se nepoužívá) */
    char *last_dir;                                /**< společný poslední adresář */
    char *recent[BASEUI_FCHOOSER_RECENT_MAX];      /**< naposledy použité adresáře, nejnovější první */
    char *bookmarks;                               /**< serializované záložky IGFD */
} g_lastdir;

/** @brief Jména kategorií = základ INI klíčů; pořadí odpovídá enumu. */
static const char *const s_category_names[BASEUI_FCHOOSER_CAT_COUNT] = {
    "generic",
    "cmt",
    "mzf",
    "dsk",
    "qdisk",
    "ramdisk",
    "memext",
    "hdd",
    "rom",
    "sdcard",
    "plotter",
    "snapshot",
    "video",
    "dbg_memory",
    "dbg_symbols",
    "dbg_lists",
    "dbg_export",
};

G_STATIC_ASSERT(G_N_ELEMENTS(s_category_names) == BASEUI_FCHOOSER_CAT_COUNT);

const char *baseui_fchooser_lastdir_category_name(baseui_fchooser_category_t category)
{
    if ((int)category < 0 || category >= BASEUI_FCHOOSER_CAT_COUNT)
        return s_category_names[BASEUI_FCHOOSER_CAT_GENERIC];
    return s_category_names[category];
}

/**
 * @brief Náhradní kategorie, jejíž adresář se zkusí, když vlastní chybí.
 *
 * CMT a MZF sdílejí stejné soubory (páska z MZF), proto se zastupují
 * navzájem - stejně jako ve starší verzi emulátoru.
 *
 * @return Náhradní kategorie, nebo GENERIC (= žádná).
 */
static baseui_fchooser_category_t lastdir_fallback_category(baseui_fchooser_category_t category)
{
    switch (category)
    {
    case BASEUI_FCHOOSER_CAT_CMT:
        return BASEUI_FCHOOSER_CAT_MZF;
    case BASEUI_FCHOOSER_CAT_MZF:
        return BASEUI_FCHOOSER_CAT_CMT;
    default:
        return BASEUI_FCHOOSER_CAT_GENERIC;
    }
}

/**
 * @brief Nahradí řetězec ve slotu kopií @p value.
 * @param slot Ukazatel na slot (vlastněný řetězec nebo NULL).
 * @param value Nová hodnota; NULL nebo prázdný řetězec slot vynuluje.
 */
static void lastdir_slot_set(char **slot, const char *value)
{
    g_free(*slot);
    *slot = (value != NULL && value[0] != '\0') ? g_strdup(value) : NULL;
}

/**
 * @brief Je cesta existující adresář?
 * @param dirpath Cesta, může být NULL.
 */
static bool lastdir_dir_exists(const char *dirpath)
{
    return (dirpath != NULL && dirpath[0] != '\0' && g_file_test(dirpath, G_FILE_TEST_IS_DIR));
}

/**
 * @brief Je znak oddělovač adresářů? Na Windows platí '\\' i '/'.
 */
static bool lastdir_is_separator(char c)
{
#ifdef G_OS_WIN32
    return (c == '\\' || c == '/');
#else
    return (c == '/');
#endif
}

/**
 * @brief Vrátí normalizovanou kopii cesty adresáře (bez koncového oddělovače).
 *
 * Kořen ("/", "C:\", "C:/") zůstává beze změny, aby zůstal platnou cestou.
 *
 * @return Nově alokovaný řetězec (g_free()).
 */
static char *lastdir_normalize(const char *dirpath)
{
    char *s = g_strdup(dirpath);
    size_t len = strlen(s);
    while (len > 1 && lastdir_is_separator(s[len - 1]))
    {
        /* "C:\" - kořen disku necháme */
        if (len == 3 && s[1] == ':')
            break;
        s[--len] = '\0';
    }
    return s;
}

/**
 * @brief Porovná dvě cesty adresářů; na Windows bez ohledu na velikost písmen.
 */
static bool lastdir_path_equal(const char *a, const char *b)
{
#ifdef G_OS_WIN32
    return (g_ascii_strcasecmp(a, b) == 0);
#else
    return (strcmp(a, b) == 0);
#endif
}

/**
 * @brief Přesune adresář na začátek seznamu posledních.
 * @pre Volající drží g_lastdir.mutex.
 * @param dirpath Normalizovaná cesta.
 */
static void lastdir_recent_push(const char *dirpath)
{
    /* Najít existující výskyt; jinak "uvolnit" poslední pozici. */
    int found = BASEUI_FCHOOSER_RECENT_MAX - 1;
    for (int i = 0; i < BASEUI_FCHOOSER_RECENT_MAX; i++)
    {
        if (g_lastdir.recent[i] == NULL)
        {
            found = i;
            break;
        }
        if (lastdir_path_equal(g_lastdir.recent[i], dirpath))
        {
            found = i;
            break;
        }
    }
    g_free(g_lastdir.recent[found]);
    for (int i = found; i > 0; i--)
        g_lastdir.recent[i] = g_lastdir.recent[i - 1];
    g_lastdir.recent[0] = g_strdup(dirpath);
}

void baseui_fchooser_lastdir_reset(void)
{
    g_mutex_lock(&g_lastdir.mutex);
    for (int i = 0; i < BASEUI_FCHOOSER_CAT_COUNT; i++)
        lastdir_slot_set(&g_lastdir.category_dir[i], NULL);
    lastdir_slot_set(&g_lastdir.last_dir, NULL);
    for (int i = 0; i < BASEUI_FCHOOSER_RECENT_MAX; i++)
        lastdir_slot_set(&g_lastdir.recent[i], NULL);
    lastdir_slot_set(&g_lastdir.bookmarks, NULL);
    g_mutex_unlock(&g_lastdir.mutex);
}

void baseui_fchooser_lastdir_remember(baseui_fchooser_category_t category, const char *dirpath)
{
    if (dirpath == NULL || dirpath[0] == '\0' || strcmp(dirpath, ".") == 0)
        return;

    char *norm = lastdir_normalize(dirpath);

    g_mutex_lock(&g_lastdir.mutex);
    if (category > BASEUI_FCHOOSER_CAT_GENERIC && category < BASEUI_FCHOOSER_CAT_COUNT)
        lastdir_slot_set(&g_lastdir.category_dir[category], norm);
    lastdir_slot_set(&g_lastdir.last_dir, norm);
    lastdir_recent_push(norm);
    g_mutex_unlock(&g_lastdir.mutex);

    g_free(norm);
}

char *baseui_fchooser_lastdir_get(baseui_fchooser_category_t category)
{
    if ((int)category < 0 || category >= BASEUI_FCHOOSER_CAT_COUNT)
        category = BASEUI_FCHOOSER_CAT_GENERIC;

    /* Kandidáty zkopírujeme pod zámkem, existenci na disku testujeme mimo
     * zámek (g_file_test může na síťové cestě trvat). */
    char *candidates[3];
    g_mutex_lock(&g_lastdir.mutex);
    candidates[0] = g_strdup(g_lastdir.category_dir[category]);
    candidates[1] = g_strdup(g_lastdir.category_dir[lastdir_fallback_category(category)]);
    candidates[2] = g_strdup(g_lastdir.last_dir);
    g_mutex_unlock(&g_lastdir.mutex);

    char *result = NULL;
    for (int i = 0; i < 3; i++)
    {
        if (result == NULL && lastdir_dir_exists(candidates[i]))
            result = candidates[i];
        else
            g_free(candidates[i]);
    }
    return result;
}

void baseui_fchooser_lastdir_resolve(baseui_fchooser_category_t category,
                                     const char *path, const char *fileName, const char *filePathName,
                                     char **out_path, char **out_fileName, char **out_filePathName)
{
    *out_path = NULL;
    *out_fileName = NULL;
    *out_filePathName = NULL;

    const char *use_fileName = fileName;
    char *derived_name = NULL;

    if (filePathName != NULL && filePathName[0] != '\0')
    {
        char *dir = g_path_get_dirname(filePathName);
        bool has_dir = (strcmp(dir, ".") != 0);
        bool dir_ok = has_dir && lastdir_dir_exists(dir);
        g_free(dir);

        if (dir_ok)
        {
            /* 1. Volající přesně ví, co nabídnout. */
            *out_filePathName = g_strdup(filePathName);
            return;
        }

        /* Jen jméno, nebo neexistující adresář - jméno si ponecháme. */
        derived_name = g_path_get_basename(filePathName);
        if (derived_name[0] != '\0' && !lastdir_is_separator(derived_name[0]) && strcmp(derived_name, ".") != 0)
            use_fileName = derived_name;
    }

    if (path != NULL && path[0] != '\0' && strcmp(path, ".") != 0 && lastdir_dir_exists(path))
    {
        /* 2. Explicitní adresář od volajícího. */
        *out_path = g_strdup(path);
    }
    else if (path != NULL && path[0] != '\0' && g_file_test(path, G_FILE_TEST_IS_REGULAR))
    {
        /* 2b. Volající dal cestu k souboru (např. výchozí soubor seznamu
         * v debuggeru) - jeho adresář, jméno jako výchozí název. */
        *out_path = g_path_get_dirname(path);
        if (use_fileName == NULL || use_fileName[0] == '\0')
        {
            g_free(derived_name);
            derived_name = g_path_get_basename(path);
            use_fileName = derived_name;
        }
    }
    else
    {
        /* 3. Paměť, 4. pracovní adresář. */
        *out_path = baseui_fchooser_lastdir_get(category);
        if (*out_path == NULL)
            *out_path = g_strdup(".");
    }

    if (use_fileName != NULL && use_fileName[0] != '\0')
        *out_fileName = g_strdup(use_fileName);

    g_free(derived_name);
}

char **baseui_fchooser_lastdir_get_recent(void)
{
    GStrvBuilder *b = g_strv_builder_new();
    g_mutex_lock(&g_lastdir.mutex);
    for (int i = 0; i < BASEUI_FCHOOSER_RECENT_MAX && g_lastdir.recent[i] != NULL; i++)
        g_strv_builder_add(b, g_lastdir.recent[i]);
    g_mutex_unlock(&g_lastdir.mutex);
    char **res = g_strv_builder_end(b);
    g_strv_builder_unref(b);
    return res;
}

char *baseui_fchooser_lastdir_get_bookmarks(void)
{
    g_mutex_lock(&g_lastdir.mutex);
    char *res = g_strdup(g_lastdir.bookmarks != NULL ? g_lastdir.bookmarks : "");
    g_mutex_unlock(&g_lastdir.mutex);
    return res;
}

void baseui_fchooser_lastdir_set_bookmarks(const char *serialized)
{
    g_mutex_lock(&g_lastdir.mutex);
    lastdir_slot_set(&g_lastdir.bookmarks, serialized);
    g_mutex_unlock(&g_lastdir.mutex);
}

/* ========================================================================= */
/*                              INI                                          */
/* ========================================================================= */

/**
 * @brief Propagate callback: hodnota z INI -> slot (cbdata = char **).
 */
static void lastdir_propagatecfg_slot(void *e, void *data)
{
    char **slot = (char **)data;
    g_mutex_lock(&g_lastdir.mutex);
    lastdir_slot_set(slot, cfgelement_get_text_value((st_CFGELEMENT *)e));
    g_mutex_unlock(&g_lastdir.mutex);
}

/**
 * @brief Save callback: slot -> hodnota elementu před zápisem INI (cbdata = char **).
 */
static void lastdir_savecfg_slot(void *e, void *data)
{
    char **slot = (char **)data;
    g_mutex_lock(&g_lastdir.mutex);
    char *value = g_strdup(*slot != NULL ? *slot : "");
    g_mutex_unlock(&g_lastdir.mutex);
    cfgelement_set_text_value((st_CFGELEMENT *)e, value);
    g_free(value);
}

/**
 * @brief Zaregistruje jeden textový element navázaný na slot.
 */
static void lastdir_register_slot(CFGMOD *cmod, const char *name, char **slot)
{
    /* cfgfile API nemá const, jméno ale jen kopíruje (cfgcommon_set_text) */
    CFGELM *elm = cfgmodule_register_new_element(cmod, (char *)name, CFGENTYPE_TEXT, "");
    cfgelement_set_propagate_cb(elm, lastdir_propagatecfg_slot, slot);
    cfgelement_set_save_cb(elm, lastdir_savecfg_slot, slot);
}

void baseui_fchooser_lastdir_config_init(void)
{
    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, "FILECHOOSER");

    lastdir_register_slot(cmod, "last_dir", &g_lastdir.last_dir);

    for (int i = BASEUI_FCHOOSER_CAT_GENERIC + 1; i < BASEUI_FCHOOSER_CAT_COUNT; i++)
    {
        char *key = g_strdup_printf("%s_dir", s_category_names[i]);
        lastdir_register_slot(cmod, key, &g_lastdir.category_dir[i]);
        g_free(key); /* cfgmodule si jméno kopíruje */
    }

    for (int i = 0; i < BASEUI_FCHOOSER_RECENT_MAX; i++)
    {
        char *key = g_strdup_printf("recent_dir_%d", i + 1);
        lastdir_register_slot(cmod, key, &g_lastdir.recent[i]);
        g_free(key);
    }

    lastdir_register_slot(cmod, "bookmarks", &g_lastdir.bookmarks);

    /* Vzor snapshot_config_init: globální cfgroot_propagate se nevolá. */
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);

    /* Ruční zásah do INI mohl v seznamu nechat díry - zhutnit (invariant). */
    g_mutex_lock(&g_lastdir.mutex);
    int dst = 0;
    for (int i = 0; i < BASEUI_FCHOOSER_RECENT_MAX; i++)
    {
        char *p = g_lastdir.recent[i];
        g_lastdir.recent[i] = NULL;
        if (p == NULL)
            continue;
        bool dup = false;
        for (int j = 0; j < dst; j++)
            dup = dup || lastdir_path_equal(g_lastdir.recent[j], p);
        if (dup)
            g_free(p);
        else
            g_lastdir.recent[dst++] = p;
    }
    g_mutex_unlock(&g_lastdir.mutex);
}
