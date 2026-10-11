/**
 * @file   sdlapp_window.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @version 2.0.0
 * @brief  Implementace aplikačních oken pro SdlApp.
 *
 * Obsahuje vytvoření a zničení oken (standardní, popup, properties),
 * validaci stavového automatu, vytvoření rendereru, zpracování eventů
 * a renderingu, odesílání zpráv, správu jména, aktivity, GUI dat,
 * SDL handle getterů, window state wrapperů a callbacků.
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

#include "sdlapp_window.h"
#include "sdlapp_winmanager_int.h"
#include <stdlib.h>
#include <glib.h>
#include <SDL3/SDL.h>

/* ======================================================================
 * Validace přechodu stavového automatu
 * ====================================================================== */

/**
 * @brief Validuje přechod stavového automatu okna.
 *
 * Kontroluje, zda je přechod z jednoho stavu do druhého povolený
 * podle definovaných pravidel životního cyklu okna.
 *
 * @param from Aktuální stav.
 * @param to Cílový stav.
 * @return TRUE pokud je přechod povolený, FALSE jinak.
 */
static gboolean sdlapp_window_validate_state_transition(SdlAppWindowState from, SdlAppWindowState to)
{
    switch (from)
    {
    case SDLAPP_WINDOW_STATE_CREATED:
        return (to == SDLAPP_WINDOW_STATE_INITIALIZED || to == SDLAPP_WINDOW_STATE_DESTROYING);
    case SDLAPP_WINDOW_STATE_INITIALIZED:
        return (to == SDLAPP_WINDOW_STATE_ACTIVE || to == SDLAPP_WINDOW_STATE_DESTROYING);
    case SDLAPP_WINDOW_STATE_ACTIVE:
        return (to == SDLAPP_WINDOW_STATE_PAUSED || to == SDLAPP_WINDOW_STATE_DESTROYING);
    case SDLAPP_WINDOW_STATE_PAUSED:
        return (to == SDLAPP_WINDOW_STATE_ACTIVE || to == SDLAPP_WINDOW_STATE_DESTROYING);
    case SDLAPP_WINDOW_STATE_DESTROYING:
        return (to == SDLAPP_WINDOW_STATE_DESTROYED);
    case SDLAPP_WINDOW_STATE_DESTROYED:
        return FALSE;
    default:
        return FALSE;
    }
}

/**
 * @brief Nastaví nový stav okna s validací přechodu.
 *
 * Pokud přechod není povolený, vypíše varování a stav nezmění.
 *
 * @param win Ukazatel na okno.
 * @param new_state Nový požadovaný stav.
 */
static void sdlapp_window_set_state(SdlAppWindow *win, SdlAppWindowState new_state)
{
    if (!win)
        return;

    if (!sdlapp_window_validate_state_transition(win->state, new_state))
    {
        WARN("Invalid window state transition '%s': %d -> %d", win->name ? win->name : "?", win->state, new_state);
        return;
    }

    win->state = new_state;
}

/* ======================================================================
 * Vytvoření rendereru
 * ====================================================================== */

/**
 * @brief Vytvoří renderer pro okno podle zvoleného typu.
 *
 * Pro SDLAPP_RENDERER_SDL vytvoří SDL_Renderer, pro SDLAPP_RENDERER_OPENGL
 * vytvoří OpenGL kontext.
 *
 * @param win Ukazatel na okno.
 * @param type Požadovaný typ rendereru.
 * @return TRUE při úspěchu, FALSE při selhání.
 */
static gboolean create_renderer(SdlAppWindow *win, SdlAppRendererType type)
{
    if (!win)
        return FALSE;

    gboolean result = TRUE;

    win->renderer_type = type;
    switch (type)
    {
    case SDLAPP_RENDERER_SDL:
        win->sdl_renderer = SDL_CreateRenderer(win->sdl_window, SDL_GetHint(SDL_HINT_RENDER_DRIVER));
        if (!win->sdl_renderer)
        {
            result = FALSE;
            SDLAPP_ERROR("Can't create SDL_Renderer: %s", SDL_GetError());
        }
        break;

    case SDLAPP_RENDERER_OPENGL:
        win->gl_context = SDL_GL_CreateContext(win->sdl_window);
        if (!win->gl_context)
        {
            result = FALSE;
            SDLAPP_ERROR("Can't create OpenGL context: %s", SDL_GetError());
        }
        break;

    default:
        result = FALSE;
        SDLAPP_ERROR("Unsupported renderer type: %d", type);
        break;
    }

    return result;
}

/* ======================================================================
 * Interní vytvoření okna
 * ====================================================================== */

/**
 * @brief Interní funkce pro vytvoření aplikačního okna.
 *
 * Alokuje SdlAppWindow, vytvoří renderer, zaregistruje v window manageru,
 * zavolá init_callback a nastaví stav na INITIALIZED. Vynucuje OpenGL
 * singleton — maximálně jedno OpenGL okno.
 *
 * @param wm Window manager.
 * @param name Unikátní název okna.
 * @param sdl_window Již vytvořené SDL okno.
 * @param parent Rodičovské okno (může být NULL).
 * @param type Typ renderovacího backendu.
 * @param init_callback Callback pro inicializaci (může být NULL).
 * @param init_user_data Data pro init_callback.
 * @return Ukazatel na nové okno, nebo NULL při selhání.
 */
static SdlAppWindow *sdlapp_window_create_internal(SdlAppWindowManager *wm, const char *name, SDL_Window *sdl_window, SdlAppWindow *parent, SdlAppRendererType type, SdlAppWindowInitCb init_callback, gpointer init_user_data)
{
    if (!wm)
        return NULL;

    if (!name)
    {
        SDLAPP_ERROR("Window name is NULL");
        return NULL;
    }

    if (!sdl_window)
    {
        SDLAPP_ERROR("SDL_Window is NULL");
        return NULL;
    }

    /* OpenGL singleton enforcement — max jedno OpenGL okno */
    if (type == SDLAPP_RENDERER_OPENGL)
    {
        if (sdlapp_winmanager_has_opengl_window(wm))
        {
            SDLAPP_ERROR("OpenGL window already exists — only one OpenGL window is allowed");
            SDL_DestroyWindow(sdl_window);
            return NULL;
        }
    }

    SdlAppWindow *win = g_new0(SdlAppWindow, 1);
    win->manager = wm;
    win->name = g_strdup(name);
    g_mutex_init(&win->mutex);
    win->parent = parent;
    win->is_active = FALSE;
    win->state = SDLAPP_WINDOW_STATE_CREATED;
    win->callbacks.init = init_callback;
    win->callbacks.init_data = init_user_data;
    win->sdl_window = sdl_window;

    if (!create_renderer(win, type))
    {
        SDL_DestroyWindow(win->sdl_window);
        g_mutex_clear(&win->mutex);
        g_free(win->name);
        g_free(win);
        return NULL;
    }

    if (!sdlapp_winmanager_register_window(wm, win))
    {
        sdlapp_window_destroy(wm, win);
        return NULL;
    }

    if ((init_callback) && (!init_callback(win, init_user_data)))
    {
        sdlapp_window_destroy(wm, win);
        return NULL;
    }

    /* init_callback úspěšně dokončen (nebo nebyl zadán) */
    sdlapp_window_set_state(win, SDLAPP_WINDOW_STATE_INITIALIZED);

    return win;
}

/* ======================================================================
 * Veřejné funkce pro vytvoření oken
 * ====================================================================== */

/** @brief Vytvoří nové aplikační okno s automatickým pozicováním. */
SdlAppWindow *sdlapp_window_create(SdlAppWindowManager *wm, const char *name, const char *title, int width, int height, Uint32 flags, SdlAppRendererType type, SdlAppWindowInitCb init_callback, gpointer init_user_data)
{
    if (!title)
    {
        title = (name) ? name : "SDLApp Window";
    }

    SDL_Window *sdl_window = SDL_CreateWindow(title, width, height, flags);
    if (!sdl_window)
    {
        SDLAPP_ERROR("Can't create SDL_Window: %s", SDL_GetError());
        return NULL;
    }

    SdlAppWindow *win = sdlapp_window_create_internal(wm, name, sdl_window, NULL, type, init_callback, init_user_data);
    if (win)
    {
        int x = 0, y = 0;
        sdlapp_winmanager_good_place_for(wm, win, &x, &y);
        SDL_SetWindowPosition(win->sdl_window, x, y);
    }

    return win;
}

/** @brief Vytvoří popup okno relativně k rodičovskému oknu. */
SdlAppWindow *sdlapp_window_create_popup(SdlAppWindowManager *wm, const char *name, SdlAppWindow *parent, int offset_x, int offset_y, int width, int height, Uint32 flags, SdlAppRendererType type, SdlAppWindowInitCb init_callback, gpointer init_user_data)
{
    if (!parent)
    {
        SDLAPP_ERROR("Parent window is NULL");
        return NULL;
    }

    if (!parent->sdl_window)
    {
        SDLAPP_ERROR("Parent SDL_Window is NULL");
        return NULL;
    }

    SDL_Window *sdl_window = SDL_CreatePopupWindow(parent->sdl_window, offset_x, offset_y, width, height, flags);
    if (!sdl_window)
    {
        SDLAPP_ERROR("Can't create SDL_Window: %s", SDL_GetError());
        return NULL;
    }

    return sdlapp_window_create_internal(wm, name, sdl_window, parent, type, init_callback, init_user_data);
}

/** @brief Vytvoří okno přes SDL_CreateWindowWithProperties. */
SdlAppWindow *sdlapp_window_create_with_properties(SdlAppWindowManager *wm, const char *name, SdlAppWindow *parent, SDL_PropertiesID props, SdlAppRendererType type, SdlAppWindowInitCb init_callback, gpointer init_user_data)
{
    SDL_Window *sdl_window = SDL_CreateWindowWithProperties(props);
    if (!sdl_window)
    {
        SDLAPP_ERROR("Can't create SDL_Window: %s", SDL_GetError());
        return NULL;
    }

    return sdlapp_window_create_internal(wm, name, sdl_window, parent, type, init_callback, init_user_data);
}

/* ======================================================================
 * Zničení okna
 * ====================================================================== */

/** @brief Zničí aplikační okno a uvolní všechny jeho zdroje. */
void sdlapp_window_destroy(SdlAppWindowManager *wm, SdlAppWindow *window)
{
    if (!wm || !window)
        return;

    g_mutex_lock(&window->mutex);

    sdlapp_window_set_state(window, SDLAPP_WINDOW_STATE_DESTROYING);
    window->is_active = FALSE;

    if (window->callbacks.destroy)
        window->callbacks.destroy(window, window->callbacks.destroy_data);

    sdlapp_winmanager_unregister_window(wm, window);

    switch (window->renderer_type)
    {
    case SDLAPP_RENDERER_SDL:
        if (window->sdl_renderer)
        {
            SDL_DestroyRenderer(window->sdl_renderer);
        }
        break;

    case SDLAPP_RENDERER_OPENGL:
        if (window->gl_context)
        {
            SDL_GL_DestroyContext(window->gl_context);
        }
        break;

    default:
        break;
    }

    SDL_DestroyWindow(window->sdl_window);

    window->state = SDLAPP_WINDOW_STATE_DESTROYED;

    g_free(window->name);
    g_mutex_unlock(&window->mutex);
    g_mutex_clear(&window->mutex);
    g_free(window);
}

/* ======================================================================
 * Zpracování eventů a renderingu
 * ====================================================================== */

/** @brief Zpracuje SDL event pro konkrétní okno (callback request, drop, event_cb). */
void sdlapp_window_process_event(SdlAppWindow *win, SDL_Event *event)
{
    if (!win)
        return;

    /* callback request — inter-thread volání */
    if ((event->type == SDLAPP_WINDOW_EVENT_WINDOW_CALLBACK_REQUEST) && (event->user.windowID == SDL_GetWindowID(win->sdl_window)))
    {
        if (event->user.data1)
        {
            SdlAppWindowCallMeRqCb callme = (SdlAppWindowCallMeRqCb)event->user.data1;
            if (callme)
            {
                callme(win, event);
            }
            else
            {
                SDLAPP_ERROR("CallMe request callback is NULL");
            }
        }
    }

    /* drag & drop souboru */
    if (event->type == SDL_EVENT_DROP_FILE)
    {
        if (event->drop.windowID == SDL_GetWindowID(win->sdl_window))
        {
            if (win->callbacks.drop_file && event->drop.data)
            {
                win->callbacks.drop_file(win, event->drop.data, win->callbacks.drop_file_data);
            }
        }
    }

    /* předání eventu uživatelskému callbacku */
    if ((win->is_active) && (win->callbacks.event))
        win->callbacks.event(win, event, win->gui_data);
}

/** @brief Provede rendering pro okno (render_cb nebo výchozí render). */
void sdlapp_window_process_render(SdlAppWindow *win)
{
    if ((!win) || (!win->sdl_window) || (!win->is_active))
        return;

    if (win->callbacks.render)
    {
        win->callbacks.render(win, win->callbacks.render_data);
        return;
    }

    /* provedeme zástupný render */
    switch (win->renderer_type)
    {
    case SDLAPP_RENDERER_SDL:
        SDL_SetRenderDrawColor(win->sdl_renderer, 0, 0, 0, 255);
        SDL_RenderClear(win->sdl_renderer);
        SDL_RenderPresent(win->sdl_renderer);
        break;

    case SDLAPP_RENDERER_OPENGL:
        SDL_GL_MakeCurrent(win->sdl_window, win->gl_context);
        SDL_GL_SwapWindow(win->sdl_window);
        break;

    default:
        break;
    }
}

/* ======================================================================
 * Odesílání zpráv
 * ====================================================================== */

/** @brief Odešle SDL user event cílený na toto okno. */
gboolean sdlapp_window_send_message(SdlAppWindow *win, Uint32 type, Sint32 code, gpointer data1, gpointer data2)
{
    if ((!win) || (!win->sdl_window))
        return FALSE;

    return sdlapp_send_message_for_SDL_windowID(SDL_GetWindowID(win->sdl_window), type, code, data1, data2);
}

/* ======================================================================
 * Jméno okna
 * ====================================================================== */

/** @brief Vrátí interní ukazatel na jméno okna (pozor: není thread-safe na životnost ukazatele). */
const char *sdlapp_window_get_name(SdlAppWindow *win)
{
    g_mutex_lock(&win->mutex);
    const char *name = win->name;
    g_mutex_unlock(&win->mutex);
    return name;
}

/** @brief Vrátí thread-safe kopii jména okna (volající musí uvolnit přes g_free). */
gchar *sdlapp_window_get_name_dup(SdlAppWindow *win)
{
    if (!win)
        return NULL;

    g_mutex_lock(&win->mutex);
    gchar *name = g_strdup(win->name);
    g_mutex_unlock(&win->mutex);
    return name;
}

/** @brief Přejmenuje okno (selže při prázdném nebo duplicitním jméně). */
gboolean sdlapp_window_set_name(SdlAppWindow *win, const char *name)
{
    if (!win)
        return FALSE;

    g_mutex_lock(&win->mutex);
    gboolean result = sdlapp_winmanager_change_window_name(win->manager, win, name);
    g_mutex_unlock(&win->mutex);
    return result;
}

/* ======================================================================
 * Aktivita okna
 * ====================================================================== */

/** @brief Zjistí, zda je okno aktivní. Thread-safe. */
gboolean sdlapp_window_get_active(SdlAppWindow *win)
{
    if (!win)
        return FALSE;

    g_mutex_lock(&win->mutex);
    gboolean active = win->is_active;
    g_mutex_unlock(&win->mutex);
    return active;
}

/** @brief Aktivuje okno (přechod INITIALIZED/PAUSED → ACTIVE). Thread-safe. */
void sdlapp_window_activate(SdlAppWindow *win)
{
    if (!win)
        return;

    g_mutex_lock(&win->mutex);
    win->is_active = TRUE;
    /* přechod INITIALIZED→ACTIVE nebo PAUSED→ACTIVE */
    if (win->state == SDLAPP_WINDOW_STATE_INITIALIZED || win->state == SDLAPP_WINDOW_STATE_PAUSED)
    {
        sdlapp_window_set_state(win, SDLAPP_WINDOW_STATE_ACTIVE);
    }
    g_mutex_unlock(&win->mutex);
}

/** @brief Nastaví/odebere aktivitu okna s odpovídajícím přechodem stavového automatu. Thread-safe. */
void sdlapp_window_set_active(SdlAppWindow *win, gboolean active)
{
    if (!win)
        return;

    g_mutex_lock(&win->mutex);
    win->is_active = active;
    if (active)
    {
        if (win->state == SDLAPP_WINDOW_STATE_INITIALIZED || win->state == SDLAPP_WINDOW_STATE_PAUSED)
            sdlapp_window_set_state(win, SDLAPP_WINDOW_STATE_ACTIVE);
    }
    else
    {
        if (win->state == SDLAPP_WINDOW_STATE_ACTIVE)
            sdlapp_window_set_state(win, SDLAPP_WINDOW_STATE_PAUSED);
    }
    g_mutex_unlock(&win->mutex);
}

/* ======================================================================
 * Stav okna (lifecycle state machine)
 * ====================================================================== */

/** @brief Vrátí aktuální stav okna ve stavovém automatu. Thread-safe. */
SdlAppWindowState sdlapp_window_get_state(SdlAppWindow *win)
{
    if (!win)
        return SDLAPP_WINDOW_STATE_DESTROYED;

    g_mutex_lock(&win->mutex);
    SdlAppWindowState state = win->state;
    g_mutex_unlock(&win->mutex);
    return state;
}

/* ======================================================================
 * GUI data
 * ====================================================================== */

/** @brief Vrátí uživatelská GUI data asociovaná s oknem. Thread-safe. */
gpointer sdlapp_window_get_gui_data(SdlAppWindow *win)
{
    if (!win)
        return NULL;

    g_mutex_lock(&win->mutex);
    gpointer gui_data = win->gui_data;
    g_mutex_unlock(&win->mutex);
    return gui_data;
}

/** @brief Nastaví GUI data a vrátí předchozí hodnotu. Thread-safe. */
gpointer sdlapp_window_set_gui_data(SdlAppWindow *win, gpointer gui_data)
{
    if (!win)
        return NULL;

    g_mutex_lock(&win->mutex);
    gpointer old_data = win->gui_data;
    win->gui_data = gui_data;
    g_mutex_unlock(&win->mutex);
    return old_data;
}

/* ======================================================================
 * Gettery pro SDL handles
 * ====================================================================== */

/** @brief Vrátí SDL_Window handle okna. */
SDL_Window *sdlapp_window_get_sdl_window(SdlAppWindow *win)
{
    if (!win)
        return NULL;
    return win->sdl_window;
}

/** @brief Vrátí SDL renderer okna (NULL pokud okno používá OpenGL). */
SDL_Renderer *sdlapp_window_get_sdl_renderer(SdlAppWindow *win)
{
    if (!win || win->renderer_type != SDLAPP_RENDERER_SDL)
        return NULL;
    return win->sdl_renderer;
}

/** @brief Vrátí OpenGL kontext okna (NULL pokud okno používá SDL renderer). */
SDL_GLContext sdlapp_window_get_gl_context(SdlAppWindow *win)
{
    if (!win || win->renderer_type != SDLAPP_RENDERER_OPENGL)
        return NULL;
    return win->gl_context;
}

/** @brief Vrátí typ renderovacího backendu okna. */
SdlAppRendererType sdlapp_window_get_renderer_type(SdlAppWindow *win)
{
    if (!win)
        return SDLAPP_RENDERER_SDL;
    return win->renderer_type;
}

/** @brief Vrátí window manager, ke kterému okno patří. */
SdlAppWindowManager *sdlapp_window_get_manager(SdlAppWindow *win)
{
    if (!win)
        return NULL;
    return win->manager;
}

/* ======================================================================
 * Window state wrappery
 * ====================================================================== */

/** @brief Zjistí velikost okna. */
void sdlapp_window_get_size(SdlAppWindow *win, int *w, int *h)
{
    if (!win || !win->sdl_window)
        return;
    SDL_GetWindowSize(win->sdl_window, w, h);
}

/** @brief Nastaví velikost okna. */
void sdlapp_window_set_size(SdlAppWindow *win, int w, int h)
{
    if (!win || !win->sdl_window)
        return;
    SDL_SetWindowSize(win->sdl_window, w, h);
}

/** @brief Zjistí pozici okna. */
void sdlapp_window_get_position(SdlAppWindow *win, int *x, int *y)
{
    if (!win || !win->sdl_window)
        return;
    SDL_GetWindowPosition(win->sdl_window, x, y);
}

/** @brief Nastaví pozici okna. */
void sdlapp_window_set_position(SdlAppWindow *win, int x, int y)
{
    if (!win || !win->sdl_window)
        return;
    SDL_SetWindowPosition(win->sdl_window, x, y);
}

/** @brief Nastaví režim celé obrazovky. */
void sdlapp_window_set_fullscreen(SdlAppWindow *win, gboolean fullscreen)
{
    if (!win || !win->sdl_window)
        return;
    SDL_SetWindowFullscreen(win->sdl_window, fullscreen ? TRUE : FALSE);
}

/** @brief Zjistí, zda je okno v režimu celé obrazovky. */
gboolean sdlapp_window_get_fullscreen(SdlAppWindow *win)
{
    if (!win || !win->sdl_window)
        return FALSE;
    return (SDL_GetWindowFlags(win->sdl_window) & SDL_WINDOW_FULLSCREEN) ? TRUE : FALSE;
}

/** @brief Nastaví titulek okna. */
void sdlapp_window_set_title(SdlAppWindow *win, const char *title)
{
    if (!win || !win->sdl_window || !title)
        return;
    SDL_SetWindowTitle(win->sdl_window, title);
}

/** @brief Vrátí titulek okna. */
const char *sdlapp_window_get_title(SdlAppWindow *win)
{
    if (!win || !win->sdl_window)
        return NULL;
    return SDL_GetWindowTitle(win->sdl_window);
}

/** @brief Minimalizuje okno. */
void sdlapp_window_minimize(SdlAppWindow *win)
{
    if (!win || !win->sdl_window)
        return;
    SDL_MinimizeWindow(win->sdl_window);
}

/** @brief Maximalizuje okno. */
void sdlapp_window_maximize(SdlAppWindow *win)
{
    if (!win || !win->sdl_window)
        return;
    SDL_MaximizeWindow(win->sdl_window);
}

/** @brief Obnoví okno z minimalizace/maximalizace. */
void sdlapp_window_restore(SdlAppWindow *win)
{
    if (!win || !win->sdl_window)
        return;
    SDL_RestoreWindow(win->sdl_window);
}

/* ======================================================================
 * Callbacky
 * ====================================================================== */

/** @brief Vrátí init callback a jeho data. Thread-safe. */
SdlAppWindowInitCb sdlapp_window_get_init_cb(SdlAppWindow *win, gpointer *init_data)
{
    if (!win)
        return NULL;

    g_mutex_lock(&win->mutex);
    SdlAppWindowInitCb init_cb = win->callbacks.init;
    if (init_data)
        *init_data = win->callbacks.init_data;
    g_mutex_unlock(&win->mutex);
    return init_cb;
}

/** @brief Nastaví event callback. Thread-safe. */
void sdlapp_window_set_event_cb(SdlAppWindow *win, SdlAppWindowEventCb event_cb, gpointer event_data)
{
    if (!win)
        return;

    g_mutex_lock(&win->mutex);
    win->callbacks.event = event_cb;
    win->callbacks.event_data = event_data;
    g_mutex_unlock(&win->mutex);
}

/** @brief Vrátí event callback a jeho data. Thread-safe. */
SdlAppWindowEventCb sdlapp_window_get_event_cb(SdlAppWindow *win, gpointer *event_data)
{
    if (!win)
        return NULL;

    g_mutex_lock(&win->mutex);
    SdlAppWindowEventCb event_cb = win->callbacks.event;
    if (event_data)
        *event_data = win->callbacks.event_data;
    g_mutex_unlock(&win->mutex);
    return event_cb;
}

/** @brief Nastaví render callback. Thread-safe. */
void sdlapp_window_set_render_cb(SdlAppWindow *win, SdlAppWindowRenderCb render_cb, gpointer render_data)
{
    if (!win)
        return;

    g_mutex_lock(&win->mutex);
    win->callbacks.render = render_cb;
    win->callbacks.render_data = render_data;
    g_mutex_unlock(&win->mutex);
}

/** @brief Vrátí render callback a jeho data. Thread-safe. */
SdlAppWindowRenderCb sdlapp_window_get_render_cb(SdlAppWindow *win, gpointer *render_data)
{
    if (!win)
        return NULL;

    g_mutex_lock(&win->mutex);
    SdlAppWindowRenderCb render_cb = win->callbacks.render;
    if (render_data)
        *render_data = win->callbacks.render_data;
    g_mutex_unlock(&win->mutex);
    return render_cb;
}

/** @brief Nastaví destroy callback. Thread-safe. */
void sdlapp_window_set_destroy_cb(SdlAppWindow *win, SdlAppWindowDestroyCb destroy_cb, gpointer destroy_data)
{
    if (!win)
        return;

    g_mutex_lock(&win->mutex);
    win->callbacks.destroy = destroy_cb;
    win->callbacks.destroy_data = destroy_data;
    g_mutex_unlock(&win->mutex);
}

/** @brief Vrátí destroy callback a jeho data. Thread-safe. */
SdlAppWindowDestroyCb sdlapp_window_get_destroy_cb(SdlAppWindow *win, gpointer *destroy_data)
{
    if (!win)
        return NULL;

    g_mutex_lock(&win->mutex);
    SdlAppWindowDestroyCb destroy_cb = win->callbacks.destroy;
    if (destroy_data)
        *destroy_data = win->callbacks.destroy_data;
    g_mutex_unlock(&win->mutex);
    return destroy_cb;
}

/** @brief Nastaví close_requested callback. Thread-safe. */
void sdlapp_window_set_close_requested_cb(SdlAppWindow *win, SdlAppWindowCloseRequestedCb cb, gpointer user_data)
{
    if (!win)
        return;

    g_mutex_lock(&win->mutex);
    win->callbacks.close_requested = cb;
    win->callbacks.close_requested_data = user_data;
    g_mutex_unlock(&win->mutex);
}

/** @brief Vrátí close_requested callback a jeho data. Thread-safe. */
SdlAppWindowCloseRequestedCb sdlapp_window_get_close_requested_cb(SdlAppWindow *win, gpointer *user_data)
{
    if (!win)
        return NULL;

    g_mutex_lock(&win->mutex);
    SdlAppWindowCloseRequestedCb cb = win->callbacks.close_requested;
    if (user_data)
        *user_data = win->callbacks.close_requested_data;
    g_mutex_unlock(&win->mutex);
    return cb;
}

/** @brief Nastaví drag & drop callback. Thread-safe. */
void sdlapp_window_set_drop_file_cb(SdlAppWindow *win, SdlAppWindowDropFileCb cb, gpointer user_data)
{
    if (!win)
        return;

    g_mutex_lock(&win->mutex);
    win->callbacks.drop_file = cb;
    win->callbacks.drop_file_data = user_data;
    g_mutex_unlock(&win->mutex);
}

/** @brief Vrátí drag & drop callback a jeho data. Thread-safe. */
SdlAppWindowDropFileCb sdlapp_window_get_drop_file_cb(SdlAppWindow *win, gpointer *user_data)
{
    if (!win)
        return NULL;

    g_mutex_lock(&win->mutex);
    SdlAppWindowDropFileCb cb = win->callbacks.drop_file;
    if (user_data)
        *user_data = win->callbacks.drop_file_data;
    g_mutex_unlock(&win->mutex);
    return cb;
}
