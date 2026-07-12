#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "infer/ImageProcessing.hpp"
struct RoiRect { float x1=0,y1=0,x2=0,y2=0; };
struct RoiEvent { std::string region_id; std::string label; float dwell_sec=0; };
class RoiFilter {
public:
 explicit RoiFilter(const RoiConfig& cfg);
 std::vector<Detection> filter(const std::vector<Detection>& detections,int width,int height,int64_t timestamp_ms,std::vector<RoiEvent>* events=nullptr);
 const std::vector<RoiRect>& pixel_regions() const{return pixel_regions_;}
private:
 struct Track{Detection det;int64_t first_ms=0,last_ms=0;};
 RoiConfig cfg_;
 std::vector<RoiRect> pixel_regions_;
 std::vector<std::string> region_ids_;
 std::unordered_map<std::string,Track> tracks_;
};
