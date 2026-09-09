#include "terrainmask.h"

#include <algorithm>
#include <set>

#include "groundsplat.h"

namespace bf6 {

bool build_terrain_mask_atlas(const GroundCoverage& g,
                              TerrainMaskAtlas& out, std::string& err)
{
    out = TerrainMaskAtlas(); err.clear();
    if (g.size <= 0 || g.slots <= 0 ||
        g.idx.size() != (size_t)g.size * g.size * g.slots ||
        g.w.size() != g.idx.size())
    { err = "invalid camera coverage raster"; return false; }
    if ((g.size & 7) != 0)
    { err = "terrain evaluator output size must be a multiple of 8"; return false; }

    std::set<int> active;
    for (size_t i=0;i<g.idx.size();++i) {
        const uint8_t mi=g.idx[i];
        if (!g.w[i] || mi==255 || mi>=g.materials.size()) continue;
        active.insert(g.materials[mi].layer);
    }
    if (active.size() > 64) { err = "camera window needs more than 64 mask panels"; return false; }

    out.source_size=g.size;out.atlas_size=g.size*out.grid;
    out.r8.assign((size_t)out.atlas_size*out.atlas_size,0);
    int panel=0;
    const float span_x=g.hi[0]-g.lo[0],span_z=g.hi[1]-g.lo[1];
    if (!(span_x>0.f&&span_z>0.f)) { err="camera coverage has empty world bounds"; return false; }
    for(int layer:active){
        out.layer_panel[layer]=panel;
        const int px=panel%out.grid,pz=panel/out.grid;
        TerrainMaskTransform t;
        t.scale_x=1.f/(span_x*out.grid);t.scale_z=1.f/(span_z*out.grid);
        t.bias_x=(float)px/out.grid-g.lo[0]*t.scale_x;
        t.bias_z=(float)pz/out.grid-g.lo[1]*t.scale_z;
        out.transform[layer]=t;panel++;
    }

    for(int z=0;z<g.size;z++)for(int x=0;x<g.size;x++){
        const size_t p=(size_t)z*g.size+x;
        for(int s=0;s<g.slots;s++){
            const size_t at=p*g.slots+s;const uint8_t mi=g.idx[at],w=g.w[at];
            if(!w||mi==255||mi>=g.materials.size())continue;
            const int q=out.layer_panel[g.materials[mi].layer];
            const int ax=(q%out.grid)*g.size+x,az=(q/out.grid)*g.size+z;
            out.r8[(size_t)az*out.atlas_size+ax]=w;
        }
    }

    out.tiles_per_side=g.size/8;
    const int nt=out.tiles_per_side*out.tiles_per_side;
    out.list_offset.resize(nt);out.list_count.resize(nt);
    out.tile_descriptor.resize(nt);out.packed_head.resize(nt);
    for(int tz=0;tz<out.tiles_per_side;tz++)for(int tx=0;tx<out.tiles_per_side;tx++){
        std::set<int> tile;
        for(int z=tz*8;z<tz*8+8;z++)for(int x=tx*8;x<tx*8+8;x++){
            const size_t p=(size_t)z*g.size+x;
            for(int s=0;s<g.slots;s++){
                const size_t at=p*g.slots+s;const uint8_t mi=g.idx[at];
                if(!g.w[at]||mi==255||mi>=g.materials.size())continue;
                tile.insert(g.materials[mi].layer);
            }
        }
        const int ti=tz*out.tiles_per_side+tx;
        out.list_offset[ti]=(uint32_t)(out.packed_work.size()/2);
        out.list_count[ti]=(uint32_t)tile.size();
        out.tile_descriptor[ti]=(uint32_t)(tx*8)|((uint32_t)(tz*8)<<16);
        if(tile.size()>255 || out.list_offset[ti]>0x00ffffffu)
        { err="terrain tile work list exceeds the shipped head encoding";return false; }
        out.packed_head[ti]=out.list_count[ti]|(out.list_offset[ti]<<8);
        for(auto it=tile.rbegin();it!=tile.rend();++it){
            const uint32_t layer=(uint32_t)*it;
            out.packed_work.push_back((layer<<26)|layer);
            out.packed_work.push_back(0);
        }
    }
    return true;
}

} // namespace bf6
