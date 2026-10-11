#include "debugger_workspace.h"
#include "bootstrap/myimgui.h"
#include "emulator/debugger/debugger.h"
#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

static ImGuiID s_viewport, s_root, s_default, s_cpu, s_map, s_breakpoints;
static bool s_active;

/* Keep one explicit list for visibility and native-host close behavior. */
static bool requested(bool close=false)
{
    bool* flags[]={
        &g_gui->showDebuggerWindow,&g_gui->showBreakpointsWindow,
        &g_gui->showMemoryMapWindow,&g_gui->showMemoryHeatmapWindow,
        &g_gui->showVarsWindow,&g_gui->showWatchWindow,&g_gui->showIoWindow,
        &g_gui->showSymbolsWindow,&g_gui->showBookmarksWindow,
        &g_gui->showMemoryBrowserWindow,&g_gui->showMemoryDiffWindow,
        &g_gui->showMembrowserPcgEditor,&g_gui->showMembrowserCharInserter,
        &g_gui->showCpuWindow,&g_gui->showStackWindow,
        &g_gui->showStackRegionsWindow,&g_gui->showStackHistoryWindow,
        &g_gui->showCallstackWindow,&g_gui->showProfilerWindow,
        &g_gui->showEventViewerWindow,&g_gui->showDisassemblerWindow,
        &g_gui->showCtcStateWindow,&g_gui->showPpiStateWindow,
        &g_gui->showPiozStateWindow,&g_gui->showPsgStateWindow,
        &g_gui->showGdgStateWindow,&g_gui->showPsgAudioScopeWindow,
        &g_gui->showFdcStateWindow,&g_gui->showQdiskStateWindow,
        &g_gui->showRamdiskStateWindow,&g_gui->showIde8StateWindow
    };
    bool any=false;
    for(bool* flag:flags) {any|=*flag;if(close)*flag=false;}
    for(int i=0;i<4;i++) {
        any|=g_gui->showMemoryBrowserWindowExtra[i]|g_gui->showDisasmExtraWindow[i];
        if(close)g_gui->showMemoryBrowserWindowExtra[i]=g_gui->showDisasmExtraWindow[i]=false;
    }
    return any;
}

void debugger_workspace_begin_frame()
{
    s_active=false;
    auto& io=ImGui::GetIO();
    // OpenGL retains its existing frontend; SDL owns this grouped layout.
    if(!(io.ConfigFlags&ImGuiConfigFlags_DockingEnable)||!requested())return;
    auto* main=ImGui::GetMainViewport();
    ImVec2 pos(main->Pos.x+main->Size.x+20,main->Pos.y),size(1280,800);
    if(ImGui::GetPlatformIO().Monitors.Size) {
        const auto& mon=ImGui::GetPlatformIO().Monitors[0];
        size.x=std::min(size.x,mon.WorkSize.x);size.y=std::min(size.y,mon.WorkSize.y);
        pos.x=std::max(mon.WorkPos.x,std::min(pos.x,mon.WorkPos.x+mon.WorkSize.x-size.x));
        pos.y=std::max(mon.WorkPos.y,std::min(pos.y,mon.WorkPos.y+mon.WorkSize.y-size.y));
    }
    ImGui::SetNextWindowPos(pos,ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size,ImGuiCond_FirstUseEver);
    ImGuiWindowClass host;
    host.ViewportFlagsOverrideSet=ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&host);
    bool open=true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
    ImGui::Begin("Debugger workspace##debugger_workspace",&open,
                 ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoCollapse);
    s_viewport=ImGui::GetWindowViewport()->ID;
    s_root=ImGui::GetID("debugger_dockspace");
    if(!ImGui::DockBuilderGetNode(s_root)) {
        ImGui::DockBuilderAddNode(s_root,ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(s_root,ImGui::GetContentRegionAvail());
        ImGuiID body=s_root;
        ImGui::DockBuilderSplitNode(body,ImGuiDir_Right,.19f,&s_map,&body);
        ImGui::DockBuilderSplitNode(body,ImGuiDir_Right,.35f,&s_cpu,&body);
        ImGui::DockBuilderSplitNode(body,ImGuiDir_Down,.45f,&s_breakpoints,&s_default);
        ImGui::DockBuilderFinish(s_root);
    }
    ImGui::DockSpace(s_root);
    ImGui::End();ImGui::PopStyleVar();
    if(!open) {
        // Match the primary debugger's own close path: deactivate tracing
        // through its existing emulator-thread recompute request.
        if(g_gui->showDebuggerWindow)debugger_hide_main_window_request();
        requested(true);return;
    }
    s_active=true;
}

const char* debugger_workspace_panel(const char* name)
{
    if(!s_active||!name)return name;
    ImGuiID dock=s_default;
    if(std::strstr(name,"###cpu_window"))dock=s_cpu;
    else if(std::strstr(name,"memmap"))dock=s_map;
    else if(std::strstr(name,"##bpt_main"))dock=s_breakpoints;
    auto* window=ImGui::FindWindowByName(name);
    if(window&&window->DockNode&&ImGui::DockNodeIsInHierarchyOf(window->DockNode,ImGui::DockBuilderGetNode(s_root)))
        dock=window->DockId; // Preserve rearrangement inside this workspace.
    ImGui::SetNextWindowViewport(s_viewport);
    ImGui::SetNextWindowDockID(dock,ImGuiCond_Always);
    return name;
}

void debugger_workspace_end_frame()
{
    if(s_active&&ImGui::GetFrameCount()==20) {
        int count=0;
        for(auto* window:ImGui::GetCurrentContext()->Windows)
            if(window->Active&&window->DockNode&&ImGui::DockNodeIsInHierarchyOf(window->DockNode,ImGui::DockBuilderGetNode(s_root)))count++;
        std::printf("Debugger workspace: viewport=%u docked_panels=%d\n",s_viewport,count);
    }
    s_active=false;
}
