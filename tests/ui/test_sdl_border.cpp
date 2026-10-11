/* Production presentation UVs: exact half borders and all active pixels. */
#include <SDL3/SDL.h>
#include "libs/imgui/imgui.h"
#include "libs/imgui/backends/imgui_impl_sdl3.h"
#include "libs/imgui/backends/imgui_impl_sdlrenderer3.h"
#include "ui-imgui/display_picture.h"
#include <cstdio>
#include <cstring>

int main(int argc,char** argv)
{
    const char* driver=argc>1?argv[1]:"software";
    if(!strcmp(driver,"warp")) {SDL_SetHintWithPriority(SDL_HINT_RENDER_DIRECT3D11_WARP,"1",SDL_HINT_OVERRIDE);driver="direct3d11";}
    if(!SDL_Init(SDL_INIT_VIDEO))return 2;
    int failures=0;
    if(VIDEO_UI_BORDER_PIXELS!=50)failures++;
    const float scales[]={1.0f,1.5f,2.0f};
    for(int pass=0;pass<3;pass++) {
        float scale=scales[pass];
        int width=(int)(640*scale)+100,height=(int)(400*scale)+100;
        auto* window=SDL_CreateWindow("Hidden half border",width,height,SDL_WINDOW_HIDDEN);
        auto* renderer=SDL_CreateRenderer(window,driver);if(!window||!renderer)return 2;
        auto* source=SDL_CreateSurface(VIDEO_DISPLAY_WIDTH,VIDEO_DISPLAY_HEIGHT,SDL_PIXELFORMAT_RGBA32);
        for(int y=0;y<source->h;y++)for(int x=0;x<source->w;x++) {
            bool canvas=x>=VIDEO_BORDER_LEFT_WIDTH&&x<VIDEO_BORDER_LEFT_WIDTH+VIDEO_CANVAS_WIDTH&&
                        y>=VIDEO_BORDER_TOP_HEIGHT&&y<VIDEO_BORDER_TOP_HEIGHT+VIDEO_CANVAS_HEIGHT;
            // Integer scales exercise every640-mode sample. The default1.5x case
            // exercises real320-mode doubled pairs (exact3x guest pixels),
            // avoiding unspecified nearest-sampling ties between distinct640 samples.
            SDL_WriteSurfacePixel(source,x,y,canvas?((x-VIDEO_BORDER_LEFT_WIDTH)/(pass==1?2:1))%251:255,
                                  canvas?(y-VIDEO_BORDER_TOP_HEIGHT)%251:0,canvas?85:0,255);
        }
        auto* texture=SDL_CreateTextureFromSurface(renderer,source);SDL_SetTextureScaleMode(texture,SDL_SCALEMODE_NEAREST);
        ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
        ImGui_ImplSDL3_InitForSDLRenderer(window,renderer);ImGui_ImplSDLRenderer3_Init(renderer);
        ImGui_ImplSDLRenderer3_NewFrame();ImGui_ImplSDL3_NewFrame();ImGui::NewFrame();
        auto* vp=ImGui::GetMainViewport();ImGui::SetNextWindowPos(vp->Pos);ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0);
        ImGui::Begin("picture",nullptr,ImGuiWindowFlags_NoDecoration);
        imgui_display_picture((ImTextureID)texture,vp->Pos,ImVec2(width,height));
        ImGui::End();ImGui::PopStyleVar(2);ImGui::Render();
        SDL_SetRenderDrawColor(renderer,0,0,0,255);SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
        auto* pixels=SDL_RenderReadPixels(renderer,nullptr);
        if(!pixels)failures++;
        else {
            for(int y=0;y<height;y++)for(int x=0;x<width;x++) {
                bool active=x>=50&&x<width-50&&y>=50&&y<height-50;
                int sx=active?(int)((x-50+0.5f)/scale)+VIDEO_BORDER_LEFT_WIDTH:0,sy=active?(int)((y-50+0.5f)/(2*scale))+VIDEO_BORDER_TOP_HEIGHT:0;
                Uint8 er,eg,eb,ea,r,g,b,a;SDL_ReadSurfacePixel(source,sx,sy,&er,&eg,&eb,&ea);
                SDL_ReadSurfacePixel(pixels,x,y,&r,&g,&b,&a);
                if(r!=er||g!=eg||b!=eb||a!=ea) {failures++;if(failures<5)printf("FAIL pixel %d,%d got %u,%u,%u expected %u,%u,%u\n",x,y,r,g,b,er,eg,eb);}
            }
            if(argc>2) {char path[4096];snprintf(path,sizeof(path),"%s/border-pass%d.bmp",argv[2],pass);SDL_SaveBMP(pixels,path);}
        }
        printf("Fixed 50px border scale%.1f size=%dx%d canvas=%dx%d failures=%d\n",scale,width,height,(int)(640*scale),(int)(400*scale),failures);
        SDL_DestroySurface(pixels);ImGui_ImplSDLRenderer3_Shutdown();ImGui_ImplSDL3_Shutdown();ImGui::DestroyContext();
        SDL_DestroyTexture(texture);SDL_DestroySurface(source);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);
    }
    SDL_Quit();printf("Fixed 50px border failures=%d backend=%s\n",failures,driver);return failures?1:0;
}
