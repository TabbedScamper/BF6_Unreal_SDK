#include "lightingzones.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>

#include "ebx.h"

namespace bf6 {
namespace {

constexpr uint32_t F_OBJECTS      = 0x5A6115B2;
constexpr uint32_t F_DATAREFS     = 0xCDA4739B;
constexpr uint32_t F_COMPONENTS   = 0x5E00055A;
constexpr uint32_t F_BUNDLENAME   = 0x095E926D;
constexpr uint32_t F_BP_TRANSFORM = 0x7B554EF5;
constexpr uint32_t F_BLUEPRINT    = 0x6B831B88;
constexpr uint32_t F_EXCLUDED     = 0x4CD269A8;
constexpr uint32_t F_SMG_MEMBERS  = 0xA43BD992;

constexpr uint32_t F_LINKS        = 0xF7791E41;
constexpr uint32_t F_PROPERTIES   = 0xC78408A3;
constexpr uint32_t F_EVENTS       = 0xF5DFCFC5;
constexpr uint32_t F_SOURCE       = 0x4676BA2C;
constexpr uint32_t F_TARGET       = 0x645B319C;
constexpr uint32_t F_SOURCE_FIELD = 0xC41FB636;
constexpr uint32_t F_TARGET_FIELD = 0x2D7314C9;
constexpr uint32_t F_TRANSFORM    = 0xD6351EDE;
constexpr uint32_t F_HALF_EXTENTS = 0x0D497F25;
constexpr uint32_t F_POINTS       = 0xEDFC6DF6;
constexpr uint32_t F_HEIGHT       = 0x28CEA299;
constexpr uint32_t F_PROX_DIST    = 0xD6D44B21;
constexpr uint32_t PIN_GEOMETRY   = 0x31FB3B9F;

constexpr uint32_t F_LT_RIGHT   = 0xC478CC3B;
constexpr uint32_t F_LT_UP      = 0xBF151EF9;
constexpr uint32_t F_LT_FORWARD = 0x695D12A4;
constexpr uint32_t F_LT_TRANS   = 0xBC4B07B4;
constexpr uint32_t K_VEC_X = 956422932, K_VEC_Y = 1123815262, K_VEC_Z = 849976220;

const char* K_AREA = "b4a11959-380a-707f-04d8-9d30554ca8fc";
const char* K_OBB  = "c8e55f62-8409-c039-a6bb-fbd11cb03739";
const char* K_POLY = "9fc7ba2d-7564-b0a6-8a9c-61f3fd93e55d";
const char* K_VE_REF = "9c791c39-8fc4-b787-d7a1-3982f139c621";
const char* K_LT = "06ce1d10-9a4e-fc64-2a9f-a9d482576ffa";

const uint32_t kDescendFields[] = {
    F_BP_TRANSFORM, F_EXCLUDED, F_BUNDLENAME, F_BLUEPRINT,
    F_OBJECTS, F_DATAREFS, F_COMPONENTS
};

const char* kSkipSubworld[] = { "_autotests", "_tools", "marketing", "glacierflow/" };
const char* kDestructionBranch[] = {
    "464ab605-1fb8-3d21-202f-f3feb43dffb9", "591bcc82-5380-7cbd-543e-faabbc873c09",
    "ef0c40ad-226b-f6fe-04dd-7b26c26350a2", "96e64a4c-c7e1-644c-dedb-499e51d965c2",
    "570f0b4f-fcd0-d977-580e-d409f464c6f1", "e788769d-af38-c1e6-d413-10cffb99e5f8",
    "643b508d-7f3e-af14-3022-033b9ced8d79", "b3120863-e2a0-5eb4-4523-a3f4dc814e20",
    "a0e4f0a2-5007-928a-20de-1f613062529c", "0f86bea6-d792-51ab-8d91-b724ea871296",
};
constexpr int kMaxDepth = 24;

std::string lower(std::string s) {
    for (char& c : s) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
    return s;
}
std::string leaf_of(const std::string& s) {
    const size_t i = s.find_last_of('/');
    return i == std::string::npos ? s : s.substr(i + 1);
}
bool ends_with(const std::string& s, const std::string& t) {
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}
std::string strip_ebx(std::string n) {
    if (ends_with(lower(n), ".ebx")) n.resize(n.size() - 4);
    return n;
}

TypeGuid raw_guid(const std::string& dashed) {
    TypeGuid g{}; std::string h;
    for (char c : dashed) if (c != '-') h += c;
    if (h.size() != 32) return g;
    auto byte = [&](size_t i) { return (uint8_t)std::stoul(h.substr(i * 2, 2), nullptr, 16); };
    g[0]=byte(3); g[1]=byte(2); g[2]=byte(1); g[3]=byte(0);
    g[4]=byte(5); g[5]=byte(4); g[6]=byte(7); g[7]=byte(6);
    for (int i=8;i<16;i++) g[(size_t)i]=byte((size_t)i);
    return g;
}

Vec3 vec_of(const EbxValue* d) {
    Vec3 out;
    if (!d || d->kind != EbxValue::Kind::Struct) return out;
    auto get = [&](uint32_t h) -> float {
        const EbxValue* f=d->field(h); if (!f) return 0.f;
        if (f->kind==EbxValue::Kind::Real) return (float)f->f;
        if (f->kind==EbxValue::Kind::Int) return (float)f->i;
        if (f->kind==EbxValue::Kind::Uint) return (float)f->u;
        return 0.f;
    };
    out.x=get(K_VEC_X); out.y=get(K_VEC_Y); out.z=get(K_VEC_Z); return out;
}
float as_f(const EbxValue* v) {
    if (!v) return 0.f;
    if (v->kind==EbxValue::Kind::Real) return (float)v->f;
    if (v->kind==EbxValue::Kind::Int) return (float)v->i;
    if (v->kind==EbxValue::Kind::Uint) return (float)v->u;
    return 0.f;
}
Mat34 lt_to_mat(const EbxValue* t, const TypeGuid& lt) {
    Mat34 out=mat_identity();
    if (!t || t->kind!=EbxValue::Kind::Struct || t->guid!=lt) return out;
    static const uint32_t m[4]={F_LT_RIGHT,F_LT_UP,F_LT_FORWARD,F_LT_TRANS};
    Mat34 b;
    for (int i=0;i<4;i++) { const EbxValue* v=t->field(m[i]); if (!v) return out; b.m[i]=vec_of(v); }
    return b;
}

int ref_index(const EbxValue* v) {
    return v && v->kind==EbxValue::Kind::InstanceRef ? v->instance : -1;
}

class ZoneWalk {
public:
    ZoneWalk(Source& s, TypeDb& t) : src(s), types(t), area(raw_guid(K_AREA)),
        obb(raw_guid(K_OBB)), poly(raw_guid(K_POLY)), ve_ref(raw_guid(K_VE_REF)), lt(raw_guid(K_LT)) {}
    bool run(const std::string& level, std::string& err);
    std::vector<LightingZone> zones;
    LightingZoneStats st;

private:
    void build_catalog();
    std::string resolve_name(const std::string&) const;
    bool type_matters(Ebx&, size_t);
    void walk_ref(const std::string&, const Mat34&, std::set<std::string>, int);
    void visit(const EbxValue&, const Mat34&, const std::string&, const std::set<std::string>&, int);
    void collect_partition(Ebx&, const Mat34&, const std::string&);
    std::string reachable_preset(Ebx&, int, const std::map<int,std::vector<int>>&);

    Source& src; TypeDb& types;
    std::unordered_map<std::string,std::string> by_name;
    std::map<std::string,std::string> gi;
    std::map<TypeGuid,bool> matters;
    std::set<std::string> destruction;
    TypeGuid area{},obb{},poly{},ve_ref{},lt{};
};

void ZoneWalk::build_catalog() {
    for (const auto& kv:src.ebx()) {
        const std::string low=lower(kv.first), ref=kv.first+".ebx";
        by_name.emplace(low,ref); by_name.emplace(leaf_of(low),ref);
    }
    gi=src.partition_index();
    for (const char* d:kDestructionBranch) destruction.insert(d);
}
std::string ZoneWalk::resolve_name(const std::string& name) const {
    if (name.empty()) return {};
    const std::string n=lower(name);
    for (const std::string& k:{n,leaf_of(n),n+".ebx"}) {
        auto it=by_name.find(k); if (it!=by_name.end()) return it->second;
    }
    return {};
}
bool ZoneWalk::type_matters(Ebx& e,size_t i) {
    const TypeGuid g=e.instance_type(i); auto it=matters.find(g); if(it!=matters.end()) return it->second;
    const TypeLayout& lay=types.layout_full(g); bool yes=false;
    if(!lay.valid){st.unresolved_types++;yes=true;}
    else {
        bool smg=false;
        for(const FieldInfo& f:lay.fields){
            if(f.name_hash==F_SMG_MEMBERS){smg=true;break;}
            for(uint32_t h:kDescendFields) if(f.name_hash==h){yes=true;break;}
        }
        if(smg) yes=false;
    }
    matters[g]=yes; return yes;
}

std::string ZoneWalk::reachable_preset(Ebx& e,int start,const std::map<int,std::vector<int>>& graph) {
    std::queue<int> q; std::set<int> seen; q.push(start); seen.insert(start);
    while(!q.empty()) {
        const int n=q.front(); q.pop();
        if(n>=0 && (size_t)n<e.instance_count() && e.instance_type((size_t)n)==ve_ref) {
            const std::vector<uint32_t> want{F_BLUEPRINT};
            const EbxValue d=e.read_instance((size_t)n,&want);
            const EbxValue* b=d.field(F_BLUEPRINT);
            if(b && b->kind==EbxValue::Kind::ImportRef) {
                std::string p=strip_ebx(b->import_path);
                const std::string l=lower(p), leaf=leaf_of(l);
                if(!p.empty() && (leaf.rfind("ve_",0)==0 || l.find("/lighting/ve_")!=std::string::npos)) return p;
            }
        }
        auto it=graph.find(n); if(it==graph.end()) continue;
        for(int v:it->second) if(v>=0 && seen.insert(v).second) q.push(v);
    }
    return {};
}

void ZoneWalk::collect_partition(Ebx& e,const Mat34& parent,const std::string& ref) {
    std::vector<int> areas;
    for(size_t i=0;i<e.instance_count();i++) if(e.instance_type(i)==area) areas.push_back((int)i);
    if(areas.empty()) return;
    st.proximity+=areas.size();

    struct LinkEdge { int source, target; uint32_t source_field, target_field; };
    std::vector<LinkEdge> links;
    std::set<std::pair<int,int>> geometry_link_set;
    std::map<int,std::vector<int>> graph;
    const std::vector<uint32_t> want{F_LINKS,F_PROPERTIES,F_EVENTS};
    for(size_t i=0;i<e.instance_count();i++) {
        const TypeLayout& lay=types.layout_full(e.instance_type(i));
        bool has=false; if(lay.valid) for(const FieldInfo& f:lay.fields)
            if(f.name_hash==F_LINKS || f.name_hash==F_PROPERTIES || f.name_hash==F_EVENTS){has=true;break;}
        if(!has) continue;
        const EbxValue root=e.read_instance(i,&want);
        auto add=[&](uint32_t h,bool is_link){
            const EbxValue* a=root.field(h); if(!a || a->kind!=EbxValue::Kind::Array) return;
            for(const EbxValue& x:a->items) {
                if(x.kind!=EbxValue::Kind::Struct) continue;
                int s=ref_index(x.field(F_SOURCE)),t=ref_index(x.field(F_TARGET));
                if(s<0 || t<0) continue;
                graph[s].push_back(t);
                if(is_link) {
                    const EbxValue* sf=x.field(F_SOURCE_FIELD);
                    const EbxValue* tf=x.field(F_TARGET_FIELD);
                    uint32_t source_field=0, target_field=0;
                    if(sf && sf->kind==EbxValue::Kind::Int) source_field=(uint32_t)sf->i;
                    else if(sf && sf->kind==EbxValue::Kind::Uint) source_field=(uint32_t)sf->u;
                    if(tf && tf->kind==EbxValue::Kind::Int) target_field=(uint32_t)tf->i;
                    else if(tf && tf->kind==EbxValue::Kind::Uint) target_field=(uint32_t)tf->u;
                    links.push_back({s,t,source_field,target_field});
                    if(source_field==PIN_GEOMETRY && target_field==0)
                        geometry_link_set.insert({s,t});
                }
            }
        };
        add(F_LINKS,true); add(F_PROPERTIES,false); add(F_EVENTS,false);
    }

    const std::set<int> area_set(areas.begin(),areas.end());
    for(const auto& edge:links) {
        if(!area_set.count(edge.source)) continue;
        if(edge.source_field!=PIN_GEOMETRY){st.non_geometry_links++;continue;}
        if(edge.target_field!=0){st.non_shape_geometry_links++;continue;}
        st.link_edges++;
        const int fake=(edge.source+1)%(int)e.instance_count();
        if(geometry_link_set.count({fake,edge.target})) st.rotated_control_hits++;
        const TypeGuid tg=e.instance_type((size_t)edge.target);
        if(tg==obb) st.target_obb++;
        else if(tg==poly) st.target_polygon++;
        else {
            st.target_other++;
            if(st.target_other_examples.size()<20) {
                st.target_other_examples.push_back(
                    TypeDb::guid_str(tg)+"|"+strip_ebx(ref)+"|"+
                    std::to_string(edge.source)+"->"+std::to_string(edge.target));
            }
            continue;
        }

        const std::string preset=reachable_preset(e,edge.source,graph);
        if(preset.empty()){st.omitted_no_preset++;continue;}
        st.joined_preset++;

        const std::vector<uint32_t> awant{F_PROX_DIST};
        const EbxValue ad=e.read_instance((size_t)edge.source,&awant);
        LightingZone z; z.preset=preset; z.source=strip_ebx(ref);
        z.proximity_instance=edge.source; z.shape_instance=edge.target;
        z.fade_distance=as_f(ad.field(F_PROX_DIST));
        if(tg==obb) {
            const std::vector<uint32_t> swant{F_TRANSFORM,F_HALF_EXTENTS};
            const EbxValue d=e.read_instance((size_t)edge.target,&swant);
            const EbxValue* tx=d.field(F_TRANSFORM),*he=d.field(F_HALF_EXTENTS);
            if(!tx || !he){st.malformed_shape++;continue;}
            z.kind=kZoneObb; z.xf=mat_mul(parent,lt_to_mat(tx,lt)); z.half_extents=vec_of(he);
        } else {
            const std::vector<uint32_t> swant{F_POINTS,F_HEIGHT};
            const EbxValue d=e.read_instance((size_t)edge.target,&swant);
            const EbxValue* pts=d.field(F_POINTS);
            if(!pts || pts->kind!=EbxValue::Kind::Array || pts->items.size()<3){st.malformed_shape++;continue;}
            z.kind=kZonePolygon; z.xf=parent; z.height=as_f(d.field(F_HEIGHT));
            for(const EbxValue& p:pts->items) z.points.push_back(vec_of(&p));
        }
        zones.push_back(std::move(z));
    }
}

void ZoneWalk::visit(const EbxValue& inst,const Mat34& parent,const std::string& ref,
                     const std::set<std::string>& guard,int depth) {
    if(destruction.count(TypeDb::guid_str(inst.guid))) return;
    const EbxValue* local=inst.field(F_BP_TRANSFORM);
    const Mat34 world=(local && local->kind==EbxValue::Kind::Struct) ? mat_mul(parent,lt_to_mat(local,lt)) : parent;
    if(const EbxValue* ex=inst.field(F_EXCLUDED))
        if((ex->kind==EbxValue::Kind::Bool&&ex->b)||(ex->kind==EbxValue::Kind::Uint&&ex->u)||(ex->kind==EbxValue::Kind::Int&&ex->i)) return;
    if(const EbxValue* bn=inst.field(F_BUNDLENAME)) if(bn->kind==EbxValue::Kind::Str&&!bn->s.empty()) {
        const std::string l=lower(bn->s); for(const char* s:kSkipSubworld) if(l.find(s)!=std::string::npos) return;
        const std::string sub=resolve_name(bn->s); if(!sub.empty()) walk_ref(sub,world,guard,depth+1); return;
    }
    if(const EbxValue* bp=inst.field(F_BLUEPRINT)) if(bp->kind==EbxValue::Kind::ImportRef&&!bp->import_path.empty()) {
        walk_ref(bp->import_path,world,guard,depth+1); return;
    }
    for(uint32_t h:{F_OBJECTS,F_DATAREFS,F_COMPONENTS}) if(const EbxValue* a=inst.field(h))
        if(a->kind==EbxValue::Kind::Array) for(const EbxValue& c:a->items)
            if(c.kind==EbxValue::Kind::Struct) visit(c,world,ref,guard,depth);
}

void ZoneWalk::walk_ref(const std::string& ref,const Mat34& parent,std::set<std::string> guard,int depth) {
    if(depth>kMaxDepth||ref.empty()) return;
    const std::string key=lower(ref); if(guard.count(key)){st.cycles++;return;}
    std::string name=strip_ebx(ref),err; std::vector<uint8_t> raw=src.get_ebx(name,err);
    if(raw.empty()){st.missing++;return;}
    Ebx e(types); e.set_guid_index(&gi); if(!e.parse(std::move(raw),err)){st.parse_fail++;return;}
    guard.insert(key); st.partitions++;
    collect_partition(e,parent,ref);
    static const std::vector<uint32_t> want(std::begin(kDescendFields),std::end(kDescendFields));
    for(size_t i=0;i<e.instance_count();i++) {
        st.instances++; if(!type_matters(e,i)) continue;
        const EbxValue d=e.read_instance(i,&want); if(d.kind==EbxValue::Kind::Struct) visit(d,parent,ref,guard,depth);
    }
}

bool ZoneWalk::run(const std::string& level_rel,std::string& err) {
    err.clear(); zones.clear(); st=LightingZoneStats(); build_catalog();
    std::string rel=lower(level_rel); while(!rel.empty()&&rel.back()=='/') rel.pop_back();
    const std::string leaf=leaf_of(rel); std::string start=resolve_name(rel+"/"+leaf);
    if(start.empty()) start=resolve_name(rel); if(start.empty()) start=resolve_name(leaf);
    if(start.empty()&&rel.find('/')==std::string::npos) {
        std::vector<std::string> tails{"/levels/"+leaf+"/"+leaf};
        if(leaf.rfind("mp_",0)!=0) tails.push_back("/levels/mp_"+leaf+"/mp_"+leaf);
        for(const std::string& tail:tails) {
            std::vector<std::string> hits; for(const auto& kv:by_name) if(ends_with(kv.first,tail)) hits.push_back(kv.first);
            if(hits.empty()) continue; std::sort(hits.begin(),hits.end()); start=by_name[hits[0]]; break;
        }
    }
    if(start.empty()){err="could not resolve the level root for "+level_rel;return false;}
    walk_ref(start,mat_identity(),{},0); return true;
}

} // namespace

bool level_lighting_zones(Source& src,TypeDb& types,const std::string& level,
                          std::vector<LightingZone>& out,LightingZoneStats& stats,std::string& err) {
    ZoneWalk w(src,types); if(!w.run(level,err)) return false;
    out=std::move(w.zones); stats=w.st; return true;
}

} // namespace bf6
