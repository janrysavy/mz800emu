// dear imgui: Renderer Backend for SDL_Renderer for SDL3
// (Requires: SDL 3.1.8+)

// Note that SDL_Renderer is an _optional_ component of SDL3, which IMHO is now largely obsolete.
// For a multi-platform app consider using other technologies:
// - SDL3+SDL_GPU: SDL_GPU is SDL3 new graphics abstraction API.
// - SDL3+DirectX, SDL3+OpenGL, SDL3+Vulkan: combine SDL with dedicated renderers.
// If your application wants to render any non trivial amount of graphics other than UI,
// please be aware that SDL_Renderer currently offers a limited graphic API to the end-user
// and it might be difficult to step out of those boundaries.

// Implemented features:
//  [X] Renderer: User texture binding. Use 'SDL_Texture*' as texture identifier. Read the FAQ about ImTextureID/ImTextureRef!
//  [X] Renderer: Large meshes support (64k+ vertices) even with 16-bit indices (ImGuiBackendFlags_RendererHasVtxOffset).
//  [X] Renderer: Texture updates support for dynamic font atlas (ImGuiBackendFlags_RendererHasTextures).
//  [X] Renderer: Expose selected render state for draw callbacks to use. Access in '(ImGui_ImplXXXX_RenderState*)GetPlatformIO().Renderer_RenderState'.
//  [X] Renderer: Multi-viewport support (mz800emu local SDL extension).

// You can copy and use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// Prefer including the entire imgui/ repository into your project (either as a copy or as a submodule), and only build the backends you need.
// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

// CHANGELOG
//  2025-09-18: Call platform_io.ClearRendererHandlers() on shutdown.
//  2025-06-11: Added support for ImGuiBackendFlags_RendererHasTextures, for dynamic font atlas. Removed ImGui_ImplSDLRenderer3_CreateFontsTexture() and ImGui_ImplSDLRenderer3_DestroyFontsTexture().
//  2025-01-18: Use endian-dependent RGBA32 texture format, to match SDL_Color.
//  2024-10-09: Expose selected render state in ImGui_ImplSDLRenderer3_RenderState, which you can access in 'void* platform_io.Renderer_RenderState' during draw callbacks.
//  2024-07-01: Update for SDL3 api changes: SDL_RenderGeometryRaw() uint32 version was removed (SDL#9009).
//  2024-05-14: *BREAKING CHANGE* ImGui_ImplSDLRenderer3_RenderDrawData() requires SDL_Renderer* passed as parameter.
//  2024-02-12: Amend to query SDL_RenderViewportSet() and restore viewport accordingly.
//  2023-05-30: Initial version.

#include "imgui.h"
#ifndef IMGUI_DISABLE
#include "imgui_impl_sdlrenderer3.h"
#include <stdint.h>     // intptr_t

// Clang warnings with -Weverything
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"    // warning: implicit conversion changes signedness
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wfloat-equal"          // warning: comparing floating-point with '==' or '!=' is unsafe
#endif

// SDL
#include <SDL3/SDL.h>
#if !SDL_VERSION_ATLEAST(3,0,0)
#error This backend requires SDL 3.0.0+
#endif

// mz800emu local extension: SDL textures belong to one renderer. Detached
// viewports own renderers and replicas; CPU font data/readback is shared only
// for the current UI frame. No guest state or GL context is involved.
struct ImGuiSDLTexturePixels { SDL_Texture* Source; SDL_Surface* Pixels; };
struct ImGuiSDLTextureClone { SDL_Texture* Source; SDL_Texture* Clone; int Frame; };
struct ImGuiSDLViewportData
{
    SDL_Renderer* Renderer = nullptr;
    bool Owned = false;
    ImVector<ImGuiSDLTextureClone> Textures;
    ~ImGuiSDLViewportData() { for (auto& t : Textures) SDL_DestroyTexture(t.Clone); }
};

// SDL_Renderer data
struct ImGui_ImplSDLRenderer3_Data
{
    SDL_Renderer*           Renderer;       // Main viewport's renderer
    ImVector<SDL_FColor>    ColorBuffer;
    ImVector<SDL_Vertex>    SoftwareVertices; // Local SDL software-stride/viewport conversion
    ImVector<ImGuiSDLTexturePixels> FramePixels;

    ImGui_ImplSDLRenderer3_Data()   { memset((void*)this, 0, sizeof(*this)); }
};

// Backend data stored in io.BackendRendererUserData to allow support for multiple Dear ImGui contexts
// It is STRONGLY preferred that you use docking branch with multi-viewports (== single Dear ImGui context + multiple windows) instead of multiple Dear ImGui contexts.
static ImGui_ImplSDLRenderer3_Data* ImGui_ImplSDLRenderer3_GetBackendData()
{
    return ImGui::GetCurrentContext() ? (ImGui_ImplSDLRenderer3_Data*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

static void ImGuiSDL_InitViewports(SDL_Renderer* renderer);
static void ImGuiSDL_ClearFramePixels();
static SDL_Texture* ImGuiSDL_ResolveTexture(SDL_Renderer* renderer, SDL_Texture* source);

// Functions
bool ImGui_ImplSDLRenderer3_Init(SDL_Renderer* renderer)
{
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");
    IM_ASSERT(renderer != nullptr && "SDL_Renderer not initialized!");

    // Setup backend capabilities flags
    ImGui_ImplSDLRenderer3_Data* bd = IM_NEW(ImGui_ImplSDLRenderer3_Data)();
    io.BackendRendererUserData = (void*)bd;
    io.BackendRendererName = "imgui_impl_sdlrenderer3";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // We can honor ImGuiPlatformIO::Textures[] requests during render.

    bd->Renderer = renderer;
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        ImGuiSDL_InitViewports(renderer);

    return true;
}

void ImGui_ImplSDLRenderer3_Shutdown()
{
    ImGui_ImplSDLRenderer3_Data* bd = ImGui_ImplSDLRenderer3_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

    if (io.BackendFlags & ImGuiBackendFlags_RendererHasViewports)
        ImGui::DestroyPlatformWindows();
    ImGuiSDL_ClearFramePixels();
    ImGui_ImplSDLRenderer3_DestroyDeviceObjects();

    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports);
    platform_io.ClearRendererHandlers();
    IM_DELETE(bd);
}

static void ImGui_ImplSDLRenderer3_SetupRenderState(SDL_Renderer* renderer, ImVec2 scale)
{
    // Clear out any viewports and cliprect set by the user
    // FIXME: Technically speaking there are lots of other things we could backup/setup/restore during our render process.
    SDL_SetRenderViewport(renderer, nullptr);
    SDL_SetRenderClipRect(renderer, nullptr);
    SDL_SetRenderScale(renderer, scale.x, scale.y);
}

void ImGui_ImplSDLRenderer3_NewFrame()
{
    ImGui_ImplSDLRenderer3_Data* bd = ImGui_ImplSDLRenderer3_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplSDLRenderer3_Init()?");
    IM_UNUSED(bd);
    ImGuiSDL_ClearFramePixels();
}

// https://github.com/libsdl-org/SDL/issues/9009
static int SDL_RenderGeometryRaw8BitColor(SDL_Renderer* renderer, ImVector<SDL_FColor>& colors_out, ImVector<SDL_Vertex>& vertices_out, bool software, ImVec2 origin, SDL_Texture* texture, const float* xy, int xy_stride, const SDL_Color* color, int color_stride, const float* uv, int uv_stride, int num_vertices, const void* indices, int num_indices, int size_indices)
{
    // mz800emu local modification: SDL software quad detection reads UVs
    // with color_stride (SDL_render.c), rather than uv_stride. Interleaved
    // SDL_Vertex data gives all three fields the same valid stride, so SDL
    // recognizes the rectangle and uses its own texture-copy path. Avoids
    // large-image triangle area overflow without changing indices or meshes.
    if (software || origin.x != 0 || origin.y != 0)
    {
        vertices_out.resize(num_vertices);
        for (int i = 0; i < num_vertices; ++i)
        {
            const float* pos = (const float*)((const char*)xy + i * xy_stride);
            const float* tex = (const float*)((const char*)uv + i * uv_stride);
            const SDL_Color* col = (const SDL_Color*)((const char*)color + i * color_stride);
            SDL_Vertex& v = vertices_out[i];
            v.position = { pos[0] - origin.x, pos[1] - origin.y };
            v.tex_coord = { tex[0], tex[1] };
            v.color = { col->r / 255.0f, col->g / 255.0f, col->b / 255.0f, col->a / 255.0f };
        }
        const SDL_Vertex* v = vertices_out.Data;
        return SDL_RenderGeometryRaw(renderer, texture, &v->position.x, sizeof(*v),
            &v->color, sizeof(*v), &v->tex_coord.x, sizeof(*v), num_vertices,
            indices, num_indices, size_indices);
    }
    const Uint8* color2 = (const Uint8*)color;
    colors_out.resize(num_vertices);
    SDL_FColor* color3 = colors_out.Data;
    for (int i = 0; i < num_vertices; i++)
    {
        color3[i].r = color->r / 255.0f;
        color3[i].g = color->g / 255.0f;
        color3[i].b = color->b / 255.0f;
        color3[i].a = color->a / 255.0f;
        color2 += color_stride;
        color = (const SDL_Color*)color2;
    }
    return SDL_RenderGeometryRaw(renderer, texture, xy, xy_stride, color3, sizeof(*color3), uv, uv_stride, num_vertices, indices, num_indices, size_indices);
}

void ImGui_ImplSDLRenderer3_RenderDrawData(ImDrawData* draw_data, SDL_Renderer* renderer)
{
    ImGui_ImplSDLRenderer3_Data* bd = ImGui_ImplSDLRenderer3_GetBackendData();
    const bool software = SDL_strcmp(SDL_GetRendererName(renderer), "software") == 0;

    // If there's a scale factor set by the user, use that instead
    // If the user has specified a scale factor to SDL_Renderer already via SDL_RenderSetScale(), SDL will scale whatever we pass
    // to SDL_RenderGeometryRaw() by that scale factor. In that case we don't want to be also scaling it ourselves here.
    float rsx = 1.0f;
    float rsy = 1.0f;
    SDL_GetRenderScale(renderer, &rsx, &rsy);
    ImVec2 render_scale;
    render_scale.x = (rsx == 1.0f) ? draw_data->FramebufferScale.x : 1.0f;
    render_scale.y = (rsy == 1.0f) ? draw_data->FramebufferScale.y : 1.0f;

    // Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
    int fb_width = (int)(draw_data->DisplaySize.x * render_scale.x);
    int fb_height = (int)(draw_data->DisplaySize.y * render_scale.y);
    if (fb_width == 0 || fb_height == 0)
        return;

    // Catch up with texture updates. Most of the times, the list will have 1 element with an OK status, aka nothing to do.
    // (This almost always points to ImGui::GetPlatformIO().Textures[] but is part of ImDrawData to allow overriding or disabling texture updates).
    if (draw_data->Textures != nullptr)
        for (ImTextureData* tex : *draw_data->Textures)
            if (tex->Status != ImTextureStatus_OK)
                ImGui_ImplSDLRenderer3_UpdateTexture(tex);

    // Backup SDL_Renderer state that will be modified to restore it afterwards
    struct BackupSDLRendererState
    {
        SDL_Rect    Viewport;
        bool        ViewportEnabled;
        bool        ClipEnabled;
        SDL_Rect    ClipRect;
    };
    BackupSDLRendererState old = {};
    old.ViewportEnabled = SDL_RenderViewportSet(renderer);
    old.ClipEnabled = SDL_RenderClipEnabled(renderer);
    SDL_GetRenderViewport(renderer, &old.Viewport);
    SDL_GetRenderClipRect(renderer, &old.ClipRect);

    // Setup desired state
    ImGui_ImplSDLRenderer3_SetupRenderState(renderer, render_scale);

    // Setup render state structure (for callbacks and custom texture bindings)
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    ImGui_ImplSDLRenderer3_RenderState render_state;
    render_state.Renderer = renderer;
    platform_io.Renderer_RenderState = &render_state;

    // Will project scissor/clipping rectangles into framebuffer space
    ImVec2 clip_off = draw_data->DisplayPos;         // (0,0) unless using multi-viewports
    ImVec2 clip_scale = render_scale;

    // Render command lists
    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        const ImDrawVert* vtx_buffer = draw_list->VtxBuffer.Data;
        const ImDrawIdx* idx_buffer = draw_list->IdxBuffer.Data;

        for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback)
            {
                // User callback, registered via ImDrawList::AddCallback()
                // (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
                    ImGui_ImplSDLRenderer3_SetupRenderState(renderer, render_scale);
                else
                    pcmd->UserCallback(draw_list, pcmd);
            }
            else
            {
                // Project scissor/clipping rectangles into framebuffer space
                ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x, (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
                ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x, (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);
                if (clip_min.x < 0.0f) { clip_min.x = 0.0f; }
                if (clip_min.y < 0.0f) { clip_min.y = 0.0f; }
                if (clip_max.x > (float)fb_width) { clip_max.x = (float)fb_width; }
                if (clip_max.y > (float)fb_height) { clip_max.y = (float)fb_height; }
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                    continue;

                SDL_Rect r = { (int)(clip_min.x), (int)(clip_min.y), (int)(clip_max.x - clip_min.x), (int)(clip_max.y - clip_min.y) };
                SDL_SetRenderClipRect(renderer, &r);

                const float* xy = (const float*)(const void*)((const char*)(vtx_buffer + pcmd->VtxOffset) + offsetof(ImDrawVert, pos));
                const float* uv = (const float*)(const void*)((const char*)(vtx_buffer + pcmd->VtxOffset) + offsetof(ImDrawVert, uv));
                const SDL_Color* color = (const SDL_Color*)(const void*)((const char*)(vtx_buffer + pcmd->VtxOffset) + offsetof(ImDrawVert, col)); // SDL 2.0.19+

                // Bind texture, Draw
                SDL_Texture* tex = (SDL_Texture*)pcmd->GetTexID();
                if (tex && SDL_GetRendererFromTexture(tex) != renderer)
                {
                    tex = ImGuiSDL_ResolveTexture(renderer, tex);
                    if (!tex) continue; // Logged replica failure; never use a foreign texture
                }
                SDL_RenderGeometryRaw8BitColor(renderer, bd->ColorBuffer, bd->SoftwareVertices, software, draw_data->DisplayPos, tex,
                    xy, (int)sizeof(ImDrawVert),
                    color, (int)sizeof(ImDrawVert),
                    uv, (int)sizeof(ImDrawVert),
                    draw_list->VtxBuffer.Size - pcmd->VtxOffset,
                    idx_buffer + pcmd->IdxOffset, pcmd->ElemCount, sizeof(ImDrawIdx));
            }
        }
    }
    platform_io.Renderer_RenderState = nullptr;

    // Restore modified SDL_Renderer state
    SDL_SetRenderViewport(renderer, old.ViewportEnabled ? &old.Viewport : nullptr);
    SDL_SetRenderClipRect(renderer, old.ClipEnabled ? &old.ClipRect : nullptr);
    SDL_SetRenderScale(renderer, rsx, rsy);
}

// mz800emu local multi-viewport extension for the SDL_Renderer backend.
static const char* ImGuiSDL_ViewportProperty = "mz800emu.imgui.viewport";

static void ImGuiSDL_ClearFramePixels()
{
    auto* bd = ImGui_ImplSDLRenderer3_GetBackendData();
    for (auto& image : bd->FramePixels) SDL_DestroySurface(image.Pixels);
    bd->FramePixels.clear();
    for (auto* viewport : ImGui::GetPlatformIO().Viewports)
        if (auto* data = (ImGuiSDLViewportData*)viewport->RendererUserData)
            for (int i = data->Textures.Size - 1; i >= 0; --i)
                if (data->Textures[i].Frame < ImGui::GetFrameCount() - 1)
                {
                    SDL_DestroyTexture(data->Textures[i].Clone);
                    data->Textures.erase(data->Textures.Data + i);
                }
}

// Prefer retained dynamic-atlas pixels. Generic user textures are copied by
// readback on their owning renderer, with all modified texture/render state
// restored. One readback per source per UI frame, shared by all detached windows.
static SDL_Surface* ImGuiSDL_GetPixels(SDL_Texture* source)
{
    auto* bd = ImGui_ImplSDLRenderer3_GetBackendData();
    for (auto& image : bd->FramePixels)
        if (image.Source == source) return image.Pixels;
    SDL_Surface* pixels = nullptr;
    for (auto* atlas : ImGui::GetPlatformIO().Textures)
        if ((SDL_Texture*)atlas->GetTexID() == source && atlas->Format == ImTextureFormat_RGBA32)
        {
            pixels = SDL_CreateSurfaceFrom(atlas->Width, atlas->Height,
                SDL_PIXELFORMAT_RGBA32, atlas->GetPixels(), atlas->Width * 4);
            break;
        }
    if (!pixels)
    {
        auto* owner = SDL_GetRendererFromTexture(source);
        float width, height;
        if (!owner || !SDL_GetTextureSize(source, &width, &height)) return nullptr;
        auto* copy = SDL_CreateTexture(owner, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_TARGET, (int)width, (int)height);
        if (!copy) return nullptr;
        SDL_Texture* target = SDL_GetRenderTarget(owner);
        SDL_Rect viewport, clip;
        bool viewport_enabled = SDL_RenderViewportSet(owner);
        bool clip_enabled = SDL_RenderClipEnabled(owner);
        SDL_GetRenderViewport(owner, &viewport); SDL_GetRenderClipRect(owner, &clip);
        float scale_x, scale_y, r, g, b, a;
        SDL_GetRenderScale(owner, &scale_x, &scale_y);
        int logical_w, logical_h; SDL_RendererLogicalPresentation logical_mode;
        SDL_GetRenderLogicalPresentation(owner, &logical_w, &logical_h, &logical_mode);
        SDL_BlendMode blend;
        SDL_GetTextureBlendMode(source, &blend);
        SDL_GetTextureColorModFloat(source, &r, &g, &b); SDL_GetTextureAlphaModFloat(source, &a);
        if (SDL_SetRenderTarget(owner, copy))
        {
            SDL_SetRenderLogicalPresentation(owner, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
            SDL_SetRenderScale(owner, 1, 1); SDL_SetRenderViewport(owner, nullptr); SDL_SetRenderClipRect(owner, nullptr);
            SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE);
            SDL_SetTextureColorModFloat(source, 1, 1, 1); SDL_SetTextureAlphaModFloat(source, 1);
            if (SDL_RenderTexture(owner, source, nullptr, nullptr)) pixels = SDL_RenderReadPixels(owner, nullptr);
            SDL_SetTextureBlendMode(source, blend);
            SDL_SetTextureColorModFloat(source, r, g, b); SDL_SetTextureAlphaModFloat(source, a);
        }
        SDL_SetRenderTarget(owner, target);
        SDL_SetRenderLogicalPresentation(owner, logical_w, logical_h, logical_mode);
        SDL_SetRenderScale(owner, scale_x, scale_y);
        SDL_SetRenderViewport(owner, viewport_enabled ? &viewport : nullptr);
        SDL_SetRenderClipRect(owner, clip_enabled ? &clip : nullptr);
        SDL_DestroyTexture(copy);
        if (pixels && pixels->format != SDL_PIXELFORMAT_RGBA32)
        {
            SDL_Surface* converted = SDL_ConvertSurface(pixels, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(pixels); pixels = converted;
        }
    }
    if (pixels) bd->FramePixels.push_back({source, pixels});
    return pixels;
}

static SDL_Texture* ImGuiSDL_ResolveTexture(SDL_Renderer* renderer, SDL_Texture* source)
{
    auto* data = (ImGuiSDLViewportData*)SDL_GetPointerProperty(
        SDL_GetRendererProperties(renderer), ImGuiSDL_ViewportProperty, nullptr);
    if (!data) { SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Foreign ImGui texture without viewport renderer"); return nullptr; }
    const int frame = ImGui::GetFrameCount();
    ImGuiSDLTextureClone* item = nullptr;
    for (auto& candidate : data->Textures)
        if (candidate.Source == source) { item = &candidate; break; }
    if (item && item->Frame == frame) return item->Clone;
    SDL_Surface* pixels = ImGuiSDL_GetPixels(source);
    if (!pixels) { SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Viewport texture readback failed: %s", SDL_GetError()); return nullptr; }
    if (!item)
    {
        data->Textures.push_back({source, nullptr, -1});
        item = &data->Textures.back();
    }
    if (item->Clone && (item->Clone->w != pixels->w || item->Clone->h != pixels->h))
    { SDL_DestroyTexture(item->Clone); item->Clone = nullptr; }
    if (!item->Clone) item->Clone = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STATIC, pixels->w, pixels->h);
    if (!item->Clone || !SDL_UpdateTexture(item->Clone, nullptr, pixels->pixels, pixels->pitch))
    { SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Viewport texture upload failed: %s", SDL_GetError()); return nullptr; }
    SDL_BlendMode blend; SDL_ScaleMode filter;
    SDL_GetTextureBlendMode(source, &blend); SDL_GetTextureScaleMode(source, &filter);
    SDL_SetTextureBlendMode(item->Clone, blend); SDL_SetTextureScaleMode(item->Clone, filter);
    item->Frame = frame;
    return item->Clone;
}

static void ImGuiSDL_CreateWindow(ImGuiViewport* viewport)
{
    auto* data = IM_NEW(ImGuiSDLViewportData)();
    viewport->RendererUserData = data;
    SDL_Window* window = SDL_GetWindowFromID((SDL_WindowID)(intptr_t)viewport->PlatformHandle);
    auto* bd = ImGui_ImplSDLRenderer3_GetBackendData();
    data->Renderer = SDL_CreateRenderer(window, SDL_GetRendererName(bd->Renderer));
    data->Owned = true;
    if (!data->Renderer) { SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Viewport renderer failed: %s", SDL_GetError()); return; }
    SDL_SetRenderVSync(data->Renderer, 0);
    SDL_SetPointerProperty(SDL_GetRendererProperties(data->Renderer), ImGuiSDL_ViewportProperty, data);
    SDL_Log("ImGui floating renderer: viewport=%u actual=%s", viewport->ID, SDL_GetRendererName(data->Renderer));
}
static void ImGuiSDL_DestroyWindow(ImGuiViewport* viewport)
{
    if (auto* data = (ImGuiSDLViewportData*)viewport->RendererUserData)
    {
        SDL_Renderer* renderer = data->Renderer; bool owned = data->Owned;
        IM_DELETE(data); // Textures must be destroyed before their renderer
        if (owned && renderer) SDL_DestroyRenderer(renderer);
        viewport->RendererUserData = nullptr;
    }
}
static void ImGuiSDL_RenderWindow(ImGuiViewport* viewport, void*)
{
    auto* data = (ImGuiSDLViewportData*)viewport->RendererUserData;
    if (!data || !data->Renderer) return;
    if (!(viewport->Flags & ImGuiViewportFlags_NoRendererClear))
    { SDL_SetRenderDrawColor(data->Renderer, 0, 0, 0, 255); SDL_RenderClear(data->Renderer); }
    ImGui_ImplSDLRenderer3_RenderDrawData(viewport->DrawData, data->Renderer);
}
static void ImGuiSDL_SwapBuffers(ImGuiViewport* viewport, void*)
{
    auto* data = (ImGuiSDLViewportData*)viewport->RendererUserData;
    if (data && data->Renderer) SDL_RenderPresent(data->Renderer);
}
static void ImGuiSDL_InitViewports(SDL_Renderer* renderer)
{
    auto& io = ImGui::GetIO();
    if (!(io.BackendFlags & ImGuiBackendFlags_PlatformHasViewports)) return;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
    auto& platform = ImGui::GetPlatformIO();
    platform.Renderer_CreateWindow = ImGuiSDL_CreateWindow;
    platform.Renderer_DestroyWindow = ImGuiSDL_DestroyWindow;
    platform.Renderer_RenderWindow = ImGuiSDL_RenderWindow;
    platform.Renderer_SwapBuffers = ImGuiSDL_SwapBuffers;
    auto* main = IM_NEW(ImGuiSDLViewportData)(); main->Renderer = renderer;
    ImGui::GetMainViewport()->RendererUserData = main;
}

void ImGui_ImplSDLRenderer3_UpdateTexture(ImTextureData* tex)
{
    ImGui_ImplSDLRenderer3_Data* bd = ImGui_ImplSDLRenderer3_GetBackendData();

    if (tex->Status == ImTextureStatus_WantCreate)
    {
        // Create and upload new texture to graphics system
        //IMGUI_DEBUG_LOG("UpdateTexture #%03d: WantCreate %dx%d\n", tex->UniqueID, tex->Width, tex->Height);
        IM_ASSERT(tex->TexID == 0 && tex->BackendUserData == nullptr);
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);

        // Create texture
        // (Bilinear sampling is required by default. Set 'io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines' or 'style.AntiAliasedLinesUseTex = false' to allow point/nearest sampling)
        SDL_Texture* sdl_texture = SDL_CreateTexture(bd->Renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, tex->Width, tex->Height);
        IM_ASSERT(sdl_texture != nullptr && "Backend failed to create texture!");
        SDL_UpdateTexture(sdl_texture, nullptr, tex->GetPixels(), tex->GetPitch());
        SDL_SetTextureBlendMode(sdl_texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(sdl_texture, SDL_SCALEMODE_LINEAR);

        // Store identifiers
        tex->SetTexID((ImTextureID)(intptr_t)sdl_texture);
        tex->SetStatus(ImTextureStatus_OK);
    }
    else if (tex->Status == ImTextureStatus_WantUpdates)
    {
        // Update selected blocks. We only ever write to textures regions which have never been used before!
        // This backend choose to use tex->Updates[] but you can use tex->UpdateRect to upload a single region.
        SDL_Texture* sdl_texture = (SDL_Texture*)(intptr_t)tex->TexID;
        for (ImTextureRect& r : tex->Updates)
        {
            SDL_Rect sdl_r = { r.x, r.y, r.w, r.h };
            SDL_UpdateTexture(sdl_texture, &sdl_r, tex->GetPixelsAt(r.x, r.y), tex->GetPitch());
        }
        tex->SetStatus(ImTextureStatus_OK);
    }
    else if (tex->Status == ImTextureStatus_WantDestroy)
    {
        if (SDL_Texture* sdl_texture = (SDL_Texture*)(intptr_t)tex->TexID)
            SDL_DestroyTexture(sdl_texture);

        // Clear identifiers and mark as destroyed (in order to allow e.g. calling InvalidateDeviceObjects while running)
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

void ImGui_ImplSDLRenderer3_CreateDeviceObjects()
{
}

void ImGui_ImplSDLRenderer3_DestroyDeviceObjects()
{
    // Destroy all textures
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->RefCount == 1)
        {
            tex->SetStatus(ImTextureStatus_WantDestroy);
            ImGui_ImplSDLRenderer3_UpdateTexture(tex);
        }
}

//-----------------------------------------------------------------------------

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif // #ifndef IMGUI_DISABLE
