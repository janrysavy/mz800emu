/**
 * @file   sdlapp.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Implementace hlavní aplikace SdlApp.
 *
 * Obsahuje vytvoření a zničení aplikace, hlavní smyčku s event dispatchem,
 * řízení loop mode a FPS limitu, registraci custom eventů, správu timerů
 * a odesílání zpráv cíleným na konkrétní okna.
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

#include "sdlapp_int.h"
#include <SDL3/SDL.h>
#include <glib.h>

/* ======================================================================
 * Interní: SDL timer callback — doručí event do hlavního vlákna
 * ====================================================================== */

/**
 * @brief SDL timer callback — doručí custom event do hlavního vlákna.
 *
 * Tato funkce je volána z SDL timer vlákna. Vytvoří SDL_Event s timer_event_type
 * a vloží ho do fronty hlavního vlákna přes SDL_PushEvent.
 *
 * @param param Ukazatel na SdlAppTimer strukturu.
 * @param id SDL timer ID (nepoužívá se).
 * @param interval Aktuální interval timeru [ms].
 * @return Interval pro další opakování, nebo 0 pro zastavení.
 */
static Uint32 sdlapp_timer_sdl_callback(void *param, SDL_TimerID id, Uint32 interval)
{
    (void)id;
    SdlAppTimer *timer = (SdlAppTimer *)param;
    if (!timer || !timer->app)
        return 0;

    /* pošleme custom event do hlavního vlákna, kde se zavolá uživatelský callback */
    SDL_Event event;
    SDL_zero(event);
    event.type = timer->app->timer_event_type;
    event.user.code = (Sint32)timer->id;
    event.user.data1 = timer;
    SDL_PushEvent(&event);

    return interval;
}

/**
 * @brief Zpracuje timer event v hlavním vlákně.
 *
 * Událost může ve frontě SDL čekat i po odebrání timeru (sdlapp_remove_timer
 * strukturu uvolní). Ukazatel z události se proto nesmí dereferencovat,
 * dokud není nalezen v seznamu živých timerů, a i pak musí sedět ID timeru
 * (user.code): alokátor mohl stejnou adresu mezitím přidělit novému timeru.
 * Teprve potom se zavolá uživatelský callback. Pokud callback vrátí FALSE,
 * timer se automaticky odstraní.
 *
 * @param app Ukazatel na aplikaci.
 * @param event SDL event s timer daty (user.code = ID, user.data1 = timer).
 */
static void sdlapp_process_timer_event(SdlApp *app, SDL_Event *event)
{
    SdlAppTimer *timer = (SdlAppTimer *)event->user.data1;
    if (!timer)
        return;

    /* ověříme, že timer stále existuje v seznamu (bez dereference) */
    gboolean found = FALSE;
    for (GList *l = app->timers; l != NULL; l = l->next)
    {
        if (l->data == timer)
        {
            found = TRUE;
            break;
        }
    }

    /* stará událost odebraného timeru, jehož adresu dostal nový timer */
    if (!found || timer->id != (guint)event->user.code || !timer->callback)
        return;

    if (!timer->callback(timer->id, timer->user_data))
    {
        /* callback vrátil FALSE — zastavit timer */
        sdlapp_remove_timer(app, timer->id);
    }
}

/* ======================================================================
 * Vytvoření a inicializace
 * ====================================================================== */

/**
 * @brief Interní funkce pro vytvoření a inicializaci SdlApp instance.
 *
 * Alokuje strukturu, vytvoří window manager, inicializuje custom eventy,
 * timer subsystém a nastaví výchozí hodnoty.
 *
 * @return Ukazatel na novou instanci SdlApp, nebo NULL při selhání.
 */
static SdlApp *create_sdlapp(void)
{
    SdlApp *app = g_new0(SdlApp, 1);
    if (!app)
    {
        return NULL;
    }

    app->manager = sdlapp_winmanager_new(app);
    if (!app->manager)
    {
        g_free(app);
        return NULL;
    }

    app->running = FALSE;
    app->quit_requested = FALSE;
    app->loop_mode = SDLAPP_LOOP_POLL;
    app->wait_timeout_ms = 0;
    app->fps_limit = 0;
    app->last_frame_ticks = 0;
    app->quit_on_last_window_close = TRUE;
    app->custom_events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    app->timers = NULL;
    app->next_timer_id = 1;
    app->timer_event_type = SDL_RegisterEvents(1);

    /* Aplikační adresáře (HOME/CFG/WORK) — detekce binary dir, CLI overrides.
     * Předpoklad: sdlapp_options_init() už byl zavolaný (main.c). */
    app->paths = sdlapp_paths_new();

    return app;
}

/** @brief Vytvoří novou SDL aplikaci s SDL rendererem. */
SdlApp *sdlapp_new(void)
{
    if (!sdl3_backend_video_init())
    {
        SDLAPP_ERROR("Failed to initialize SDL video");
        return NULL;
    }

    SdlApp *app = create_sdlapp();
    if (!app)
    {
        sdl3_backend_video_quit();
        return NULL;
    }
    return app;
}

/** @brief Vytvoří novou SDL aplikaci s OpenGL backendem. */
SdlApp *sdlapp_opengl_new(void)
{
    if (!sdl3_backend_video_opengl_init())
    {
        SDLAPP_ERROR("Failed to initialize SDL video");
        return NULL;
    }

    SdlApp *app = create_sdlapp();
    if (!app)
    {
        sdl3_backend_video_quit();
        return NULL;
    }
    return app;
}

/* ======================================================================
 * Hlavní smyčka
 * ====================================================================== */

/**
 * @brief Zpracuje požadavek na zavření okna s volitelným callbackem.
 *
 * Najde okno podle windowID, zavolá close_requested callback (pokud je nastaven),
 * a pokud callback povolí zavření, zničí okno. Pokud nezbývají žádná okna
 * a quit_on_last_window_close je zapnut, ukončí hlavní smyčku.
 *
 * @param app Ukazatel na aplikaci.
 * @param event SDL event s informací o zavíraném okně.
 */
static void sdlapp_handle_window_close_requested(SdlApp *app, SDL_Event *event)
{
    SdlAppWindow *win = sdlapp_winmanager_get_window_by_SDL_WindowID(
        app->manager, event->window.windowID);

    if (!win)
        return; /* okno není spravované sdlapp (např. ImGui viewport) */

    /* zavoláme close_requested callback, pokud je nastaven */
    if (win->callbacks.close_requested)
    {
        if (!win->callbacks.close_requested(win, win->callbacks.close_requested_data))
        {
            return; /* callback vrátil FALSE — zrušit zavření */
        }
    }

    /* zavřít okno */
    sdlapp_window_destroy(win->manager, win);

    /* pokud nezbývají žádná okna a je zapnutý quit_on_last_window_close, ukončíme */
    if (app->quit_on_last_window_close)
    {
        if (sdlapp_winmanager_get_window_count(app->manager) == 0)
        {
            app->running = FALSE;
        }
    }
}

/**
 * @brief Zpracuje SDL eventy podle zvoleného loop mode.
 *
 * V POLL režimu zpracuje všechny čekající eventy, ve WAIT/WAIT_TIMEOUT
 * režimu nejprve čeká na první event a poté zpracuje zbylé pollem.
 * Každý event se předá window manageru a zároveň se kontrolují
 * speciální typy (QUIT, CLOSE_REQUESTED, timer event).
 *
 * @param app Ukazatel na aplikaci.
 * @return TRUE pokud byl zpracován alespoň jeden event, FALSE pokud ne.
 */
static gboolean sdlapp_poll_events(SdlApp *app)
{
    SDL_Event event;
    gboolean had_event = FALSE;

    /* načtení eventů podle zvoleného loop mode */
    switch (app->loop_mode)
    {
    case SDLAPP_LOOP_WAIT:
        if (SDL_WaitEvent(&event))
        {
            had_event = TRUE;
            goto process_event;
        }
        return FALSE;

    case SDLAPP_LOOP_WAIT_TIMEOUT:
        if (SDL_WaitEventTimeout(&event, app->wait_timeout_ms))
        {
            had_event = TRUE;
            goto process_event;
        }
        return FALSE;

    case SDLAPP_LOOP_POLL:
    default:
        break;
    }

    /* POLL režim — zpracujeme všechny čekající eventy */
    while (SDL_PollEvent(&event))
    {
        had_event = TRUE;

    process_event:
        sdlapp_winmanager_process_SDL_event_for_all(app->manager, &event);

        if (event.type == SDL_EVENT_QUIT)
        {
            app->running = FALSE;
        }

        /*
         * SDL_EVENT_WINDOW_CLOSE_REQUESTED: uživatel klikl na křížek okna.
         *
         * Když je zapnutý ImGui ViewportsEnable, ImGui vytváří vlastní
         * SDL okna pro viewport okna (floating ImGui windows). V tu chvíli
         * kliknutí na X hlavního okna nevygeneruje SDL_EVENT_QUIT
         * (protože existují další SDL okna), ale pouze
         * SDL_EVENT_WINDOW_CLOSE_REQUESTED.
         *
         * Kontrolujeme, zda event pochází z okna spravovaného sdlapp
         * (tj. hlavní aplikační okno). Pokud ano, zavoláme close callback
         * a případně ukončíme aplikaci.
         */
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        {
            sdlapp_handle_window_close_requested(app, &event);
        }

        /* timer event — zpracování callbacku v hlavním vlákně */
        if (event.type == app->timer_event_type)
        {
            sdlapp_process_timer_event(app, &event);
        }

        /* v WAIT/WAIT_TIMEOUT režimu zpracujeme jen jeden event, pak zbylé pollem */
        if (app->loop_mode != SDLAPP_LOOP_POLL)
        {
            while (SDL_PollEvent(&event))
            {
                sdlapp_winmanager_process_SDL_event_for_all(app->manager, &event);

                if (event.type == SDL_EVENT_QUIT)
                    app->running = FALSE;
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    sdlapp_handle_window_close_requested(app, &event);
                if (event.type == app->timer_event_type)
                    sdlapp_process_timer_event(app, &event);
            }
            break;
        }
    }

    return had_event;
}

/** @brief Provede jednu iteraci hlavní smyčky (eventy + render + FPS limiter). */
void sdlapp_iteration(SdlApp *app)
{
    sdlapp_poll_events(app);

    sdlapp_winmanager_process_render_for_all(app->manager);

    /* FPS limit — čekáme na zbývající čas do dalšího snímku */
    if (app->fps_limit > 0)
    {
        Uint64 now = SDL_GetTicksNS();
        Uint64 frame_ns = 1000000000ULL / app->fps_limit;

        if (app->last_frame_ticks > 0)
        {
            Uint64 elapsed = now - app->last_frame_ticks;
            if (elapsed < frame_ns)
            {
                SDL_DelayNS(frame_ns - elapsed);
            }
        }

        app->last_frame_ticks = SDL_GetTicksNS();
    }
}

/** @brief Spustí hlavní smyčku aplikace. */
void sdlapp_run(SdlApp *app)
{
    app->running = TRUE;

    /* Respektuj i quit_requested - pokud bylo ukončení vyžádáno ještě
     * před spuštěním smyčky (running se mezitím nastaví na TRUE), smyčka
     * se nesmí rozběhnout. */
    while (app->running && !app->quit_requested)
    {
        sdlapp_iteration(app);
    }
}

/* ======================================================================
 * Ukončení a zničení
 * ====================================================================== */

/** @brief Požádá o ukončení hlavní smyčky (nastaví quit_requested=TRUE
 *         a running=FALSE). */
void sdlapp_quit(SdlApp *app)
{
    if (app)
    {
        app->quit_requested = TRUE;
        app->running = FALSE;
    }
}

/** @brief Zjistí, zda hlavní smyčka aplikace běží. */
gboolean sdlapp_is_running(SdlApp *app)
{
    if (!app)
        return FALSE;
    return app->running;
}

/** @brief Zjistí, zda bylo vyžádáno ukončení aplikace (nezávisle na running). */
gboolean sdlapp_is_quit_requested(SdlApp *app)
{
    if (!app)
        return FALSE;
    return app->quit_requested;
}

/** @brief Zničí aplikaci a uvolní všechny zdroje. */
void sdlapp_destroy(SdlApp *app)
{
    if (!app)
        return;

    /* zastavíme a uvolníme všechny timery */
    GList *l = app->timers;
    while (l)
    {
        SdlAppTimer *timer = (SdlAppTimer *)l->data;
        GList *next = l->next;
        SDL_RemoveTimer(timer->sdl_id);
        g_free(timer);
        l = next;
    }
    g_list_free(app->timers);
    app->timers = NULL;

    /* uvolníme custom events tabulku */
    if (app->custom_events)
    {
        g_hash_table_destroy(app->custom_events);
        app->custom_events = NULL;
    }

    sdlapp_winmanager_destroy(app->manager);

    /* Aplikační adresáře */
    sdlapp_paths_destroy(app->paths);
    app->paths = NULL;

    sdl3_backend_video_quit();
    g_free(app);
}

/* ======================================================================
 * Loop mode a FPS limit
 * ====================================================================== */

/** @brief Nastaví režim hlavní smyčky. */
void sdlapp_set_loop_mode(SdlApp *app, SdlAppLoopMode mode, gint32 timeout_ms)
{
    if (!app)
        return;
    app->loop_mode = mode;
    app->wait_timeout_ms = timeout_ms;
}

/** @brief Zjistí aktuální režim hlavní smyčky. */
SdlAppLoopMode sdlapp_get_loop_mode(SdlApp *app)
{
    if (!app)
        return SDLAPP_LOOP_POLL;
    return app->loop_mode;
}

/** @brief Nastaví FPS limit (0 = bez limitu). */
void sdlapp_set_fps_limit(SdlApp *app, guint fps)
{
    if (!app)
        return;
    app->fps_limit = fps;
    app->last_frame_ticks = 0;
}

/** @brief Zjistí aktuální FPS limit. */
guint sdlapp_get_fps_limit(SdlApp *app)
{
    if (!app)
        return 0;
    return app->fps_limit;
}

/* ======================================================================
 * Chování při zavření posledního okna
 * ====================================================================== */

/** @brief Nastaví automatické ukončení při zavření posledního okna. */
void sdlapp_set_quit_on_last_window_close(SdlApp *app, gboolean quit)
{
    if (!app)
        return;
    app->quit_on_last_window_close = quit;
}

/** @brief Zjistí nastavení automatického ukončení při zavření posledního okna. */
gboolean sdlapp_get_quit_on_last_window_close(SdlApp *app)
{
    if (!app)
        return TRUE;
    return app->quit_on_last_window_close;
}

/* ======================================================================
 * Window manager
 * ====================================================================== */

/** @brief Vrátí ukazatel na window manager aplikace. */
SdlAppWindowManager *sdlapp_get_winmanager(SdlApp *app)
{
    if (!app)
        return NULL;
    return app->manager;
}

/* ======================================================================
 * Odesílání zpráv
 * ====================================================================== */

/** @brief Odešle SDL user event cílený na konkrétní okno podle SDL WindowID. */
gboolean sdlapp_send_message_for_SDL_windowID(SDL_WindowID win_id, Uint32 type, Sint32 code, gpointer data1, gpointer data2)
{
    SDL_Event event;
    SDL_zero(event);

    event.type = type;
    event.user.windowID = win_id;
    event.user.code = code;
    event.user.data1 = data1;
    event.user.data2 = data2;

    return SDL_PushEvent(&event);
}

/* ======================================================================
 * Custom event registration
 * ====================================================================== */

/** @brief Zaregistruje nový pojmenovaný SDL custom event typ. */
Uint32 sdlapp_register_event_type(SdlApp *app, const gchar *name)
{
    if (!app || !name)
        return 0;

    /* zkontrolujeme, jestli event s tímto jménem už neexistuje */
    gpointer existing = g_hash_table_lookup(app->custom_events, name);
    if (existing)
    {
        WARN("Event type '%s' already registered", name);
        return GPOINTER_TO_UINT(existing);
    }

    Uint32 event_type = SDL_RegisterEvents(1);
    if (event_type == 0)
    {
        SDLAPP_ERROR("Failed to register SDL event for '%s'", name);
        return 0;
    }

    g_hash_table_insert(app->custom_events, g_strdup(name), GUINT_TO_POINTER(event_type));
    return event_type;
}

/** @brief Vyhledá dříve zaregistrovaný event typ podle jména. */
Uint32 sdlapp_get_event_type(SdlApp *app, const gchar *name)
{
    if (!app || !name)
        return 0;

    gpointer value = g_hash_table_lookup(app->custom_events, name);
    if (!value)
        return 0;

    return GPOINTER_TO_UINT(value);
}

/* ======================================================================
 * Timery
 * ====================================================================== */

/** @brief Přidá periodický timer s callbackem volaným v hlavním vlákně. */
guint sdlapp_add_timer(SdlApp *app, guint interval_ms, SdlAppTimerCb cb, gpointer user_data)
{
    if (!app || !cb || interval_ms == 0)
        return 0;

    SdlAppTimer *timer = g_new0(SdlAppTimer, 1);
    timer->id = app->next_timer_id++;
    timer->interval_ms = interval_ms;
    timer->callback = cb;
    timer->user_data = user_data;
    timer->app = app;

    timer->sdl_id = SDL_AddTimer(interval_ms, sdlapp_timer_sdl_callback, timer);
    if (timer->sdl_id == 0)
    {
        SDLAPP_ERROR("Failed to create SDL timer: %s", SDL_GetError());
        g_free(timer);
        return 0;
    }

    app->timers = g_list_append(app->timers, timer);
    return timer->id;
}

/** @brief Zastaví a odstraní timer podle ID. */
void sdlapp_remove_timer(SdlApp *app, guint timer_id)
{
    if (!app || timer_id == 0)
        return;

    for (GList *l = app->timers; l != NULL; l = l->next)
    {
        SdlAppTimer *timer = (SdlAppTimer *)l->data;
        if (timer->id == timer_id)
        {
            SDL_RemoveTimer(timer->sdl_id);
            app->timers = g_list_delete_link(app->timers, l);
            g_free(timer);
            return;
        }
    }
}
