#include <cassert>
#include <iostream>
#include "infer/RoiFilter.hpp"
#include "infer/TilingTask.hpp"
#include "infer/ThumbnailTask.hpp"
#include "common/SharedDetections.hpp"
int main() {
    Frame f; f.width=8; f.height=4; f.pixel_format=PixelFormat::NV12; f.raw_data.resize(8*4*3/2,128);
    RoiConfig rc; rc.enabled=true; rc.regions.push_back({"desk",0.25f,0.0f,0.5f,1.0f});
    RoiFilter rf(rc);
    Detection inside{3,1,5,3,0,"cup",0.9f}, outside{0,1,1,2,0,"cup",0.9f};
    auto filtered=rf.filter({inside,outside},8,4,1000);
    assert(filtered.size()==1);
    TilingConfig tc; tc.enabled=true; tc.grid_cols=2; tc.grid_rows=1;
    TilingTask tt(tc); auto tiles=tt.make_tiles(8,4,rf.pixel_regions().front()); assert(tiles.size()==2);
    auto crop=tt.crop(f,tiles.front()); assert(crop.raw_data.size()==(size_t)crop.width*crop.height*3/2);

    // 4K 横向切片：确认配置 overlap 会形成明确的公共重叠带，并由两个半框生成联合复检 ROI。
    RoiRect full_roi{0, 0, 3840, 2160};
    auto tiles_4k = tt.make_tiles(3840, 2160, full_roi);
    assert(tiles_4k.size() == 2);
    assert(tiles_4k[0].x == 0 && tiles_4k[1].x < tiles_4k[0].x + tiles_4k[0].width);
    Detection left_half{1500, 600, 2130, 1200, 4, "laptop", 0.80f};
    Detection right_half{1710, 600, 2500, 1200, 4, "laptop", 0.82f};
    auto joint_regions = tt.make_joint_recheck_regions(
        {{left_half, tiles_4k[0]}, {right_half, tiles_4k[1]}}, 3840, 2160);
    assert(joint_regions.size() == 1);
    assert(joint_regions[0].x <= left_half.x1 &&
           joint_regions[0].x + joint_regions[0].width >= right_half.x2);

    // 两个 tile 半框须在 Tracker 前按整帧分割线直接融合为一个框。
    auto fused = tt.merge_boundary_detections(
        {{left_half, tiles_4k[0]}, {right_half, tiles_4k[1]}});
    assert(fused.size() == 1 && fused[0].detection.x1 == left_half.x1 &&
           fused[0].detection.x2 == right_half.x2);

    // 联合复检得到完整框时，必须无条件替换被其覆盖的 tile 半框，不能被中心距离拦住。
    std::vector<TiledDetection> tiled{{left_half, tiles_4k[0]}};
    Detection complete{1450, 560, 2550, 1240, 4, "laptop", 0.90f};
    assert(tt.suppress_tile_duplicates(tiled, {{complete, {1450, 560, 1100, 680},
                                                TiledDetection::Origin::JointRecheck}}) == 1 && tiled.empty());

    // 不同类别但近同框或强包含时只保留一个框，避免最终画面出现竞争框。
    Detection duplicate_book{100, 100, 500, 500, 5, "book", 0.80f};
    Detection duplicate_laptop{106, 104, 496, 498, 4, "laptop", 0.70f};
    auto cross_class_dedup = tt.merge({duplicate_laptop, duplicate_book});
    assert(cross_class_dedup.size() == 1 && cross_class_dedup[0].class_id == 5);
    Detection nested_phone{220, 220, 300, 300, 0, "cell phone", 0.90f};
    Detection outer_book{100, 100, 500, 500, 5, "book", 0.80f};
    auto nested_objects = tt.merge({nested_phone, outer_book});
    assert(nested_objects.size() == 1 && nested_objects[0].class_id == 0);

    // 联合复检优先于贴近 tile 边缘的半框；无论类别如何，强包含竞争组只能留下一个。
    auto exclusive = tt.select_exclusive_candidates({
        {nested_phone, {0, 0, 3840, 2160}, TiledDetection::Origin::Tile},
        {outer_book, {0, 0, 3840, 2160}, TiledDetection::Origin::JointRecheck}});
    assert(exclusive.size() == 1 && exclusive[0].detection.class_id == 5);

    // 同类别包含框在跨帧尺寸变化时必须复用同一条轨迹，不能在 display_hold
    // 期间让旧大框和新小框同时出现在发布快照中。
    TrackerConfig tracker_cfg;
    tracker_cfg.confirm_hits = 1;
    tracker_cfg.display_hold_ms = 100;
    tracker_cfg.align_to_video_pts = true;
    SharedDetections shared(tracker_cfg);
    Detection phone_outer{1243.5f, 347.28f, 1697.78f, 1065.84f, 0, "cell phone", 0.50f};
    Detection phone_inner{1301.03f, 932.0f, 1706.01f, 1071.29f, 0, "cell phone", 0.26f};
    shared.update({phone_outer}, 1, 1000, 3840, 2160);
    shared.update({phone_inner}, 2, 1033, 3840, 2160);
    assert(shared.get_for_pts(1033).detections.size() == 1);

    // 即使同一更新里已经存在两条重复轨迹，状态合并也要在发布前消除它们。
    SharedDetections duplicate_tracks(tracker_cfg);
    duplicate_tracks.update({phone_outer, phone_inner}, 1, 1500, 3840, 2160);
    assert(duplicate_tracks.get_for_pts(1500).detections.size() == 1);

    // 跨类别强包含候选先保持旧稳定框；连续两帧后才受控切换，期间始终只有一个框。
    SharedDetections switched_class(tracker_cfg);
    Detection keyboard{100, 100, 500, 500, 2, "keyboard", 0.70f};
    Detection laptop{150, 150, 450, 450, 4, "laptop", 0.75f};
    switched_class.update({keyboard}, 1, 2000, 3840, 2160);
    switched_class.update({laptop}, 2, 2033, 3840, 2160);
    auto pending_switch = switched_class.get_for_pts(2033).detections;
    assert(pending_switch.size() == 1 && pending_switch[0].class_id == 2);
    switched_class.update({laptop}, 3, 2066, 3840, 2160);
    auto confirmed_switch = switched_class.get_for_pts(2066).detections;
    assert(confirmed_switch.size() == 1 && confirmed_switch[0].class_id == 4);

    SharedDetections duplicate_classes(tracker_cfg);
    duplicate_classes.update({keyboard, laptop}, 1, 2500, 3840, 2160);
    assert(duplicate_classes.get_for_pts(2500).detections.size() == 1);

    ThumbnailConfig hc; hc.enabled=true; hc.width=4; hc.height=2;
    auto thumb=ThumbnailTask(hc).create(f); assert(thumb.data.size()==12);
    std::cout << "image_processing_smoke: OK\n";
}
