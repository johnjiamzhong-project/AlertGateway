#pragma once
#include <vector>
#include "infer/ImageProcessing.hpp"
#include "infer/RoiFilter.hpp"
struct TileRect { int x=0,y=0,width=0,height=0; };
struct TiledDetection {
    enum class Origin { Tile, JointRecheck };
    Detection detection;
    TileRect tile;
    Origin origin = Origin::Tile;
};
class TilingTask {
public:
    explicit TilingTask(const TilingConfig& cfg) : cfg_(cfg) {}
    std::vector<TileRect> make_tiles(int frame_width,int frame_height,const RoiRect& roi) const;
    Frame crop(const Frame& frame,const TileRect& tile) const;
    std::vector<TileRect> make_joint_recheck_regions(const std::vector<TiledDetection>& detections,
                                                      int frame_width, int frame_height) const;
    std::vector<TiledDetection> merge_boundary_detections(std::vector<TiledDetection> detections);
    size_t suppress_tile_duplicates(std::vector<TiledDetection>& tiled,
                                    const std::vector<TiledDetection>& full_frame) const;
    std::vector<TiledDetection> select_exclusive_candidates(
        std::vector<TiledDetection> detections) const;
    static std::vector<Detection> detections_only(std::vector<TiledDetection> detections);
    size_t last_boundary_merge_count() const { return last_boundary_merge_count_; }
    size_t last_cross_class_duplicate_count() const { return last_cross_class_duplicate_count_; }
    size_t last_exclusive_suppression_count() const { return last_exclusive_suppression_count_; }
    std::vector<Detection> merge(std::vector<Detection> detections) const;
private:
    TilingConfig cfg_;
    size_t last_boundary_merge_count_ = 0;
    mutable size_t last_cross_class_duplicate_count_ = 0;
    mutable size_t last_exclusive_suppression_count_ = 0;
};
