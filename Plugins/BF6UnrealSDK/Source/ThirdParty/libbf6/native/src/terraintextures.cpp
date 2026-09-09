#include "terraintextures.h"

#include <algorithm>

#include "source.h"
#include "terrainlayers.h"

namespace bf6 {

bool load_terrain_texture(Source& src, const std::string& guid, int max_dim,
                          TerrainBindlessTexture& t, std::string& err)
{
    t=TerrainBindlessTexture();err.clear();const auto& pidx=src.partition_index();
    auto pi=pidx.find(guid);if(pi==pidx.end()){err="terrain texture GUID absent from live partition index: "+guid;return false;}
    std::string name=pi->second;if(name.size()>4&&name.compare(name.size()-4,4,".ebx")==0)name.resize(name.size()-4);
    std::vector<uint8_t> res=src.get_res(name,err);if(res.empty()){err="terrain texture resource "+name+": "+err;return false;}
    auto fetch=[&](const std::string& g){std::string e;return src.get_chunk(g,e);};
    t.file_guid=guid;t.resource=name;
    TextureHeader h;if(Texture::read_header(res,h)){t.source_format=h.format;t.source_width=h.width;t.source_height=h.height;t.source_slices=h.slices;}
    const bool ok=max_dim>0?Texture::decode_capped(res,fetch,t.image,max_dim,err):Texture::decode(res,fetch,t.image,0,err);
    if(!ok)err="terrain texture decode "+name+": "+err;
    return ok;
}

bool load_terrain_bindless(Source& src, const TerrainLayers& layers,
                           const std::set<int>& active_layers, int max_dim,
                           TerrainBindlessTable& out, std::string& err)
{
    out=TerrainBindlessTable();err.clear();
    std::set<std::string> guids;
    for(const TerrainLayer& l:layers.layers()){
        if(!active_layers.empty()&&!active_layers.count((int)l.index))continue;
        for(const auto& kv:l.material.raw_textures){out.declarations++;if(!kv.second.empty())guids.insert(kv.second);}
    }
    out.unique_guids=(uint32_t)guids.size();
    uint32_t descriptor=1;
    for(const std::string& guid:guids){
        TerrainBindlessTexture t;if(!load_terrain_texture(src,guid,max_dim,t,err))return false;t.descriptor=descriptor;
        out.descriptor_by_guid[guid]=descriptor++;out.textures.push_back(std::move(t));
    }
    return true;
}

} // namespace bf6
