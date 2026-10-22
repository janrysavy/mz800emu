/* Physical main-window input must not activate a stale debugger navigation ID.
 * Pure ImGui + hidden SDL software fixture; no guest mutation or OS input. */
#include <SDL3/SDL.h>
#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "ui-imgui/emulator_input_focus.h"
#include <cstdio>
#include <initializer_list>

int main()
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"dummy");
    if(!SDL_Init(SDL_INIT_VIDEO))return 2;
    SDL_Window* main=SDL_CreateWindow("Hidden guest",640,480,SDL_WINDOW_HIDDEN);
    SDL_Window* debugger=SDL_CreateWindow("Hidden debugger",640,480,SDL_WINDOW_HIDDEN);
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
    io.DisplaySize=ImVec2(1280,800);io.DeltaTime=1.f/60;
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    int pauses=0, failures=0;ImGuiID pause_id=0;
    auto frame=[&]() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(640,480));
        ImGui::Begin("##MainEmulatorWindow",nullptr,emulator_view_navigation_flags);
        ImGui::Dummy(ImVec2(600,400));ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(650,0));ImGui::SetNextWindowSize(ImVec2(400,400));
        ImGui::Begin("Debugger");pause_id=ImGui::GetID("Pause");
        if(ImGui::Button("Pause"))pauses++;
        ImGui::End();ImGui::Render();
    };
    frame();frame();
    auto* guest=ImGui::FindWindowByName("##MainEmulatorWindow");
    auto* debug=ImGui::FindWindowByName("Debugger");
    // A detached debugger can remain ImGui's NavWindow after native focus
    // returns to the main SDL window. Model that viewport distinction only.
    bool detached=true;
    auto stale=[&]() {
        ImGui::FocusWindow(debug);
        ImGui::SetNavID(pause_id,ImGuiNavLayer_Main,debug->NavRootFocusScopeId,ImRect());
        ImGui::GetCurrentContext()->NavCursorVisible=true;
    };
    auto input=[&](SDL_Window* source,ImGuiKey key,bool down,bool overlay=false) {
        SDL_Event event={};event.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;
        event.key.windowID=SDL_GetWindowID(source);
        debug->ViewportId=guest->ViewportId+(detached?1:0);
        imgui_emulator_input_focus(main,&event,overlay);
        io.AddKeyEvent(key,down);frame();
    };
    int guest_cases=0;
    for(ImGuiKey direction:{ImGuiKey_None,ImGuiKey_LeftArrow,ImGuiKey_RightArrow,
                           ImGuiKey_UpArrow,ImGuiKey_DownArrow}) {
        stale();int before=pauses;
        if(direction!=ImGuiKey_None)input(main,direction,true);
        input(main,ImGuiKey_Space,true);input(main,ImGuiKey_Space,false);
        if(direction!=ImGuiKey_None)input(main,direction,false);
        if(pauses!=before||ImGui::GetCurrentContext()->NavWindow!=guest)failures++;
        guest_cases++;
    }
    // Genuine debugger input must retain normal Space-button activation.
    stale();int before=pauses;
    input(debugger,ImGuiKey_Space,true);input(debugger,ImGuiKey_Space,false);
    if(pauses!=before+1||ImGui::GetCurrentContext()->NavWindow!=debug)failures++;
    // Main-viewport dialogs and overlay input must not be stolen by the guest.
    stale();imgui_emulator_input_focus(main,nullptr,false);
    if(ImGui::GetCurrentContext()->NavWindow!=debug)failures++;
    input(main,ImGuiKey_Space,true,true);input(main,ImGuiKey_Space,false,true);
    if(ImGui::GetCurrentContext()->NavWindow!=debug)failures++;
    detached=false;
    stale();input(main,ImGuiKey_Space,true);input(main,ImGuiKey_Space,false);
    if(ImGui::GetCurrentContext()->NavWindow!=debug)failures++;
    // Native focus events also recover stale detached navigation, without
    // requiring a mouse click. Unrelated events must never change focus.
    detached=true;stale();debug->ViewportId=guest->ViewportId+1;
    SDL_Event focus={};focus.type=SDL_EVENT_WINDOW_FOCUS_GAINED;
    focus.window.windowID=SDL_GetWindowID(main);
    imgui_emulator_input_focus(main,&focus,false);
    if(ImGui::GetCurrentContext()->NavWindow!=guest)failures++;
    stale();SDL_Event unrelated={};unrelated.type=SDL_EVENT_MOUSE_MOTION;
    imgui_emulator_input_focus(main,&unrelated,false);
    if(ImGui::GetCurrentContext()->NavWindow!=debug)failures++;
    // A context menu owns keyboard navigation even over the guest viewport.
    ImGui::NewFrame();ImGui::Begin("Debugger");ImGui::OpenPopup("Menu");
    if(ImGui::BeginPopup("Menu")) {
        auto* popup=ImGui::GetCurrentContext()->NavWindow;
        imgui_emulator_input_focus(main,&focus,false);
        if(ImGui::GetCurrentContext()->NavWindow!=popup)failures++;
        ImGui::CloseCurrentPopup();ImGui::EndPopup();
    } else failures++;
    ImGui::End();ImGui::Render();
    std::printf("Guest navigation cases=%d failures=%d expected UI activations=%d\n",guest_cases,failures,pauses);
    ImGui::DestroyContext();SDL_DestroyWindow(debugger);SDL_DestroyWindow(main);SDL_Quit();
    return failures?1:0;
}
