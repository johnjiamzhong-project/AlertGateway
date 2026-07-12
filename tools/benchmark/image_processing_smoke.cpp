#include <cassert>
#include <iostream>
#include "infer/RoiFilter.hpp"
#include "infer/TilingTask.hpp"
#include "infer/ThumbnailTask.hpp"
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
    ThumbnailConfig hc; hc.enabled=true; hc.width=4; hc.height=2;
    auto thumb=ThumbnailTask(hc).create(f); assert(thumb.data.size()==12);
    std::cout << "image_processing_smoke: OK\n";
}
