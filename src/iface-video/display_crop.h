#ifndef MZ800EMU_DISPLAY_CROP_H
#define MZ800EMU_DISPLAY_CROP_H
#include "hw-generic/gdg/video.h"
/* Guest raster stays intact; border thickness belongs to host presentation. */
#if MZARCH == 800
#define VIDEO_UI_BORDER_PIXELS 50
#define VIDEO_UI_CANVAS_WIDTH VIDEO_CANVAS_WIDTH
#define VIDEO_UI_CANVAS_HEIGHT (VIDEO_CANVAS_HEIGHT * 2)
#else
#define VIDEO_UI_BORDER_PIXELS 0
#define VIDEO_UI_CANVAS_WIDTH VIDEO_DISPLAY_WIDTH
#define VIDEO_UI_CANVAS_HEIGHT (VIDEO_DISPLAY_HEIGHT * 2)
#endif
#define VIDEO_UI_DISPLAY_WIDTH (VIDEO_UI_CANVAS_WIDTH + VIDEO_UI_BORDER_PIXELS * 2)
#define VIDEO_UI_DISPLAY_HEIGHT (VIDEO_UI_CANVAS_HEIGHT + VIDEO_UI_BORDER_PIXELS * 2)
typedef struct VideoUiSegment { float x0,y0,x1,y1,u0,v0,u1,v1; } VideoUiSegment;
static inline int video_ui_segments(float width,float height,float scale_x,float scale_y,VideoUiSegment out[9])
{
#if MZARCH == 800
    float bx=VIDEO_UI_BORDER_PIXELS/scale_x,by=VIDEO_UI_BORDER_PIXELS/scale_y;
    if(width<=2*bx||height<=2*by)return 0;
    const float x[4]={0,bx,width-bx,width},y[4]={0,by,height-by,height};
    const float u[4]={0,(float)VIDEO_BORDER_LEFT_WIDTH/VIDEO_DISPLAY_WIDTH,
        (float)(VIDEO_BORDER_LEFT_WIDTH+VIDEO_CANVAS_WIDTH)/VIDEO_DISPLAY_WIDTH,1};
    const float v[4]={0,(float)VIDEO_BORDER_TOP_HEIGHT/VIDEO_DISPLAY_HEIGHT,
        (float)(VIDEO_BORDER_TOP_HEIGHT+VIDEO_CANVAS_HEIGHT)/VIDEO_DISPLAY_HEIGHT,1};
    int n=0;
    for(int row=0;row<3;row++)for(int col=0;col<3;col++) {
        VideoUiSegment s={x[col],y[row],x[col+1],y[row+1],u[col],v[row],u[col+1],v[row+1]};out[n++]=s;
    }
    return n;
#else
    VideoUiSegment s={0,0,width,height,0,0,1,1};out[0]=s;return 1;
#endif
}
#endif
