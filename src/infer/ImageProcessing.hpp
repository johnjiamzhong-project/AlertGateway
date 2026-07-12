#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "common/Frame.hpp"

struct ThumbnailConfig {
    bool enabled = false;
    int width = 320;
    int height = 180;
    bool on_detection_only = true;
};
struct RoiRegionConfig {
    std::string id;
    float x = 0, y = 0, w = 1, h = 1;
};
struct RoiConfig {
    bool enabled = false;
    std::vector<RoiRegionConfig> regions;
    bool filter_outside = true;
    float track_dwell_sec = 0;
};
struct TilingConfig {
    bool enabled = false;
    int grid_cols = 2, grid_rows = 1;
    float overlap_ratio = 0.10f;
    float merge_iou_threshold = 0.45f;
};
struct ImageProcessingConfig {
    ThumbnailConfig thumbnail;
    RoiConfig roi;
    TilingConfig tiling;
};
struct Thumbnail {
    int width = 0, height = 0;
    std::string format = "nv12";
    std::vector<uint8_t> data;
};
