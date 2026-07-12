#pragma once
#include <vector>
#include "infer/ImageProcessing.hpp"
#include "infer/RoiFilter.hpp"
struct TileRect { int x=0,y=0,width=0,height=0; };
class TilingTask {
public:
    explicit TilingTask(const TilingConfig& cfg) : cfg_(cfg) {}
    std::vector<TileRect> make_tiles(int frame_width,int frame_height,const RoiRect& roi) const;
    Frame crop(const Frame& frame,const TileRect& tile) const;
    std::vector<Detection> merge(std::vector<Detection> detections) const;
private:
    TilingConfig cfg_;
};
