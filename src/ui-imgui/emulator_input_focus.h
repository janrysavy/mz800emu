#ifndef MZ800EMU_EMULATOR_INPUT_FOCUS_H
#define MZ800EMU_EMULATOR_INPUT_FOCUS_H
#include <SDL3/SDL.h>
#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"

// Image-only guest surface; debugger and menu widgets own their own navigation.
static constexpr ImGuiWindowFlags emulator_view_navigation_flags = ImGuiWindowFlags_NoNavInputs;

/* Called on the UI thread before SDL events reach ImGui's next NavUpdate.
 * Native focus can return to the main SDL window while ImGui still remembers
 * a button in a detached debugger. Reconcile only that stale cross-viewport
 * focus. Same-viewport dialogs, popups, overlay and real debugger events keep
 * their normal keyboard ownership. Mouse clicks and explicit shortcuts still
 * work; no guest key, CPU state or device clock is modified here. */
static inline void imgui_emulator_input_focus(SDL_Window* main,const SDL_Event* event,bool overlay)
{
    if(!main||!event||overlay||!ImGui::GetCurrentContext())return;
    SDL_WindowID source=0;
    if(event->type==SDL_EVENT_KEY_DOWN||event->type==SDL_EVENT_KEY_UP)
        source=event->key.windowID;
    else if(event->type==SDL_EVENT_WINDOW_FOCUS_GAINED)
        source=event->window.windowID;
    else return;
    if(source!=SDL_GetWindowID(main)||ImGui::IsPopupOpen(nullptr,
        ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel))return;
    ImGuiWindow* view=ImGui::FindWindowByName("##MainEmulatorWindow");
    if(!view||!view->WasActive)return;
    ImGuiWindow* nav=ImGui::GetCurrentContext()->NavWindow;
    if(nav&&nav!=view&&nav->ViewportId==view->ViewportId)return;
    if(nav!=view)ImGui::FocusWindow(view);
}
#endif
