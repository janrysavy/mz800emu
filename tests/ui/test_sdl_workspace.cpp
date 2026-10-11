/* One hidden native debugger host; independent game/host sizing and close. */
#include <SDL3/SDL.h>
#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "libs/imgui/backends/imgui_impl_sdl3.h"
#include "libs/imgui/backends/imgui_impl_sdlrenderer3.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/debugger_workspace.h"
#include <cstdio>
#include <cstring>

MyImGui* g_gui;

int main(int argc,char** argv)
{
    const char* driver=argc>1?argv[1]:"software";
    if(!strcmp(driver,"warp")) {SDL_SetHintWithPriority(SDL_HINT_RENDER_DIRECT3D11_WARP,"1",SDL_HINT_OVERRIDE);driver="direct3d11";}
    if(!SDL_Init(SDL_INIT_VIDEO))return 2;
    auto* window=SDL_CreateWindow("Hidden game",640,480,SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE);
    auto* renderer=SDL_CreateRenderer(window,driver);
    if(!window||!renderer)return 2;
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
    io.ConfigFlags|=ImGuiConfigFlags_ViewportsEnable|ImGuiConfigFlags_DockingEnable;
    ImGui_ImplSDL3_InitForSDLRenderer(window,renderer);ImGui_ImplSDLRenderer3_Init(renderer);
    ImGui::GetPlatformIO().Platform_ShowWindow=[](ImGuiViewport*){};
    MyImGui gui={};g_gui=&gui;
    bool* panels[]={&gui.showCpuWindow,&gui.showBreakpointsWindow,&gui.showMemoryMapWindow};
    const char* names[]={"CPU Registers###cpu_window","Breakpoints##bpt_main","Memory Map###memmap_window"};
    for(auto* panel:panels)*panel=true;
    int failures=0;
    for(int frame=0;frame<8;frame++) {
        SDL_Event event;while(SDL_PollEvent(&event))ImGui_ImplSDL3_ProcessEvent(&event);
        ImGui_ImplSDLRenderer3_NewFrame();ImGui_ImplSDL3_NewFrame();ImGui::NewFrame();
        debugger_workspace_begin_frame();
        for(int i=0;i<3;i++)if(*panels[i]) {
            if(ImGui::Begin(debugger_workspace_panel(names[i]),panels[i])) {
                ImGui::Text("Panel %d: A=42 PC=6767",i);
                ImGui::Button("Action");
            }
            ImGui::End();
        }
        debugger_workspace_end_frame();ImGui::Render();
        SDL_SetRenderDrawColor(renderer,17,83,149,255);SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
        ImGui::UpdatePlatformWindows();
        int children=0;ImGuiViewport* host=nullptr;
        for(auto* vp:ImGui::GetPlatformIO().Viewports) {
            if(vp==ImGui::GetMainViewport()||!vp->RendererUserData)continue;
            children++;host=vp;
            auto* child=SDL_GetWindowFromID((SDL_WindowID)(intptr_t)vp->PlatformHandle);
            auto* target=SDL_GetRenderer(child);
            if(!child||child==window||target==renderer||!(SDL_GetWindowFlags(child)&SDL_WINDOW_HIDDEN)||strcmp(SDL_GetRendererName(target),driver))failures++;
            ImGui::GetPlatformIO().Renderer_RenderWindow(vp,nullptr);
            auto* pixels=SDL_RenderReadPixels(target,nullptr);
            int bright=0;
            if(pixels)for(int y=0;y<pixels->h;y+=2)for(int x=0;x<pixels->w;x+=2) {
                Uint8 r,g,b,a;SDL_ReadSurfacePixel(pixels,x,y,&r,&g,&b,&a);if(r>150&&g>150&&b>150)bright++;
            }
            if(bright<25)failures++;
            if(pixels&&argc>2) {char p[4096];snprintf(p,sizeof(p),"%s/workspace-%d.bmp",argv[2],frame);SDL_SaveBMP(pixels,p);}
            SDL_DestroySurface(pixels);ImGui::GetPlatformIO().Renderer_SwapBuffers(vp,nullptr);
        }
        if(frame<6&&children!=1)failures++;
        if(frame==7&&children!=0)failures++;
        for(int i=0;i<3;i++)if(*panels[i]) {
            auto* panel=ImGui::FindWindowByName(names[i]);
            if(!host||!panel||!panel->DockNode||panel->ViewportId!=host->ID)failures++;
        }
        auto* parent=SDL_RenderReadPixels(renderer,nullptr);Uint8 r,g,b,a;
        if(!parent||!SDL_ReadSurfacePixel(parent,320,240,&r,&g,&b,&a)||r!=17||g!=83||b!=149)failures++;
        SDL_DestroySurface(parent);SDL_RenderPresent(renderer);
        // Resize the game independently; then resize/move the debugger host.
        if(frame==2)SDL_SetWindowSize(window,1280,800);
        if(frame==3&&host) {auto* child=SDL_GetWindowFromID((SDL_WindowID)(intptr_t)host->PlatformHandle);SDL_SetWindowSize(child,2128,1328);SDL_SetWindowPosition(child,100,100);}
        if(frame==4)*panels[0]=false;
        if(frame==5&&host) {SDL_Event close={};close.type=SDL_EVENT_WINDOW_CLOSE_REQUESTED;close.window.windowID=(SDL_WindowID)(intptr_t)host->PlatformHandle;ImGui_ImplSDL3_ProcessEvent(&close);}
        printf("Workspace frame%d native_hosts=%d failures=%d\n",frame,children,failures);
    }
    for(auto* panel:panels)if(*panel)failures++;
    ImGui_ImplSDLRenderer3_Shutdown();ImGui_ImplSDL3_Shutdown();ImGui::DestroyContext();
    int count=0;auto** windows=SDL_GetWindows(&count);SDL_free(windows);if(count!=1)failures++;
    SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    printf("Workspace failures=%d renderer=%s\n",failures,driver);return failures?1:0;
}
