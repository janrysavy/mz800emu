/* Hidden native floating-window pixel/lifecycle regression; never OpenGL. */
#include <SDL3/SDL.h>
#include "libs/imgui/imgui.h"
#include "libs/imgui/backends/imgui_impl_sdl3.h"
#include "libs/imgui/backends/imgui_impl_sdlrenderer3.h"
#include <cstdio>
#include <cstring>

int main(int argc,char** argv)
{
    const char* driver=argc>1?argv[1]:"software";
    const char* output=argc>2?argv[2]:nullptr;
    if(!strcmp(driver,"warp")) { SDL_SetHintWithPriority(SDL_HINT_RENDER_DIRECT3D11_WARP,"1",SDL_HINT_OVERRIDE);driver="direct3d11"; }
    if(!SDL_Init(SDL_INIT_VIDEO)) return 2;
    SDL_Window* window=SDL_CreateWindow("Hidden parent",640,480,SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer=SDL_CreateRenderer(window,driver);
    if(!window||!renderer) { std::printf("Init failed: %s\n",SDL_GetError());return 2; }
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
    io.ConfigFlags|=ImGuiConfigFlags_ViewportsEnable;io.ConfigViewportsNoAutoMerge=true;
    ImGui_ImplSDL3_InitForSDLRenderer(window,renderer);ImGui_ImplSDLRenderer3_Init(renderer);
    // Platform-created windows remain hidden throughout this regression.
    ImGui::GetPlatformIO().Platform_ShowWindow=[](ImGuiViewport*){};
    if(!(io.BackendFlags&ImGuiBackendFlags_RendererHasViewports)) { std::printf("No native viewport support on %s\n",SDL_GetCurrentVideoDriver());return 2; }
    SDL_Texture* image=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,4,4);
    SDL_SetTextureBlendMode(image,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(image,SDL_SCALEMODE_NEAREST);
    int failures=0;bool open[2]={true,true};ImGuiID ids[2]={};
    unsigned char rgba[4*4*4];
    for(int frame=0;frame<7;frame++)
    {
        const bool green=frame>=3;
        for(int i=0;i<16;i++) { rgba[i*4]=green?0:255;rgba[i*4+1]=green?255:0;rgba[i*4+2]=0;rgba[i*4+3]=255; }
        SDL_UpdateTexture(image,nullptr,rgba,16);
        SDL_SetTextureColorModFloat(image,.25f,.5f,.75f);SDL_SetTextureAlphaModFloat(image,.5f);
        SDL_Event event;while(SDL_PollEvent(&event))ImGui_ImplSDL3_ProcessEvent(&event);
        ImGui_ImplSDLRenderer3_NewFrame();ImGui_ImplSDL3_NewFrame();
        io.DisplaySize=ImVec2(640,480);ImGui::NewFrame();
        ImVec2 origin=ImGui::GetMainViewport()->Pos;
        for(int i=0;i<2;i++) if(open[i])
        {
            ImVec2 size=frame<3?ImVec2(256,180):ImVec2(1280,800);
            ImGui::SetNextWindowPos(ImVec2(origin.x+70+i*100+frame*8,origin.y+50+i*40));
            ImGui::SetNextWindowSize(size);
            if(ImGui::Begin(i?"Breakpoints":"Registers",&open[i]))
            {
                auto* viewport=ImGui::GetWindowViewport();ids[i]=viewport->ID;
                ImGui::Text("A=42 BC=1234 real native window");
                ImVec2 pos=ImGui::GetWindowPos();
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)image,
                    ImVec2(pos.x+20,pos.y+70),ImVec2(pos.x+size.x-20,pos.y+size.y-20));
            }
            ImGui::End();
        }
        ImGui::Render();SDL_SetRenderDrawColor(renderer,17,83,149,255);SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
        ImGui::UpdatePlatformWindows();
        int detached=0;
        for(auto* viewport:ImGui::GetPlatformIO().Viewports)
        {
            if(viewport==ImGui::GetMainViewport()||!viewport->RendererUserData)continue;
            detached++;
            SDL_Window* child=SDL_GetWindowFromID((SDL_WindowID)(intptr_t)viewport->PlatformHandle);
            SDL_Renderer* target=SDL_GetRenderer(child);
            if(!child||child==window||!target||target==renderer||!(SDL_GetWindowFlags(child)&SDL_WINDOW_HIDDEN)) { std::printf("FAIL native child ownership/hidden state\n");failures++;continue; }
            if(std::strcmp(SDL_GetRendererName(target),driver)) {std::printf("FAIL selected child driver\n");failures++;}
            SDL_ClearError();
            ImGui::GetPlatformIO().Renderer_RenderWindow(viewport,nullptr);
            SDL_Surface* surface=SDL_RenderReadPixels(target,nullptr);
            Uint8 r=0,g=0,b=0,a=0;
            if(!surface||!SDL_ReadSurfacePixel(surface,30,90,&r,&g,&b,&a)||r!=(green?0:255)||g!=(green?255:0)||b||a!=255)
            {std::printf("FAIL child image frame%d viewport%u pixel=%u,%u,%u,%u error=%s\n",frame,viewport->ID,r,g,b,a,SDL_GetError());failures++;}
            // Font atlas was cloned as well: above the image there must be
            // several bright text pixels, rather than a blank solid surface.
            int bright=0;
            if(surface) for(int y=25;y<60;y++)for(int x=10;x<220;x++) { SDL_ReadSurfacePixel(surface,x,y,&r,&g,&b,&a);if(r>150&&g>150&&b>150)bright++; }
            if(bright<10) {std::printf("FAIL child font pixels frame%d\n",frame);failures++;}
            if(surface&&output) {char path[4096];std::snprintf(path,sizeof(path),"%s/frame-%d-%u.bmp",output,frame,viewport->ID);SDL_SaveBMP(surface,path);}
            SDL_DestroySurface(surface);
            ImGui::GetPlatformIO().Renderer_SwapBuffers(viewport,nullptr);
        }
        if(frame<5&&detached!=2) {std::printf("FAIL expected two native windows frame%d count%d\n",frame,detached);failures++;}
        SDL_Surface* parent=SDL_RenderReadPixels(renderer,nullptr);Uint8 r=0,g=0,b=0,a=0;
        if(!parent||!SDL_ReadSurfacePixel(parent,320,240,&r,&g,&b,&a)||r!=17||g!=83||b!=149) {std::printf("FAIL child draw changed parent\n");failures++;}
        SDL_DestroySurface(parent);
        float mr,mg,mb,ma;SDL_GetTextureColorModFloat(image,&mr,&mg,&mb);SDL_GetTextureAlphaModFloat(image,&ma);
        if(mr!=.25f||mg!=.5f||mb!=.75f||ma!=.5f) {std::printf("FAIL source texture state\n");failures++;}
        SDL_RenderPresent(renderer);
        if(frame==4)
        {
            auto* viewport=ImGui::FindViewportByID(ids[0]);
            SDL_Event close={};close.type=SDL_EVENT_WINDOW_CLOSE_REQUESTED;close.window.windowID=(SDL_WindowID)(intptr_t)viewport->PlatformHandle;
            ImGui_ImplSDL3_ProcessEvent(&close);
        }
        std::printf("Frame%d detached=%d red/green=%d\n",frame,detached,green);
    }
    if(open[0]) {std::printf("FAIL child close request not propagated\n");failures++;}
    ImGui_ImplSDLRenderer3_Shutdown();ImGui_ImplSDL3_Shutdown();ImGui::DestroyContext();
    int count=0;SDL_Window** windows=SDL_GetWindows(&count);SDL_free(windows);
    if(count!=1) {std::printf("FAIL secondary windows leaked count%d\n",count);failures++;}
    SDL_DestroyTexture(image);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    std::printf("Native viewport failures=%d renderer=%s\n",failures,driver);
    return failures?1:0;
}
