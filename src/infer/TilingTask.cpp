#include "infer/TilingTask.hpp"
#include <algorithm>
#include <cmath>
namespace {
float iou(const Detection&a,const Detection&b) {
    float x1=std::max(a.x1,b.x1),y1=std::max(a.y1,b.y1);
    float x2=std::min(a.x2,b.x2),y2=std::min(a.y2,b.y2);
    float inter=std::max(0.0f,x2-x1)*std::max(0.0f,y2-y1);
    float aa=std::max(0.0f,a.x2-a.x1)*std::max(0.0f,a.y2-a.y1);
    float ab=std::max(0.0f,b.x2-b.x1)*std::max(0.0f,b.y2-b.y1);
    return inter/(aa+ab-inter+1e-6f);
}
}
std::vector<TileRect> TilingTask::make_tiles(int fw,int fh,const RoiRect& roi) const {
    if (!cfg_.enabled) return {};
    int cols=std::max(1,std::min(2,cfg_.grid_cols)), rows=std::max(1,std::min(2,cfg_.grid_rows));
    int x0=std::max(0,(int)std::floor(roi.x1)), y0=std::max(0,(int)std::floor(roi.y1));
    int x2=std::min(fw,(int)std::ceil(roi.x2)), y2=std::min(fh,(int)std::ceil(roi.y2));
    int rw=std::max(2,x2-x0), rh=std::max(2,y2-y0);
    float ov=std::max(0.0f,std::min(0.4f,cfg_.overlap_ratio));
    std::vector<TileRect> out;
    for(int r=0;r<rows;++r) for(int c=0;c<cols;++c) {
        int tw=std::min(rw,(int)std::ceil(rw/(float)cols*(1+ov)));
        int th=std::min(rh,(int)std::ceil(rh/(float)rows*(1+ov)));
        int sx=x0+(rw*c)/cols, sy=y0+(rh*r)/rows;
        sx=std::max(x0,std::min(sx,x2-tw)); sy=std::max(y0,std::min(sy,y2-th));
        out.push_back({sx&~1,sy&~1,tw&~1,th&~1});
    }
    return out;
}
Frame TilingTask::crop(const Frame& f,const TileRect& t) const {
    Frame out; out.width=t.width; out.height=t.height; out.pixel_format=f.pixel_format;
    out.timestamp_ms=f.timestamp_ms; out.raw_data.resize((size_t)t.width*t.height*(f.pixel_format==PixelFormat::NV12?3:4)/2);
    if(f.pixel_format==PixelFormat::NV12) {
        for(int y=0;y<t.height;++y) std::copy_n(f.raw_data.data()+(size_t)(t.y+y)*f.width+t.x,t.width,out.raw_data.data()+(size_t)y*t.width);
        const uint8_t* suv=f.raw_data.data()+(size_t)f.width*f.height;
        uint8_t* duv=out.raw_data.data()+(size_t)t.width*t.height;
        for(int y=0;y<t.height/2;++y) std::copy_n(suv+(size_t)(t.y/2+y)*f.width+t.x,t.width,duv+(size_t)y*t.width);
    } else {
        for(int y=0;y<t.height;++y) std::copy_n(f.raw_data.data()+(size_t)(t.y+y)*f.width*2+t.x*2,t.width*2,out.raw_data.data()+(size_t)y*t.width*2);
    }
    return out;
}
std::vector<Detection> TilingTask::merge(std::vector<Detection> detections) const {
    std::sort(detections.begin(),detections.end(),[](const auto&a,const auto&b){return a.score>b.score;});
    std::vector<Detection> out;
    for(const auto& d:detections) {
        bool dup=false; for(const auto& k:out) if(k.class_id==d.class_id&&iou(k,d)>cfg_.merge_iou_threshold){dup=true;break;}
        if(!dup) out.push_back(d);
    }
    return out;
}
