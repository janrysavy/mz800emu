/* SDL/ImGui final-pixel regression: large textured image and resize.
 * No OpenGL, emulated CPU or native VRAM involved. GPLv3. */
#include <SDL3/SDL.h>
#include "libs/imgui/imgui.h"
#include "libs/imgui/backends/imgui_impl_sdl3.h"
#include "libs/imgui/backends/imgui_impl_sdlrenderer3.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
    const char *driver=argc>1 ? argv[1] : "software";
    const char *output=argc>2 ? argv[2] : nullptr;
    if (!strcmp(driver,"software")) SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"dummy");
    if (!strcmp(driver,"warp")) {
        SDL_SetHintWithPriority(SDL_HINT_RENDER_DIRECT3D11_WARP,"1",SDL_HINT_OVERRIDE);
        driver="direct3d11";
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) return 2;
    SDL_Window *window=SDL_CreateWindow("Final ImGui pixel regression",640,480,
                                       SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer=SDL_CreateRenderer(window,driver);
    if (!window || !renderer) { std::fprintf(stderr,"Init: %s\n",SDL_GetError()); return 2; }
    std::printf("Runtime SDL=%d renderer=%s\n", SDL_GetVersion(), SDL_GetRendererName(renderer));
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename=nullptr;
    ImGui_ImplSDL3_InitForSDLRenderer(window,renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    // Larger than the guest crop: actual UI includes framebuffer borders.
    const int tw=856,th=600;
    std::vector<unsigned char> bytes(tw*th*4);
    for (int y=0;y<th;y++) for (int x=0;x<tw;x++) {
        auto *p=&bytes[(y*tw+x)*4];
        p[0]=x<tw/2 ? 255 : 0; p[1]=y<th/2 ? 255 : 0; p[2]=0;p[3]=255;
    }
    SDL_Texture *texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,tw,th);
    SDL_UpdateTexture(texture,nullptr,bytes.data(),tw*4);
    SDL_SetTextureScaleMode(texture,SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
    const int sizes[][2]={{640,480},{1280,800},{2128,1328},{3840,2160},{640,480}};
    int failures=0;
    for (int n=0;n<5;n++) {
        const int w=sizes[n][0],h=sizes[n][1];
        SDL_SetWindowSize(window,w,h);
        SDL_SyncWindow(window);
        SDL_Event event;
        while(SDL_PollEvent(&event)) ImGui_ImplSDL3_ProcessEvent(&event);
        ImGui_ImplSDLRenderer3_NewFrame(); ImGui_ImplSDL3_NewFrame();
        // Hidden windows may retain an old display size. Explicit dimensions
        // match the real renderer output after the SDL resize above.
        ImGui::GetIO().DisplaySize=ImVec2((float)w,(float)h);
        ImGui::GetIO().DisplayFramebufferScale=ImVec2(1,1);
        ImGui::NewFrame();
        ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)texture,ImVec2(0,0),ImVec2((float)w,(float)h));
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(w*.7f,h*.7f),ImVec2(w*.9f,h*.9f),IM_COL32(17,83,149,255));
        ImGui::GetForegroundDrawList()->AddText(ImVec2(20,20),IM_COL32_WHITE,"Debugger overlay survives resize");
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer,117,142,153,255);SDL_RenderClear(renderer);
        SDL_ClearError();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
        SDL_Surface *surface=SDL_RenderReadPixels(renderer,nullptr);
        std::printf("Size %dx%d actual=%s SDL=%s\n",w,h,SDL_GetRendererName(renderer),SDL_GetError());
        if (*SDL_GetError()) { std::printf("FAIL SDL render error\n"); failures++; }
        const int points[][5]={{w/4,h/4,255,255,0},{3*w/4,h/4,0,255,0},{w/4,3*h/4,255,0,0},{w*6/10,h*8/10,0,0,0},{w*8/10,h*8/10,17,83,149}};
        for (const auto &point:points) {
            Uint8 r=0,g=0,b=0,a=0;
            if (!surface || !SDL_ReadSurfacePixel(surface,point[0],point[1],&r,&g,&b,&a) ||
                r!=point[2] || g!=point[3] || b!=point[4] || a!=255) {
                std::printf("FAIL pixel %d,%d got=%u,%u,%u,%u expected=%d,%d,%d,255\n",
                    point[0],point[1],r,g,b,a,point[2],point[3],point[4]);failures++;
            }
        }
        if(surface && output) {
            char path[4096];std::snprintf(path,sizeof(path),"%s/frame-%d-%dx%d.bmp",output,n,w,h);SDL_SaveBMP(surface,path);
        }
        SDL_DestroySurface(surface);SDL_RenderPresent(renderer);
    }
    // Crop, per-vertex uniform tint/alpha, clip, and texture-state restoration.
    SDL_SetWindowSize(window,640,480);SDL_SyncWindow(window);
    ImGui_ImplSDLRenderer3_NewFrame();ImGui_ImplSDL3_NewFrame();
    ImGui::GetIO().DisplaySize=ImVec2(640,480);ImGui::GetIO().DisplayFramebufferScale=ImVec2(1,1);
    ImGui::NewFrame();
    SDL_SetTextureColorModFloat(texture,.25f,.5f,.75f);
    SDL_SetTextureAlphaModFloat(texture,.75f);
    auto *list=ImGui::GetBackgroundDrawList();
    list->PushClipRect(ImVec2(160,120),ImVec2(480,360));
    list->AddImage((ImTextureID)texture,ImVec2(0,0),ImVec2(640,480),
                   ImVec2(.5f,0),ImVec2(1,.5f),IM_COL32(64,192,128,128));
    list->PopClipRect();ImGui::Render();
    SDL_SetRenderDrawColor(renderer,117,142,153,255);SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
    SDL_Surface *surface=SDL_RenderReadPixels(renderer,nullptr);
    if (!surface) { std::fprintf(stderr,"Readback: %s\n",SDL_GetError()); return 2; }
    Uint8 r,g,b,a;
    SDL_ReadSurfacePixel(surface,320,240,&r,&g,&b,&a);
    if(r<57 || r>59 || g<166 || g>168 || b<75 || b>77 || a!=255) {
        std::printf("FAIL clipped/tinted/alpha crop got=%u,%u,%u,%u\n",r,g,b,a);failures++;
    }
    SDL_ReadSurfacePixel(surface,80,60,&r,&g,&b,&a);
    if(r!=117 || g!=142 || b!=153 || a!=255) {std::printf("FAIL outside clip\n");failures++;}
    float mr,mg,mb,ma;
    SDL_GetTextureColorModFloat(texture,&mr,&mg,&mb);SDL_GetTextureAlphaModFloat(texture,&ma);
    if(mr!=.25f || mg!=.5f || mb!=.75f || ma!=.75f) {std::printf("FAIL restored texture modulation\n");failures++;}
    SDL_SetTextureColorModFloat(texture,1,1,1);SDL_SetTextureAlphaModFloat(texture,1);
    SDL_DestroySurface(surface);SDL_RenderPresent(renderer);
    // Force the 16-bit draw list to use VtxOffset, before submitting the image.
    ImGui_ImplSDLRenderer3_NewFrame();ImGui_ImplSDL3_NewFrame();ImGui::NewFrame();
    list=ImGui::GetBackgroundDrawList();
    for(int i=0;i<16385;i++) list->AddRectFilled(ImVec2(-16,-16),ImVec2(-4,-4),IM_COL32_WHITE);
    list->AddImage((ImTextureID)texture,ImVec2(0,0),ImVec2(640,480));
    bool offset=false;
    for(const ImDrawCmd &cmd:list->CmdBuffer) if(cmd.GetTexID()==(ImTextureID)texture && cmd.VtxOffset>0) offset=true;
    if(sizeof(ImDrawIdx)==2 && !offset) {std::printf("FAIL VtxOffset not exercised\n");failures++;}
    ImGui::Render();SDL_SetRenderDrawColor(renderer,117,142,153,255);SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
    surface=SDL_RenderReadPixels(renderer,nullptr);
    if (!surface) { std::fprintf(stderr,"Readback: %s\n",SDL_GetError()); return 2; }
    SDL_ReadSurfacePixel(surface,160,120,&r,&g,&b,&a);
    if(r!=255 || g!=255 || b!=0 || a!=255) {std::printf("FAIL image with VtxOffset\n");failures++;}
    SDL_DestroySurface(surface);SDL_RenderPresent(renderer);
    std::printf("Crop/tint/alpha/clip/state and %zu-bit-index VtxOffset checks complete\n",sizeof(ImDrawIdx)*8);
    SDL_DestroyTexture(texture);
    ImGui_ImplSDLRenderer3_Shutdown();ImGui_ImplSDL3_Shutdown();ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    std::printf("Final geometry pixel failures: %d\n",failures);
    return failures ? 1 : 0;
}
