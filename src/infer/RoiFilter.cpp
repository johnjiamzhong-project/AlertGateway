#include "infer/RoiFilter.hpp"
#include <algorithm>
#include <sstream>
namespace {
float clamp01(float v){return std::max(0.0f,std::min(1.0f,v));}
float iou(const Detection&a,const Detection&b){
 float x1=std::max(a.x1,b.x1),y1=std::max(a.y1,b.y1),x2=std::min(a.x2,b.x2),y2=std::min(a.y2,b.y2);
 float inter=std::max(0.0f,x2-x1)*std::max(0.0f,y2-y1),aa=std::max(0.0f,a.x2-a.x1)*std::max(0.0f,a.y2-a.y1),ab=std::max(0.0f,b.x2-b.x1)*std::max(0.0f,b.y2-b.y1);
 return inter/(aa+ab-inter+1e-6f);
}
}
RoiFilter::RoiFilter(const RoiConfig& cfg):cfg_(cfg){}
std::vector<Detection> RoiFilter::filter(const std::vector<Detection>& ds,int width,int height,int64_t ts,std::vector<RoiEvent>* events){
 pixel_regions_.clear(); region_ids_.clear();
 for(const auto&r:cfg_.regions){float x=clamp01(r.x),y=clamp01(r.y),x2=clamp01(r.x+r.w),y2=clamp01(r.y+r.h);if(x2>x&&y2>y){pixel_regions_.push_back({x*width,y*height,x2*width,y2*height});region_ids_.push_back(r.id);}}
 if(!cfg_.enabled||pixel_regions_.empty())return ds;
 std::vector<Detection> out;
 for(const auto&d:ds){float cx=(d.x1+d.x2)*.5f,cy=(d.y1+d.y2)*.5f;int region=-1;
  for(size_t i=0;i<pixel_regions_.size();++i){const auto&r=pixel_regions_[i];if(cx>=r.x1&&cx<=r.x2&&cy>=r.y1&&cy<=r.y2){region=(int)i;break;}}
  if(region<0&&cfg_.filter_outside)continue; out.push_back(d);
  if(region>=0&&cfg_.track_dwell_sec>0&&events){std::ostringstream key;key<<region<<":"<<d.class_id;auto&t=tracks_[key.str()];
   if(t.first_ms==0||iou(t.det,d)<.20f)t={d,ts,ts};else{t.det=d;t.last_ms=ts;}
   float dwell=(t.last_ms-t.first_ms)/1000.0f;if(dwell>=cfg_.track_dwell_sec)events->push_back({region_ids_[region],d.label,dwell});
  }
 }
 return out;
}
