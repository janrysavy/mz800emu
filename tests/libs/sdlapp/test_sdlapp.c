/**
 * @file   test_sdlapp.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Unit testy knihovny sdlapp (SDL3 aplikační framework)
 *
 * Testuje životní cyklus SdlApp instance (vytvoření, zničení, quit),
 * window manager (vytváření/mazání oken, vyhledávání podle jména/ID/regex,
 * duplicitní jména, počet oken), stavový automat oken (INITIALIZED, ACTIVE,
 * PAUSED), window API (gettery/settery pro SDL window, renderer, manager,
 * title, name, size), close callback mechanismus, registraci custom eventů,
 * frame rate control (loop mode, fps limit), quit-on-last-window-close
 * příznak, drag & drop callback, timer API a regresní testy opravených bugů.
 *
 * Standalone test — linkuje se s Unity + sdlapp .o soubory + SDL3 + GLib.
 * SDL běží s dummy video driverem pro headless testování.
 *
 * @par Changelog:
 * - 2026-03-14: Proběhla kompletní revize a refaktorizace. Vytvořeny unit testy.
 *
 * @par Licence:
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "unity.h"

#include <SDL3/SDL.h>
#include <glib.h>
#include <string.h>

#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_int.h"

/* ========================================================================
 * Globální stav testů
 * ======================================================================== */

/** @brief Globální instance SdlApp používaná ve všech testech */
static SdlApp *g_app = NULL;

/* ========================================================================
 * setUp / tearDown — volány před/po každém testu
 * ======================================================================== */

/** @brief Inicializace před každým testem — vytvoří čistou SdlApp instanci */
void setUp(void)
{
    /* každý test začíná s čistou aplikací */
    g_app = sdlapp_new();
}

/** @brief Úklid po každém testu — zničí SdlApp instanci */
void tearDown(void)
{
    if (g_app)
    {
        sdlapp_destroy(g_app);
        g_app = NULL;
    }
}

/* ========================================================================
 * SMOKE TESTY — základní životní cyklus
 * ======================================================================== */

/** @brief Testuje, že sdlapp_new vrátí validní instanci s inicializovaným managerem */
void test_smoke_new_destroy(void)
{
    TEST_ASSERT_NOT_NULL(g_app);
    TEST_ASSERT_NOT_NULL(g_app->manager);
}

/** @brief Testuje, že sdlapp_is_running vrátí FALSE pro NULL ukazatel */
void test_smoke_is_running_null(void)
{
    TEST_ASSERT_FALSE(sdlapp_is_running(NULL));
}

/** @brief Testuje, že po sdlapp_new je running=FALSE (aplikace ještě neběží) */
void test_smoke_is_running_default(void)
{
    TEST_ASSERT_FALSE(sdlapp_is_running(g_app));
}

/** @brief Testuje, že sdlapp_quit nastaví running=FALSE */
void test_smoke_quit(void)
{
    g_app->running = TRUE;
    sdlapp_quit(g_app);
    TEST_ASSERT_FALSE(sdlapp_is_running(g_app));
}

/** @brief Testuje, že sdlapp_quit(NULL) nespadne (NULL-safe) */
void test_smoke_quit_null(void)
{
    sdlapp_quit(NULL);
}

/* ========================================================================
 * WINDOW MANAGER TESTY
 * ======================================================================== */

/** @brief Testuje vytvoření a zničení okna přes window manager */
void test_wm_create_window(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    TEST_ASSERT_NOT_NULL(wm);

    SdlAppWindow *win = sdlapp_window_create(wm, "test_win", "Test", 320, 240, 0, SDLAPP_RENDERER_SDL, NULL, NULL);
    TEST_ASSERT_NOT_NULL(win);
    TEST_ASSERT_EQUAL_UINT(1, sdlapp_winmanager_get_window_count(wm));

    sdlapp_window_destroy(wm, win);
    TEST_ASSERT_EQUAL_UINT(0, sdlapp_winmanager_get_window_count(wm));
}

/** @brief Testuje vyhledání okna podle jména a neexistujícího jména */
void test_wm_get_by_name(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "my_window", "Title", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);
    TEST_ASSERT_NOT_NULL(win);

    SdlAppWindow *found = sdlapp_winmanager_get_window_by_name(wm, "my_window");
    TEST_ASSERT_EQUAL_PTR(win, found);

    TEST_ASSERT_NULL(sdlapp_winmanager_get_window_by_name(wm, "nonexistent"));

    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje vyhledání okna podle SDL WindowID */
void test_wm_get_by_id(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "id_test", "Title", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);
    TEST_ASSERT_NOT_NULL(win);

    SDL_WindowID id = SDL_GetWindowID(win->sdl_window);
    SdlAppWindow *found = sdlapp_winmanager_get_window_by_SDL_WindowID(wm, id);
    TEST_ASSERT_EQUAL_PTR(win, found);

    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje vyhledání oken podle regulárního výrazu */
void test_wm_get_by_regex(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *w1 = sdlapp_window_create(wm, "win_alpha", "A", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);
    SdlAppWindow *w2 = sdlapp_window_create(wm, "win_beta", "B", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);
    SdlAppWindow *w3 = sdlapp_window_create(wm, "other", "C", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    guint count = 0;
    GError *error = NULL;
    SdlAppWindow **result = sdlapp_winmanager_get_window_list_by_regex(wm, "win_.*", &count, &error);
    TEST_ASSERT_NULL(error);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_UINT(2, count);

    g_free(result);

    sdlapp_window_destroy(wm, w3);
    sdlapp_window_destroy(wm, w2);
    sdlapp_window_destroy(wm, w1);
}

/** @brief Testuje, že duplicitní jméno okna je odmítnuto (vrátí NULL) */
void test_wm_duplicate_name(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *w1 = sdlapp_window_create(wm, "dup_name", "A", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);
    TEST_ASSERT_NOT_NULL(w1);

    SdlAppWindow *w2 = sdlapp_window_create(wm, "dup_name", "B", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);
    TEST_ASSERT_NULL(w2);

    sdlapp_window_destroy(wm, w1);
}

/* ========================================================================
 * STAVOVÝ AUTOMAT
 * ======================================================================== */

/** @brief Testuje, že po vytvoření je stav okna INITIALIZED (init_callback=NULL) */
void test_state_after_create(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "state_test", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(SDLAPP_WINDOW_STATE_INITIALIZED, sdlapp_window_get_state(win));

    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje, že aktivace okna změní stav na ACTIVE */
void test_state_activate(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "act_test", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    sdlapp_window_activate(win);
    TEST_ASSERT_EQUAL_INT(SDLAPP_WINDOW_STATE_ACTIVE, sdlapp_window_get_state(win));
    TEST_ASSERT_TRUE(sdlapp_window_get_active(win));

    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje, že deaktivace okna změní stav na PAUSED */
void test_state_deactivate(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "deact_test", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    sdlapp_window_activate(win);
    TEST_ASSERT_EQUAL_INT(SDLAPP_WINDOW_STATE_ACTIVE, sdlapp_window_get_state(win));

    sdlapp_window_set_active(win, FALSE);
    TEST_ASSERT_EQUAL_INT(SDLAPP_WINDOW_STATE_PAUSED, sdlapp_window_get_state(win));
    TEST_ASSERT_FALSE(sdlapp_window_get_active(win));

    sdlapp_window_destroy(wm, win);
}

/* ========================================================================
 * WINDOW API — gettery
 * ======================================================================== */

/** @brief Testuje, že sdlapp_window_get_sdl_window vrátí platný SDL window */
void test_api_get_sdl_window(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "sdl_test", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    TEST_ASSERT_NOT_NULL(sdlapp_window_get_sdl_window(win));
    TEST_ASSERT_EQUAL_PTR(win->sdl_window, sdlapp_window_get_sdl_window(win));

    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje, že sdlapp_window_get_sdl_renderer vrátí platný SDL renderer */
void test_api_get_sdl_renderer(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "rend_test", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    TEST_ASSERT_NOT_NULL(sdlapp_window_get_sdl_renderer(win));
    TEST_ASSERT_EQUAL_INT(SDLAPP_RENDERER_SDL, sdlapp_window_get_renderer_type(win));

    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje, že sdlapp_window_get_manager vrátí správce oken */
void test_api_get_manager(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "mgr_test", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    TEST_ASSERT_EQUAL_PTR(wm, sdlapp_window_get_manager(win));

    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje nastavení a čtení titulku okna (set/get title) */
void test_api_set_title(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "title_test", "Original", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    sdlapp_window_set_title(win, "Changed");
    TEST_ASSERT_EQUAL_STRING("Changed", sdlapp_window_get_title(win));

    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje, že get_name_dup vrátí kopii jména (jiný ukazatel než originál) */
void test_api_get_name_dup(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "name_dup", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    gchar *dup = sdlapp_window_get_name_dup(win);
    TEST_ASSERT_NOT_NULL(dup);
    TEST_ASSERT_EQUAL_STRING("name_dup", dup);
    /* musí být jiný ukazatel než interní */
    TEST_ASSERT_NOT_EQUAL(win->name, dup);

    g_free(dup);
    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje přejmenování okna a aktualizaci vyhledávání v manageru */
void test_api_set_name(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "old_name", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    TEST_ASSERT_TRUE(sdlapp_window_set_name(win, "new_name"));
    TEST_ASSERT_EQUAL_STRING("new_name", sdlapp_window_get_name(win));

    /* staré jméno by nemělo být nalezeno */
    TEST_ASSERT_NULL(sdlapp_winmanager_get_window_by_name(wm, "old_name"));
    TEST_ASSERT_NOT_NULL(sdlapp_winmanager_get_window_by_name(wm, "new_name"));

    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje získání a nastavení velikosti okna */
void test_api_get_set_size(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "size_test", "T", 320, 240, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    int w = 0, h = 0;
    sdlapp_window_get_size(win, &w, &h);
    TEST_ASSERT_EQUAL_INT(320, w);
    TEST_ASSERT_EQUAL_INT(240, h);

    sdlapp_window_set_size(win, 640, 480);
    sdlapp_window_get_size(win, &w, &h);
    TEST_ASSERT_EQUAL_INT(640, w);
    TEST_ASSERT_EQUAL_INT(480, h);

    sdlapp_window_destroy(wm, win);
}

/* ========================================================================
 * CLOSE CALLBACK TESTY
 * ======================================================================== */

/** @brief Pomocný callback — odmítne zavření okna (vrátí FALSE) */
static gboolean close_cb_cancel(SdlAppWindow *win, gpointer user_data)
{
    (void)win;
    gboolean *called = (gboolean *)user_data;
    *called = TRUE;
    return FALSE; /* zrušit zavření */
}

/** @brief Pomocný callback — povolí zavření okna (vrátí TRUE) */
static gboolean close_cb_accept(SdlAppWindow *win, gpointer user_data)
{
    (void)win;
    gboolean *called = (gboolean *)user_data;
    *called = TRUE;
    return TRUE; /* zavřít */
}

/** @brief Testuje close callback, který zruší zavření okna */
void test_close_cancel(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "close_cancel", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    gboolean called = FALSE;
    sdlapp_window_set_close_requested_cb(win, close_cb_cancel, &called);

    /* ověříme, že callback je nastaven */
    SdlAppWindowCloseRequestedCb cb = sdlapp_window_get_close_requested_cb(win, NULL);
    TEST_ASSERT_EQUAL_PTR(close_cb_cancel, cb);

    /* okno stále existuje — smazat ručně */
    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje close callback, který povolí zavření okna */
void test_close_accept(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "close_accept", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    gboolean called = FALSE;
    sdlapp_window_set_close_requested_cb(win, close_cb_accept, &called);

    SdlAppWindowCloseRequestedCb cb = sdlapp_window_get_close_requested_cb(win, NULL);
    TEST_ASSERT_EQUAL_PTR(close_cb_accept, cb);

    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje výchozí chování bez close callbacku */
void test_close_no_callback_default(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "close_default", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    SdlAppWindowCloseRequestedCb cb = sdlapp_window_get_close_requested_cb(win, NULL);
    TEST_ASSERT_NULL(cb);

    sdlapp_window_destroy(wm, win);
}

/* ========================================================================
 * CUSTOM EVENTS
 * ======================================================================== */

/** @brief Testuje registraci custom eventu a jeho zpětné vyhledání */
void test_custom_event_register(void)
{
    Uint32 ev = sdlapp_register_event_type(g_app, "MY_EVENT");
    TEST_ASSERT_NOT_EQUAL(0, ev);

    /* opětovné vyhledání */
    Uint32 ev2 = sdlapp_get_event_type(g_app, "MY_EVENT");
    TEST_ASSERT_EQUAL_UINT32(ev, ev2);
}

/** @brief Testuje duplicitní registraci — vrátí stejný typ */
void test_custom_event_duplicate(void)
{
    Uint32 ev1 = sdlapp_register_event_type(g_app, "DUP_EVENT");
    Uint32 ev2 = sdlapp_register_event_type(g_app, "DUP_EVENT");
    TEST_ASSERT_EQUAL_UINT32(ev1, ev2);
}

/** @brief Testuje vyhledání neexistujícího eventu — vrátí 0 */
void test_custom_event_not_found(void)
{
    Uint32 ev = sdlapp_get_event_type(g_app, "NONEXISTENT");
    TEST_ASSERT_EQUAL_UINT32(0, ev);
}

/* ========================================================================
 * FRAME RATE CONTROL
 * ======================================================================== */

/** @brief Testuje výchozí loop mode (POLL) */
void test_fps_default_poll(void)
{
    TEST_ASSERT_EQUAL_INT(SDLAPP_LOOP_POLL, sdlapp_get_loop_mode(g_app));
}

/** @brief Testuje nastavení loop mode na WAIT */
void test_fps_set_wait(void)
{
    sdlapp_set_loop_mode(g_app, SDLAPP_LOOP_WAIT, 0);
    TEST_ASSERT_EQUAL_INT(SDLAPP_LOOP_WAIT, sdlapp_get_loop_mode(g_app));
}

/** @brief Testuje nastavení loop mode na WAIT_TIMEOUT */
void test_fps_set_wait_timeout(void)
{
    sdlapp_set_loop_mode(g_app, SDLAPP_LOOP_WAIT_TIMEOUT, 16);
    TEST_ASSERT_EQUAL_INT(SDLAPP_LOOP_WAIT_TIMEOUT, sdlapp_get_loop_mode(g_app));
    TEST_ASSERT_EQUAL_INT(16, g_app->wait_timeout_ms);
}

/** @brief Testuje nastavení a získání FPS limitu */
void test_fps_limit(void)
{
    TEST_ASSERT_EQUAL_UINT(0, sdlapp_get_fps_limit(g_app));
    sdlapp_set_fps_limit(g_app, 60);
    TEST_ASSERT_EQUAL_UINT(60, sdlapp_get_fps_limit(g_app));
}

/* ========================================================================
 * QUIT ON LAST WINDOW CLOSE
 * ======================================================================== */

/** @brief Testuje výchozí hodnotu quit_on_last_window_close (TRUE) */
void test_quit_on_last_default(void)
{
    TEST_ASSERT_TRUE(sdlapp_get_quit_on_last_window_close(g_app));
}

/** @brief Testuje nastavení quit_on_last_window_close na FALSE */
void test_quit_on_last_set(void)
{
    sdlapp_set_quit_on_last_window_close(g_app, FALSE);
    TEST_ASSERT_FALSE(sdlapp_get_quit_on_last_window_close(g_app));
}

/* ========================================================================
 * DRAG & DROP CALLBACK
 * ======================================================================== */

/** @brief Pomocný callback pro drag & drop test */
static void drop_file_cb(SdlAppWindow *win, const char *file_path, gpointer user_data)
{
    (void)win;
    (void)file_path;
    gboolean *called = (gboolean *)user_data;
    *called = TRUE;
}

/** @brief Testuje nastavení drag & drop callbacku na okno */
void test_drop_file_callback_set(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "drop_test", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    gboolean called = FALSE;
    sdlapp_window_set_drop_file_cb(win, drop_file_cb, &called);

    SdlAppWindowDropFileCb cb = sdlapp_window_get_drop_file_cb(win, NULL);
    TEST_ASSERT_EQUAL_PTR(drop_file_cb, cb);

    sdlapp_window_destroy(wm, win);
}

/* ========================================================================
 * TIMER TESTY
 * ======================================================================== */

/** @brief Testuje přidání a odebrání timeru */
void test_timer_add_remove(void)
{
    guint id = sdlapp_add_timer(g_app, 100, (SdlAppTimerCb)close_cb_accept, NULL);
    TEST_ASSERT_NOT_EQUAL(0, id);

    /* timer existuje v seznamu */
    TEST_ASSERT_NOT_NULL(g_app->timers);

    sdlapp_remove_timer(g_app, id);
    /* po odebrání je seznam prázdný */
    TEST_ASSERT_NULL(g_app->timers);
}

/** @brief Testuje přidání timeru s nulovou frekvencí — selže */
void test_timer_zero_interval(void)
{
    guint id = sdlapp_add_timer(g_app, 0, (SdlAppTimerCb)close_cb_accept, NULL);
    TEST_ASSERT_EQUAL_UINT(0, id);
}

/** @brief Testuje odebrání neexistujícího timeru — nesmí spadnout */
void test_timer_remove_nonexistent(void)
{
    sdlapp_remove_timer(g_app, 999);
}

/** @brief Počet volání callbacku timeru v test_timer_stale_event_*. */
static int s_stale_cb_calls = 0;

/** @brief Callback timeru, který jen počítá volání. */
static gboolean stale_count_cb(guint timer_id, gpointer user_data)
{
    (void)timer_id;
    (void)user_data;
    s_stale_cb_calls++;
    return TRUE;
}

/**
 * @brief Událost odebraného timeru, která ještě čeká ve frontě SDL, nesmí
 *        zavolat jiný timer.
 *
 * Timer A se odebere (g_free), zatímco jeho událost čeká ve frontě. Nový
 * timer B může od alokátoru dostat stejnou adresu, takže samotná kontrola
 * "ukazatel je v seznamu" by starou událost přisoudila timeru B. Test tuto
 * situaci vyrobí deterministicky: událost nese ID odebraného timeru A
 * (user.code), ale ukazatel živého timeru B (user.data1). Událost se smí
 * zpracovat, jen když sedí ukazatel i ID.
 */
void test_timer_stale_event_after_remove(void)
{
    s_stale_cb_calls = 0;
    sdlapp_set_fps_limit(g_app, 0);

    guint id_a = sdlapp_add_timer(g_app, 1000000, stale_count_cb, NULL);
    TEST_ASSERT_NOT_EQUAL(0, id_a);
    sdlapp_remove_timer(g_app, id_a);

    guint id_b = sdlapp_add_timer(g_app, 1000000, stale_count_cb, NULL);
    TEST_ASSERT_NOT_EQUAL(0, id_b);
    TEST_ASSERT_NOT_EQUAL(id_a, id_b);
    SdlAppTimer *timer_b = (SdlAppTimer *)g_app->timers->data;

    SDL_Event event;
    SDL_zero(event);
    event.type = g_app->timer_event_type;
    event.user.code = (Sint32)id_a;
    event.user.data1 = timer_b;
    TEST_ASSERT_TRUE(SDL_PushEvent(&event));

    sdlapp_iteration(g_app);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_stale_cb_calls,
                                  "stale event of a removed timer called another timer");

    /* kontrola: platná událost timeru B se zpracuje */
    event.user.code = (Sint32)id_b;
    TEST_ASSERT_TRUE(SDL_PushEvent(&event));
    sdlapp_iteration(g_app);
    TEST_ASSERT_EQUAL_INT(1, s_stale_cb_calls);

    sdlapp_remove_timer(g_app, id_b);
}

/* ========================================================================
 * REGRESNÍ TESTY — opravené bugy
 * ======================================================================== */

/** @brief Regresní test bug 1: sdlapp_window_activate (dříve setter) */
void test_bug1_activate(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "bug1", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    TEST_ASSERT_FALSE(sdlapp_window_get_active(win));
    sdlapp_window_activate(win);
    TEST_ASSERT_TRUE(sdlapp_window_get_active(win));

    sdlapp_window_destroy(wm, win);
}

/** @brief Regresní test bug 5: sdlapp_is_running(NULL) nesmí spadnout */
void test_bug5_null_safe(void)
{
    TEST_ASSERT_FALSE(sdlapp_is_running(NULL));
}

/** @brief Regresní test bug 6: get_name_dup vrátí bezpečnou kopii jména */
void test_bug6_name_dup(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    SdlAppWindow *win = sdlapp_window_create(wm, "bug6_name", "T", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);

    gchar *dup = sdlapp_window_get_name_dup(win);
    TEST_ASSERT_NOT_NULL(dup);
    TEST_ASSERT_EQUAL_STRING("bug6_name", dup);

    g_free(dup);
    sdlapp_window_destroy(wm, win);
}

/** @brief Testuje NULL bezpečnost všech getterů */
void test_null_getters(void)
{
    TEST_ASSERT_NULL(sdlapp_window_get_sdl_window(NULL));
    TEST_ASSERT_NULL(sdlapp_window_get_sdl_renderer(NULL));
    TEST_ASSERT_NULL(sdlapp_window_get_gl_context(NULL));
    TEST_ASSERT_NULL(sdlapp_window_get_manager(NULL));
    TEST_ASSERT_NULL(sdlapp_window_get_name_dup(NULL));
    TEST_ASSERT_EQUAL_INT(SDLAPP_WINDOW_STATE_DESTROYED, sdlapp_window_get_state(NULL));
    TEST_ASSERT_FALSE(sdlapp_window_get_active(NULL));
    TEST_ASSERT_NULL(sdlapp_window_get_gui_data(NULL));
    TEST_ASSERT_EQUAL_UINT(0, sdlapp_get_fps_limit(NULL));
    TEST_ASSERT_EQUAL_INT(SDLAPP_LOOP_POLL, sdlapp_get_loop_mode(NULL));
    TEST_ASSERT_NULL(sdlapp_get_winmanager(NULL));
}

/** @brief Testuje správný počet oken při vytváření a mazání */
void test_wm_window_count(void)
{
    SdlAppWindowManager *wm = sdlapp_get_winmanager(g_app);
    TEST_ASSERT_EQUAL_UINT(0, sdlapp_winmanager_get_window_count(wm));

    SdlAppWindow *w1 = sdlapp_window_create(wm, "cnt1", "A", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT(1, sdlapp_winmanager_get_window_count(wm));

    SdlAppWindow *w2 = sdlapp_window_create(wm, "cnt2", "B", 100, 100, 0, SDLAPP_RENDERER_SDL, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT(2, sdlapp_winmanager_get_window_count(wm));

    sdlapp_window_destroy(wm, w1);
    TEST_ASSERT_EQUAL_UINT(1, sdlapp_winmanager_get_window_count(wm));

    sdlapp_window_destroy(wm, w2);
    TEST_ASSERT_EQUAL_UINT(0, sdlapp_winmanager_get_window_count(wm));
}

/* ========================================================================
 * main — registrace testů
 * ======================================================================== */

/** @brief Hlavní funkce — registrace a spuštění všech testů */
int main(void)
{
    /* nastavíme dummy video driver pro headless testování */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");

    UNITY_BEGIN();

    /* smoke */
    RUN_TEST(test_smoke_new_destroy);
    RUN_TEST(test_smoke_is_running_null);
    RUN_TEST(test_smoke_is_running_default);
    RUN_TEST(test_smoke_quit);
    RUN_TEST(test_smoke_quit_null);

    /* window manager */
    RUN_TEST(test_wm_create_window);
    RUN_TEST(test_wm_get_by_name);
    RUN_TEST(test_wm_get_by_id);
    RUN_TEST(test_wm_get_by_regex);
    RUN_TEST(test_wm_duplicate_name);
    RUN_TEST(test_wm_window_count);

    /* stavový automat */
    RUN_TEST(test_state_after_create);
    RUN_TEST(test_state_activate);
    RUN_TEST(test_state_deactivate);

    /* window API */
    RUN_TEST(test_api_get_sdl_window);
    RUN_TEST(test_api_get_sdl_renderer);
    RUN_TEST(test_api_get_manager);
    RUN_TEST(test_api_set_title);
    RUN_TEST(test_api_get_name_dup);
    RUN_TEST(test_api_set_name);
    RUN_TEST(test_api_get_set_size);

    /* close callback */
    RUN_TEST(test_close_cancel);
    RUN_TEST(test_close_accept);
    RUN_TEST(test_close_no_callback_default);

    /* custom events */
    RUN_TEST(test_custom_event_register);
    RUN_TEST(test_custom_event_duplicate);
    RUN_TEST(test_custom_event_not_found);

    /* frame rate control */
    RUN_TEST(test_fps_default_poll);
    RUN_TEST(test_fps_set_wait);
    RUN_TEST(test_fps_set_wait_timeout);
    RUN_TEST(test_fps_limit);

    /* quit on last window close */
    RUN_TEST(test_quit_on_last_default);
    RUN_TEST(test_quit_on_last_set);

    /* drag & drop */
    RUN_TEST(test_drop_file_callback_set);

    /* timery */
    RUN_TEST(test_timer_add_remove);
    RUN_TEST(test_timer_zero_interval);
    RUN_TEST(test_timer_remove_nonexistent);
    RUN_TEST(test_timer_stale_event_after_remove);

    /* regresní */
    RUN_TEST(test_bug1_activate);
    RUN_TEST(test_bug5_null_safe);
    RUN_TEST(test_bug6_name_dup);
    RUN_TEST(test_null_getters);

    return UNITY_END();
}
