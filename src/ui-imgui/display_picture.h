#ifndef MZ800EMU_DISPLAY_PICTURE_H
#define MZ800EMU_DISPLAY_PICTURE_H
#include "libs/imgui/imgui.h"
#include "iface-video/display_crop.h"
static inline void imgui_display_picture(ImTextureID texture,ImVec2 position,ImVec2 size)
{
    VideoUiSegment segments[9];const ImVec2 scale=ImGui::GetIO().DisplayFramebufferScale;
    int count=video_ui_segments(size.x,size.y,scale.x,scale.y,segments);
    for(int i=0;i<count;i++) {
        const VideoUiSegment& s=segments[i];
        ImGui::GetWindowDrawList()->AddImage(texture,ImVec2(position.x+s.x0,position.y+s.y0),
            ImVec2(position.x+s.x1,position.y+s.y1),ImVec2(s.u0,s.v0),ImVec2(s.u1,s.v1));
    }
    ImGui::Dummy(size);
}
#endif
