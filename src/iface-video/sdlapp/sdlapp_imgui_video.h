#ifndef SDLAPP_IMGUI_VIDEO_H
#define SDLAPP_IMGUI_VIDEO_H

#include <glib.h>
#include "libs/sdlapp/sdlapp.h"

#ifdef __cplusplus
extern "C"
{
#endif
    void iface_sdl3_video_window_resize_event_handler(SdlAppWindow *win, SDL_Event *event);

    /**
     * @brief Vrátí surface s obrazem, který okno emulátoru právě zobrazuje.
     *
     * INDEX8 surface VIDEO_DISPLAY_WIDTH x VIDEO_DISPLAY_HEIGHT (pozor na
     * pitch), do kterého UI vlákno kopíruje hotové snímky emulátoru. Slouží
     * screenshotu z GUI (přesně to, co uživatel vidí).
     *
     * @return Surface, nebo NULL (headless, okno ještě neinicializované).
     *         Vlastní ho video modul, volající ho nesmí uvolnit.
     * @pre Volat jen z UI vlákna (to jediné surface mění).
     */
    SDL_Surface *iface_sdl3_video_get_screen_surface(void);
#ifdef __cplusplus
}
#endif

#endif /* SDLAPP_IMGUI_VIDEO_H */
