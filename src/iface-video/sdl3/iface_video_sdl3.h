#ifndef IFACE_VIDEO_SDL3_H
#define IFACE_VIDEO_SDL3_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <glib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif
    SDL_Surface *video_sdl3_create_surface(void);
    void video_sdl3_set_surface_colormap(uint32_t *colormap, SDL_Surface *surface);
    gboolean video_sdl3_update_surface_from_framebuffer(SDL_Surface *surface);
    void video_sdl3_render_surface_as_background(SDL_Surface *surface, SDL_Renderer *renderer, float aspect_ratio, gboolean have_new_screen);
    GLuint SDL_SurfaceToTexture(SDL_Surface *surface);
    /* ImGui IDs hold either SDL_Texture* or an OpenGL name without truncation. */
    typedef uintptr_t VideoTexture;
    void video_sdl3_texture_backend(SDL_Renderer *renderer, bool opengl);
    VideoTexture video_sdl3_texture_create(SDL_Surface *surface);
    bool video_sdl3_texture_update(VideoTexture texture, SDL_Surface *surface);
    void video_sdl3_texture_destroy(VideoTexture texture);
    SDL_Surface *ConvertIndexedSurfaceToRGBA(SDL_Surface *surface);
    void video_sdl3_set_render_vsync(bool enabled);
    void video_sdl3_set_GL_swap_interval(int value);
#ifdef __cplusplus
}
#endif

#endif /* IFACE_VIDEO_SDL3_H */
