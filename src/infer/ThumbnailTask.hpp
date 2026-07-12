#pragma once
#include "infer/ImageProcessing.hpp"
class ThumbnailTask {
public:
    explicit ThumbnailTask(const ThumbnailConfig& cfg) : cfg_(cfg) {}
    Thumbnail create(const Frame& frame) const;
private:
    ThumbnailConfig cfg_;
};
