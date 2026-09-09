#include "terrainpage.h"

#include <cctype>
#include <set>

#include "source.h"
#include "terrainlayers.h"
#include "terrainstatic.h"
#include "terrain.h"

namespace bf6 {

bool prepare_terrain_page(Source& src,const std::string& level,
                          const TerrainPageOpts& opt,TerrainPageInputs& out,
                          std::string& err)
{
    out=TerrainPageInputs();err.clear();GroundCoverageOpts co;co.size=opt.size;co.max_slots=16;
    co.rect_min[0]=opt.rect_min[0];co.rect_min[1]=opt.rect_min[1];co.rect_size=opt.rect_size;
    if(!ground_coverage(src,level,co,out.coverage,err)||
       !build_terrain_mask_atlas(out.coverage,out.masks,err)||
       !load_terrain_shader(src,level,out.shader,err))return false;
    std::string lvl=level,tree;for(char& c:lvl)c=(char)std::tolower((unsigned char)c);
    for(const auto& kv:src.res()){std::string n=kv.first;for(char& c:n)c=(char)std::tolower((unsigned char)c);if(n.find("streamingtree")!=std::string::npos&&n.find(lvl)!=std::string::npos){tree=kv.first;break;}}
    if(tree.empty()){err="no streaming tree for terrain height window";return false;}
    std::vector<uint8_t> tres=src.get_res(tree,err);Terrain terrain;if(tres.empty()||!terrain.parse(tres,err))return false;
    terrain.resolve_external([&](const std::string& guid){std::string e;return src.get_chunk(guid,e);});
    if(!terrain.sample_window(opt.rect_min[0],opt.rect_min[1],opt.rect_size,opt.size,2,out.height,err))return false;
    TerrainLayers layers;if(!layers.load(src,level,err))return false;
    std::set<int> active;for(const auto& kv:out.masks.layer_panel)active.insert(kv.first);
    if(!load_terrain_bindless(src,layers,active,opt.texture_max_dim,out.bindless,err)||
       !build_terrain_layer_rows(out.shader,layers,out.bindless.descriptor_by_guid,
                                 out.rows,out.row_stats,err))return false;

    TerrainStaticTable table;if(!table.load(src,level,err))return false;
    for(const auto& kv:table.textures()){
        TerrainStaticBinding b;b.descriptor=kv.first;
        if(!load_terrain_texture(src,kv.second.file_guid,opt.texture_max_dim,b.texture,err)){
            TerrainStaticUnresolved u;u.descriptor=kv.first;u.file_guid=kv.second.file_guid;
            u.resource=b.texture.resource;u.source_format=b.texture.source_format;
            u.source_width=b.texture.source_width;u.source_height=b.texture.source_height;
            u.source_slices=b.texture.source_slices;u.error=err;
            out.unresolved_statics.push_back(std::move(u));err.clear();continue;
        }
        b.texture.descriptor=kv.first;out.statics.push_back(std::move(b));
    }
    return true;
}

} // namespace bf6
