#include "infer/ThumbnailTask.hpp"
#include <algorithm>
#include <cmath>
Thumbnail ThumbnailTask::create(const Frame& f) const {
    Thumbnail out; out.width=cfg_.width; out.height=cfg_.height;
    if (!cfg_.enabled || f.width<=0 || f.height<=0 || cfg_.width<=0 || cfg_.height<=0) return out;
    int w=cfg_.width&~1, h=cfg_.height&~1; out.width=w; out.height=h;
    out.data.resize((size_t)w*h*3/2);
    auto sample_y=[&](int x,int y)->uint8_t {
        x=std::max(0,std::min(f.width-1,x)); y=std::max(0,std::min(f.height-1,y));
        if(f.pixel_format==PixelFormat::NV12) return f.raw_data[(size_t)y*f.width+x];
        int p=(y*f.width+(x&~1))*2; return f.raw_data[p+(x&1?2:0)];
    };
    auto sample_uv=[&](int x,int y,bool v)->uint8_t {
        x=std::max(0,std::min(f.width-2,x&~1)); y=std::max(0,std::min(f.height-2,y&~1));
        if(f.pixel_format==PixelFormat::NV12) return f.raw_data[(size_t)f.width*f.height+(size_t)(y/2)*f.width+x+(v?1:0)];
        int p=(y*f.width+x)*2; return f.raw_data[p+(v?3:1)];
    };
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        int sx=x*f.width/w, sy=y*f.height/h; out.data[(size_t)y*w+x]=sample_y(sx,sy);
    }
    uint8_t* uv=out.data.data()+(size_t)w*h;
    for(int y=0;y<h/2;++y) for(int x=0;x<w;x+=2) {
        int sx=(x*f.width/w)&~1, sy=(y*f.height/h*2)&~1;
        uv[(size_t)y*w+x]=sample_uv(sx,sy,false);
        uv[(size_t)y*w+x+1]=sample_uv(sx,sy,true);
    }
    return out;
}
