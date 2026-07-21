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
    // 对 4K 左右 tile 使用 384~512 像素重叠；比例仍由配置控制，便于单变量验证。
    const float configured_ratio = std::clamp(cfg_.overlap_ratio, 0.01f, 0.50f);
    const int overlap_px = std::max(384, std::min(512,
        static_cast<int>(std::lround(rw * configured_ratio))));
    std::vector<TileRect> out;
    for(int r=0;r<rows;++r) for(int c=0;c<cols;++c) {
        const int left_overlap = c > 0 ? overlap_px / 2 : 0;
        const int right_overlap = c + 1 < cols ? overlap_px - overlap_px / 2 : 0;
        const int top_overlap = r > 0 ? overlap_px / 2 : 0;
        const int bottom_overlap = r + 1 < rows ? overlap_px - overlap_px / 2 : 0;
        int sx = x0 + (rw * c) / cols - left_overlap;
        int sy = y0 + (rh * r) / rows - top_overlap;
        int ex = x0 + (rw * (c + 1)) / cols + right_overlap;
        int ey = y0 + (rh * (r + 1)) / rows + bottom_overlap;
        sx = std::max(x0, sx); sy = std::max(y0, sy);
        ex = std::min(x2, ex); ey = std::min(y2, ey);
        out.push_back({sx & ~1, sy & ~1, (ex - sx) & ~1, (ey - sy) & ~1});
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

namespace {
float overlap_ratio_1d(float a1, float a2, float b1, float b2) {
    const float overlap = std::max(0.0f, std::min(a2, b2) - std::max(a1, b1));
    const float shorter = std::min(a2 - a1, b2 - b1);
    return shorter > 0.0f ? overlap / shorter : 0.0f;
}

float interval_gap(float a1, float a2, float b1, float b2) {
    if (a2 < b1) return b1 - a2;
    if (b2 < a1) return a1 - b2;
    return 0.0f;
}

float box_area(const Detection& detection) {
    return std::max(0.0f, detection.x2 - detection.x1) *
           std::max(0.0f, detection.y2 - detection.y1);
}

float intersection_area(const Detection& a, const Detection& b) {
    const float x1 = std::max(a.x1, b.x1);
    const float y1 = std::max(a.y1, b.y1);
    const float x2 = std::min(a.x2, b.x2);
    const float y2 = std::min(a.y2, b.y2);
    return std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
}

bool center_inside(const Detection& inner, const Detection& outer) {
    const float cx = (inner.x1 + inner.x2) * 0.5f;
    const float cy = (inner.y1 + inner.y2) * 0.5f;
    return cx >= outer.x1 && cx <= outer.x2 &&
           cy >= outer.y1 && cy <= outer.y2;
}

float center_distance(const Detection& a, const Detection& b) {
    const float ax = (a.x1 + a.x2) * 0.5f;
    const float ay = (a.y1 + a.y2) * 0.5f;
    const float bx = (b.x1 + b.x2) * 0.5f;
    const float by = (b.y1 + b.y2) * 0.5f;
    return std::hypot(ax - bx, ay - by);
}

float tile_edge_clearance(const TiledDetection& item) {
    const auto& d = item.detection;
    const auto& t = item.tile;
    return std::max(0.0f, std::min({d.x1 - t.x, d.y1 - t.y,
                                    t.x + t.width - d.x2,
                                    t.y + t.height - d.y2}));
}

bool tiles_overlap(const TileRect& a, const TileRect& b) {
    return std::min(a.x + a.width, b.x + b.width) > std::max(a.x, b.x) &&
           std::min(a.y + a.height, b.y + b.height) > std::max(a.y, b.y);
}

TileRect shared_overlap(const TileRect& a, const TileRect& b) {
    const int x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width, b.x + b.width);
    const int y2 = std::min(a.y + a.height, b.y + b.height);
    return {x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
}

bool intersects_rect(const Detection& d, const TileRect& r) {
    return std::min(d.x2, static_cast<float>(r.x + r.width)) > std::max(d.x1, static_cast<float>(r.x)) &&
           std::min(d.y2, static_cast<float>(r.y + r.height)) > std::max(d.y1, static_cast<float>(r.y));
}
}

std::vector<TileRect> TilingTask::make_joint_recheck_regions(
    const std::vector<TiledDetection>& detections, int fw, int fh) const {
    // 两个半框往往落在公共重叠带中间，不能再用 tile 的物理外边缘触发复检。
    constexpr float kMinOrthogonalOverlap = 0.30f;
    constexpr float kMaxParallelGap = 384.0f;
    constexpr int kPaddingX = 256, kPaddingY = 160;
    std::vector<TileRect> result;
    for (size_t i = 0; i < detections.size(); ++i) {
        for (size_t j = i + 1; j < detections.size(); ++j) {
            const auto& a = detections[i]; const auto& b = detections[j];
            if (a.detection.class_id != b.detection.class_id || !tiles_overlap(a.tile, b.tile)) continue;
            const TileRect overlap = shared_overlap(a.tile, b.tile);
            if (!intersects_rect(a.detection, overlap) || !intersects_rect(b.detection, overlap)) continue;
            const bool horizontal_split = overlap.width < overlap.height;
            const float orthogonal = horizontal_split
                ? overlap_ratio_1d(a.detection.y1, a.detection.y2, b.detection.y1, b.detection.y2)
                : overlap_ratio_1d(a.detection.x1, a.detection.x2, b.detection.x1, b.detection.x2);
            const float gap = horizontal_split
                ? interval_gap(a.detection.x1, a.detection.x2, b.detection.x1, b.detection.x2)
                : interval_gap(a.detection.y1, a.detection.y2, b.detection.y1, b.detection.y2);
            if (orthogonal < kMinOrthogonalOverlap || gap > kMaxParallelGap) continue;
            int x1 = std::max(0, static_cast<int>(std::floor(std::min(a.detection.x1, b.detection.x1))) - kPaddingX);
            int y1 = std::max(0, static_cast<int>(std::floor(std::min(a.detection.y1, b.detection.y1))) - kPaddingY);
            int x2 = std::min(fw, static_cast<int>(std::ceil(std::max(a.detection.x2, b.detection.x2))) + kPaddingX);
            int y2 = std::min(fh, static_cast<int>(std::ceil(std::max(a.detection.y2, b.detection.y2))) + kPaddingY);
            // NV12 的 RGA 裁剪宽度/起点需要 16 字节对齐；向外扩展不丢失目标，
            // 且坐标映射使用扩展后的实际 ROI，避免复检回退到 CPU。
            x1 &= ~15;
            x2 = std::min(fw, (x2 + 15) & ~15);
            y1 &= ~1; y2 &= ~1;
            TileRect candidate{x1, y1, std::max(2, x2 - x1), std::max(2, y2 - y1)};
            bool merged = false;
            for (TileRect& existing : result) {
                if (!tiles_overlap(existing, candidate)) continue;
                const int mx1 = std::min(existing.x, candidate.x), my1 = std::min(existing.y, candidate.y);
                const int mx2 = std::max(existing.x + existing.width, candidate.x + candidate.width);
                const int my2 = std::max(existing.y + existing.height, candidate.y + candidate.height);
                existing = {mx1, my1, mx2 - mx1, my2 - my1};
                merged = true;
                break;
            }
            if (!merged) result.push_back(candidate);
        }
    }
    return result;
}

std::vector<TiledDetection> TilingTask::merge_boundary_detections(
    std::vector<TiledDetection> detections) {
    last_boundary_merge_count_ = 0;
    // 所有 tile 框在调用前已经映射回整帧坐标。本阶段只看整帧的公共分割线，
    // 将同一目标的两个半框先合为一个测量，再交给 Tracker，禁止各自建轨。
    constexpr float kMinOrthogonalOverlap = 0.45f;
    constexpr float kMaxBoundaryGap = 96.0f;
    constexpr float kMinSeamMargin = 96.0f;

    for (size_t i = 0; i < detections.size(); ++i) {
        if (detections[i].detection.score < 0.0f) continue;
        for (size_t j = i + 1; j < detections.size(); ++j) {
            if (detections[i].detection.score < 0.0f) break;
            if (detections[j].detection.score < 0.0f) continue;
            auto& left = detections[i];
            auto& right = detections[j];
            if (left.detection.class_id != right.detection.class_id) continue;
            if (!tiles_overlap(left.tile, right.tile)) continue;
            const TileRect overlap = shared_overlap(left.tile, right.tile);
            const bool horizontal_split = overlap.width < overlap.height;
            const TiledDetection* first = &left;
            const TiledDetection* second = &right;
            if (horizontal_split) {
                if (first->tile.x > second->tile.x) std::swap(first, second);
            } else if (first->tile.y > second->tile.y) {
                std::swap(first, second);
            }

            const float seam = horizontal_split
                ? overlap.x + overlap.width * 0.5f
                : overlap.y + overlap.height * 0.5f;
            const float seam_margin = std::max(kMinSeamMargin,
                (horizontal_split ? overlap.width : overlap.height) * 0.25f);
            const bool both_near_seam = horizontal_split
                ? first->detection.x2 >= seam - seam_margin &&
                  second->detection.x1 <= seam + seam_margin
                : first->detection.y2 >= seam - seam_margin &&
                  second->detection.y1 <= seam + seam_margin;
            const float orthogonal_overlap = horizontal_split
                ? overlap_ratio_1d(first->detection.y1, first->detection.y2,
                                   second->detection.y1, second->detection.y2)
                : overlap_ratio_1d(first->detection.x1, first->detection.x2,
                                   second->detection.x1, second->detection.x2);
            const float boundary_gap = horizontal_split
                ? interval_gap(first->detection.x1, first->detection.x2,
                               second->detection.x1, second->detection.x2)
                : interval_gap(first->detection.y1, first->detection.y2,
                               second->detection.y1, second->detection.y2);
            const bool should_merge = both_near_seam &&
                orthogonal_overlap >= kMinOrthogonalOverlap &&
                boundary_gap <= kMaxBoundaryGap;
            if (!should_merge) continue;

            left.detection.x1 = std::min(left.detection.x1, right.detection.x1);
            left.detection.y1 = std::min(left.detection.y1, right.detection.y1);
            left.detection.x2 = std::max(left.detection.x2, right.detection.x2);
            left.detection.y2 = std::max(left.detection.y2, right.detection.y2);
            left.detection.score = std::max(left.detection.score, right.detection.score);
            right.detection.score = -1.0f;
            ++last_boundary_merge_count_;
        }
    }

    // 对没有刚好贴住公共边界、但在重叠区产生的同类重复框做 IoS 去重。
    // 两框都可信时优先保留离各自 tile 边缘更远的框；清晰度接近时合并范围。
    constexpr float kMinIntersectionOverSmaller = 0.35f;
    constexpr float kEdgePreferenceMargin = 32.0f;
    for (size_t i = 0; i < detections.size(); ++i) {
        if (detections[i].detection.score < 0.0f) continue;
        for (size_t j = i + 1; j < detections.size(); ++j) {
            if (detections[i].detection.score < 0.0f) break;
            if (detections[j].detection.score < 0.0f) continue;
            auto& first = detections[i];
            auto& second = detections[j];
            if (first.detection.class_id != second.detection.class_id) continue;
            const float smaller = std::min(box_area(first.detection), box_area(second.detection));
            if (smaller <= 0.0f || intersection_area(first.detection, second.detection) / smaller <
                    kMinIntersectionOverSmaller) continue;
            const float first_clearance = tile_edge_clearance(first);
            const float second_clearance = tile_edge_clearance(second);
            if (std::abs(first_clearance - second_clearance) >= kEdgePreferenceMargin) {
                if (first_clearance < second_clearance) first.detection.score = -1.0f;
                else second.detection.score = -1.0f;
            } else {
                first.detection.x1 = std::min(first.detection.x1, second.detection.x1);
                first.detection.y1 = std::min(first.detection.y1, second.detection.y1);
                first.detection.x2 = std::max(first.detection.x2, second.detection.x2);
                first.detection.y2 = std::max(first.detection.y2, second.detection.y2);
                first.detection.score = std::max(first.detection.score, second.detection.score);
                second.detection.score = -1.0f;
            }
        }
    }

    std::vector<TiledDetection> merged;
    merged.reserve(detections.size());
    for (auto& item : detections) {
        if (item.detection.score >= 0.0f) merged.push_back(std::move(item));
    }
    return merged;
}

size_t TilingTask::suppress_tile_duplicates(
    std::vector<TiledDetection>& tiled,
    const std::vector<TiledDetection>& full_frame) const {
    size_t suppressed = 0;
    constexpr float kMinTileCoverage = 0.50f;
    constexpr float kMinIntersectionOverSmaller = 0.35f;
    for (auto tiled_it = tiled.begin(); tiled_it != tiled.end();) {
        bool duplicate = false;
        const float tiled_area = box_area(tiled_it->detection);
        for (const auto& full : full_frame) {
            if (tiled_it->detection.class_id != full.detection.class_id || tiled_area <= 0.0f) continue;
            const float overlap = intersection_area(tiled_it->detection, full.detection) / tiled_area;
            const float full_area = box_area(full.detection);
            const float full_coverage = intersection_area(tiled_it->detection, full.detection) /
                std::max(1.0f, full_area);
            const float ios = intersection_area(tiled_it->detection, full.detection) /
                std::max(1.0f, std::min(tiled_area, full_area));
            // 完整框包含 tile 半框时，两者中心会相距较远，不能再用中心距离否决。
            if (overlap >= kMinTileCoverage || full_coverage >= kMinTileCoverage ||
                ios >= kMinIntersectionOverSmaller) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            tiled_it = tiled.erase(tiled_it);
            ++suppressed;
        } else {
            ++tiled_it;
        }
    }
    return suppressed;
}

std::vector<TiledDetection> TilingTask::select_exclusive_candidates(
    std::vector<TiledDetection> detections) const {
    // 最终绘制层不允许同一位置同时竞争两种框。这里仅处理“近乎同框”或
    // 小框中心被大框包住且小框绝大部分被覆盖的强竞争关系，避免误删相邻物体。
    constexpr float kCrossClassDuplicateIoU = 0.85f;
    constexpr float kStrongContainment = 0.50f;
    last_exclusive_suppression_count_ = 0;
    const auto reliable_first = [](const TiledDetection& a, const TiledDetection& b) {
        if (a.origin != b.origin) return a.origin == TiledDetection::Origin::JointRecheck;
        const float a_clearance = a.origin == TiledDetection::Origin::Tile ? tile_edge_clearance(a) : 0.0f;
        const float b_clearance = b.origin == TiledDetection::Origin::Tile ? tile_edge_clearance(b) : 0.0f;
        if (std::abs(a_clearance - b_clearance) > 1.0f) return a_clearance > b_clearance;
        return a.detection.score > b.detection.score;
    };
    std::sort(detections.begin(), detections.end(), reliable_first);
    std::vector<TiledDetection> kept;
    kept.reserve(detections.size());
    for (auto& candidate : detections) {
        bool competing = false;
        for (const auto& winner : kept) {
            const float smaller = std::min(box_area(winner.detection), box_area(candidate.detection));
            const float containment = smaller > 0.0f
                ? intersection_area(winner.detection, candidate.detection) / smaller : 0.0f;
            const Detection& inner = box_area(winner.detection) <= box_area(candidate.detection)
                ? winner.detection : candidate.detection;
            const Detection& outer = box_area(winner.detection) <= box_area(candidate.detection)
                ? candidate.detection : winner.detection;
            if (iou(winner.detection, candidate.detection) >= kCrossClassDuplicateIoU ||
                (containment >= kStrongContainment && center_inside(inner, outer))) {
                competing = true;
                ++last_exclusive_suppression_count_;
                break;
            }
        }
        if (!competing) kept.push_back(std::move(candidate));
    }
    return kept;
}

std::vector<Detection> TilingTask::detections_only(std::vector<TiledDetection> detections) {
    std::vector<Detection> result;
    result.reserve(detections.size());
    for (auto& candidate : detections) result.push_back(std::move(candidate.detection));
    return result;
}

std::vector<Detection> TilingTask::merge(std::vector<Detection> detections) const {
    last_cross_class_duplicate_count_ = 0;
    std::sort(detections.begin(),detections.end(),[](const auto&a,const auto&b){return a.score>b.score;});
    std::vector<Detection> out;
    constexpr float kContainmentThreshold = 0.70f;
    constexpr float kContainmentScoreTie = 0.10f;
    for(const auto& d:detections) {
        bool dup=false;
        for(auto& k:out) {
            if (k.class_id != d.class_id) continue;
            if (iou(k,d) > cfg_.merge_iou_threshold) {
                dup = true;
                break;
            }

            const float area_k = box_area(k);
            const float area_d = box_area(d);
            const float smaller_area = std::min(area_k, area_d);
            if (smaller_area <= 0.0f) continue;
            const float containment = intersection_area(k, d) / smaller_area;
            const Detection& inner = area_k <= area_d ? k : d;
            const Detection& outer = area_k <= area_d ? d : k;
            if (containment >= kContainmentThreshold && center_inside(inner, outer)) {
                // 检测分数接近时优先保留覆盖范围更大的框，避免整帧复检留下小框。
                if (area_d > area_k && d.score + kContainmentScoreTie >= k.score) {
                    k = d;
                }
                dup = true;
                break;
            }
        }
        if(!dup) out.push_back(d);
    }

    // 类别无关的近同框或强包含关系属于同一个视觉竞争组，只能留下一个框。
    constexpr float kCrossClassDuplicateIoU = 0.85f;
    constexpr float kCrossClassContainment = 0.50f;
    std::sort(out.begin(), out.end(),
              [](const Detection& a, const Detection& b) { return a.score > b.score; });
    std::vector<Detection> cross_class_filtered;
    cross_class_filtered.reserve(out.size());
    for (const Detection& detection : out) {
        bool duplicate = false;
        for (const Detection& kept : cross_class_filtered) {
            if (kept.class_id == detection.class_id) continue;
            const float smaller_area = std::min(box_area(kept), box_area(detection));
            const float containment = smaller_area > 0.0f
                ? intersection_area(kept, detection) / smaller_area : 0.0f;
            const Detection& inner = box_area(kept) <= box_area(detection) ? kept : detection;
            const Detection& outer = box_area(kept) <= box_area(detection) ? detection : kept;
            if (iou(kept, detection) < kCrossClassDuplicateIoU &&
                !(containment >= kCrossClassContainment && center_inside(inner, outer))) continue;
            duplicate = true;
            ++last_cross_class_duplicate_count_;
            break;
        }
        if (!duplicate) cross_class_filtered.push_back(detection);
    }
    return cross_class_filtered;
}
