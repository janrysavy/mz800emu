#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3_image/SDL_image.h>
#include <glib.h>
#include "images.h"
#include "iface-video/sdl3/iface_video_sdl3.h"
#include "libs/sdlapp/sdlapp.h"

extern SdlApp *g_sdlapp;

/* img_basedir je relativní k home_dir (binary_dir).
 * Skutečná absolutní cesta se sestaví v imgui_images_load_all(). */
static const char *img_basedir = "ui_resources/imgui/images";

typedef struct image_list_t
{
    char *name;
    char *filepath;
} image_list_t;

static image_list_t image_list[] = {
    {"mz800-photo", "mz800-photo.png"},
    {"mz700-photo", "mz700-photo.png"},
    {"mz1500-photo", "mz1500-photo.png"},
    /* Vlajky pro dialog volby jazyka */
    {"flag-globe", "flags/globe.png"},
    {"flag-gb", "flags/gb.png"},
    {"flag-cz", "flags/cz.png"},
    {"flag-sk", "flags/sk.png"},
    {"flag-jp", "flags/jp.png"},
    {"flag-de", "flags/de.png"},
    {"flag-it", "flags/it.png"},
    {"flag-es", "flags/es.png"},
    {"flag-nl", "flags/nl.png"},
    {"flag-fr", "flags/fr.png"},
    {"flag-pl", "flags/pl.png"},
    {"flag-ua", "flags/ua.png"},
    /* Ikony debugger iconbaru */
    {"dbg-continue", "debugger/Continue24.gif"},
    {"dbg-pause", "debugger/Pause24.gif"},
    {"dbg-step-over", "debugger/StepOver24.gif"},
    {"dbg-step-into", "debugger/StepInto24.gif"},
    {"dbg-run-to-cursor", "debugger/Run_To_Cursor.gif"},
    {"dbg-breakpoints", "debugger/Breakpoints24.png"},
    {"dbg-memory", "debugger/memory_browser24.png"},
    {"dbg-disasm", "debugger/dissassembler24.png"},
    {NULL, NULL}};

typedef struct ui_image_list_t
{
    GList *images;
    GHashTable *image_map;
} ui_image_list_t;

static ui_image_list_t *g_ui_images = NULL;

static bool imgui_images_init(void)
{
    if (g_ui_images != NULL)
    {
        // SDLAPP_ERROR("Images already initialized\n");
        return true;
    };

    g_ui_images = g_new0(ui_image_list_t, 1);
    if (!g_ui_images)
    {
        SDLAPP_ERROR("Failed to allocate memory for image list\n");
        return false;
    };

    g_ui_images->image_map = g_hash_table_new(g_str_hash, g_str_equal);
    if (!g_ui_images->image_map)
    {
        SDLAPP_ERROR("Failed to create image map\n");
        g_free(g_ui_images);
        g_ui_images = NULL;
        return false;
    };

    return true;
}

ui_glimage_t *imgui_images_append(const char *name, const char *fullpath)
{
    if (!imgui_images_init())
    {
        SDLAPP_ERROR("Failed to initialize image list\n");
        return NULL;
    };

    SDL_Surface *surface = IMG_Load(fullpath);
    if (!surface)
    {
        SDLAPP_ERROR("Failed to load image: %s - %s\n", fullpath, SDL_GetError());
        return NULL;
    };

    //SDL_Log("File %s, Surface format: %s", fullpath, SDL_GetPixelFormatName(surface->format));

    int width = surface->w;
    int height = surface->h;
    VideoTexture texture = video_sdl3_texture_create(surface);
    SDL_DestroySurface(surface);
    if (texture == 0)
    {
        g_print("Failed to create texture from surface\n");
        SDLAPP_ERROR("Failed to create texture for image: %s\n", name);
        return NULL;
    };

    ui_glimage_t *img = g_new0(ui_glimage_t, 1);
    if (!img)
    {
        SDLAPP_ERROR("Failed to allocate memory for image: %s\n", name);
        video_sdl3_texture_destroy(texture);
        return NULL;
    };

    img->name = g_strdup(name);
    img->texture = texture;
    img->width = width;
    img->height = height;

    //g_print ("Image %s loaded: %d x %d\n", name, width, height);

    g_ui_images->images = g_list_append(g_ui_images->images, img);
    g_hash_table_insert(g_ui_images->image_map, (gpointer)name, img);

    return img;
}

bool imgui_images_load_all(void)
{
    if (g_ui_images != NULL)
    {
        SDLAPP_ERROR("All images already loaded\n");
        return true;
    };

    if (!imgui_images_init())
    {
        SDLAPP_ERROR("Failed to initialize image list\n");
        return false;
    };

    /* Sestavíme absolutní cestu k img_basedir relativně k home_dir.
     * Načítané soubory pak resolvujeme proti této cestě. */
    char *images_root = sdlapp_paths_resolve_home(g_sdlapp->paths, img_basedir);

    for (int i = 0; image_list[i].name != NULL; ++i)
    {
        const char *name = image_list[i].name;
        const char *filepath = image_list[i].filepath;

        char *fullpath = g_build_filename(images_root, filepath, NULL);
        if (!fullpath)
        {
            SDLAPP_ERROR("Failed to build full path for image: %s\n", name);
            continue;
        };

        ui_glimage_t *img = imgui_images_append(name, fullpath);
        g_free(fullpath);
        if (!img)
        {
            SDLAPP_ERROR("Failed to append image: %s\n", name);
            continue;
        };
    };

    g_free(images_root);
    return true;
}

void imgui_images_destroy_all(void)
{
    if (!g_ui_images)
    {
        SDLAPP_ERROR("Images not loaded\n");
        return;
    };

    GList *images = g_ui_images->images;
    for (GList *iter = images; iter != NULL; iter = g_list_next(iter))
    {
        ui_glimage_t *img = (ui_glimage_t *)iter->data;
        if (img)
        {
            g_free(img->name);
            video_sdl3_texture_destroy(img->texture);
            g_free(img);
        };
    };

    g_list_free(images);
    g_hash_table_destroy(g_ui_images->image_map);
    g_free(g_ui_images);
    g_ui_images = NULL;
}

ui_glimage_t *imgui_images_get_image_by_name(const char *name)
{
    if (!g_ui_images)
    {
        SDLAPP_ERROR("Images not loaded\n");
        return NULL;
    };

    ui_glimage_t *img = (ui_glimage_t *)g_hash_table_lookup(g_ui_images->image_map, name);
    if (!img)
    {
        SDLAPP_ERROR("Image not found: %s\n", name);
    };

    return img;
}