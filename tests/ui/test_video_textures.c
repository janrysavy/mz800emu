/* Production texture helper checks; dummy/software only, no GL context. */
#include "unity.h"
#include "iface-video/sdl3/iface_video_sdl3.h"
#include "iface/iface_video.h"
/* Unused device/UI entry points in the compiled production translation unit.
 * These tests exercise texture helpers only, never the framebuffer consumer. */
st_DISPLAY g_display;
iface_video_t *g_iface_video;
void display_set_framerate_mode(en_DISPLAY_FRAMERATE_MODE mode) {
    (void)mode; TEST_FAIL_MESSAGE("Unexpected device/frame-rate call in texture check");
}

static SDL_Window *window;
static SDL_Renderer *renderer;
void setUp(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    TEST_ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO));
    window = SDL_CreateWindow("texture checks", 16, 16, SDL_WINDOW_HIDDEN);
    TEST_ASSERT_NOT_NULL(window);
    renderer = SDL_CreateRenderer(window, "software");
    TEST_ASSERT_NOT_NULL(renderer);
    video_sdl3_texture_backend(renderer, false);
}
void tearDown(void) {
    video_sdl3_texture_backend(NULL, false);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
}
static void expect_pixel(VideoTexture texture, int x, int y, int r, int g, int b) {
    TEST_ASSERT_EQUAL(sizeof(void *), sizeof(VideoTexture));
    TEST_ASSERT_TRUE(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255));
    TEST_ASSERT_TRUE(SDL_RenderClear(renderer));
    SDL_FRect dest = {0, 0, 3, 2};
    TEST_ASSERT_TRUE(SDL_RenderTexture(renderer, (SDL_Texture *)texture, NULL, &dest));
    SDL_Surface *readback = SDL_RenderReadPixels(renderer, NULL);
    TEST_ASSERT_NOT_NULL(readback);
    Uint8 rr, gg, bb, aa;
    TEST_ASSERT_TRUE(SDL_ReadSurfacePixel(readback, x, y, &rr, &gg, &bb, &aa));
    SDL_DestroySurface(readback);
    TEST_ASSERT_INT_WITHIN(1, r, rr);
    TEST_ASSERT_INT_WITHIN(1, g, gg);
    TEST_ASSERT_INT_WITHIN(1, b, bb);
}
void test_indexed_pitch_opacity_update(void) {
    SDL_Surface *surface = SDL_CreateSurface(3, 2, SDL_PIXELFORMAT_INDEX8);
    TEST_ASSERT_NOT_NULL(surface);
    SDL_Palette *palette = SDL_CreatePalette(2);
    SDL_Color colors[] = {{255,0,0,0}, {0,0,255,0}};
    TEST_ASSERT_TRUE(SDL_SetPaletteColors(palette, colors, 0, 2));
    TEST_ASSERT_TRUE(SDL_SetSurfacePalette(surface, palette));
    SDL_DestroyPalette(palette);
    Uint8 *pixels = surface->pixels;
    memset(pixels, 0, surface->pitch * surface->h);
    memset(pixels + surface->pitch, 1, 3);
    VideoTexture texture = video_sdl3_texture_create(surface);
    TEST_ASSERT_NOT_EQUAL(0, texture);
    expect_pixel(texture, 2, 1, 0, 0, 255);
    memset(pixels + surface->pitch, 0, 3);
    TEST_ASSERT_TRUE(video_sdl3_texture_update(texture, surface));
    expect_pixel(texture, 2, 1, 255, 0, 0);
    video_sdl3_texture_destroy(texture); SDL_DestroySurface(surface);
    video_sdl3_texture_destroy(0);
}
void test_rgba_image_alpha(void) {
    SDL_Surface *surface = SDL_CreateSurface(3, 2, SDL_PIXELFORMAT_RGBA32);
    TEST_ASSERT_NOT_NULL(surface);
    SDL_FillSurfaceRect(surface, NULL,
        SDL_MapRGBA(SDL_GetPixelFormatDetails(surface->format), NULL, 0, 255, 0, 128));
    VideoTexture texture = video_sdl3_texture_create(surface);
    TEST_ASSERT_NOT_EQUAL(0, texture);
    expect_pixel(texture, 1, 1, 0, 128, 0);
    video_sdl3_texture_destroy(texture); SDL_DestroySurface(surface);
}
int main(void) {
    UNITY_BEGIN(); RUN_TEST(test_indexed_pitch_opacity_update);
    RUN_TEST(test_rgba_image_alpha); return UNITY_END();
}
