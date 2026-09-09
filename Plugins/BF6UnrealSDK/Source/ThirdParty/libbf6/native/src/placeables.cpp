#include "placeables.h"
#include "json.hpp"

#include <fstream>
#include <sstream>
#include <cstdio>

namespace bf6 {

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// A property's default value -> a display/edit string. Complex defaults
// (arrays/objects like a vector's components) come back "" for now.
static std::string stringify_scalar(const bf6json::Value& v) {
    switch (v.type) {
        case bf6json::Value::Bool: return v.b ? "true" : "false";
        case bf6json::Value::Str:  return v.str;
        case bf6json::Value::Num: {
            double d = v.num;
            if (d == (long long)d) return std::to_string((long long)d);
            char buf[32]; std::snprintf(buf, sizeof(buf), "%g", d); return buf;
        }
        default: return "";
    }
}

bool PlaceableDB::allowed_on(const Placeable& p, const std::string& level) {
    if (level.empty()) return true;      // no level context -> list everything
    if (p.universal) return true;        // no restrictions -> universal
    for (const auto& l : p.levels) if (l == level) return true;
    return false;
}

bool PlaceableDB::load(const std::string& fbexport_dir, std::string& err) {
    // --- levels -------------------------------------------------------------
    std::string body;
    if (!read_file(fbexport_dir + "/level_info.json", body)) {
        err = "cannot open level_info.json in " + fbexport_dir;
        return false;
    }
    {
        std::string je;
        bf6json::Value root = bf6json::parse(body.data(), body.size(), je);
        if (!root.is_obj()) { err = "level_info.json: " + je; return false; }
        for (const auto& kv : root.obj) levels_.push_back(kv.first);
    }

    // --- placeables ---------------------------------------------------------
    if (!read_file(fbexport_dir + "/asset_types.json", body)) {
        err = "cannot open asset_types.json in " + fbexport_dir;
        return false;
    }
    std::string je;
    bf6json::Value root = bf6json::parse(body.data(), body.size(), je);
    const bf6json::Value* arr = root.find("AssetTypes");
    if (!arr || !arr->is_arr()) { err = "asset_types.json: no AssetTypes array (" + je + ")"; return false; }

    items_.reserve(arr->arr.size());
    for (const auto& e : arr->arr) {
        if (!e.is_obj()) continue;
        Placeable p;
        if (const auto* t = e.find("type"))      p.type      = t->as_str(p.type);
        if (const auto* d = e.find("directory")) p.directory = d->as_str(p.directory);
        if (p.type.empty()) continue;   // an unnamed entry is not placeable

        // constants[] carries {name:"mesh"|"physicsCost"|..., value:...}
        if (const auto* consts = e.find("constants"); consts && consts->is_arr()) {
            for (const auto& c : consts->arr) {
                const auto* nm = c.find("name");
                if (!nm || !nm->is_str()) continue;
                const auto* val = c.find("value");
                if (!val) continue;
                if (nm->str == "mesh")             p.mesh = val->as_str(p.mesh);
                else if (nm->str == "physicsCost") p.physics_cost = val->as_int(0);
            }
        }

        // levelRestrictions[] - absent or empty means universal.
        if (const auto* lr = e.find("levelRestrictions"); lr && lr->is_arr() && !lr->arr.empty()) {
            for (const auto& l : lr->arr) if (l.is_str()) p.levels.push_back(l.str);
            p.universal = p.levels.empty();
        } else {
            p.universal = true;
        }

        // properties[] - the SDK's editable fields: {name, type, default}.
        if (const auto* props = e.find("properties"); props && props->is_arr()) {
            for (const auto& pr : props->arr) {
                if (!pr.is_obj()) continue;
                PlaceProp pp;
                if (const auto* nm = pr.find("name")) pp.name = nm->as_str(pp.name);
                if (const auto* ty = pr.find("type")) pp.type = ty->as_str(pp.type);
                if (const auto* dv = pr.find("default")) pp.def = stringify_scalar(*dv);
                // selections[] - the enum option list (the SDK's dropdowns)
                if (const auto* sel = pr.find("selections"); sel && sel->is_arr()) {
                    for (const auto& s : sel->arr) {
                        if (!s.is_str()) continue;
                        if (!pp.selections.empty()) pp.selections += '\n';
                        pp.selections += s.str;
                    }
                }
                if (!pp.name.empty()) p.props.push_back(std::move(pp));
            }
        }

        items_.push_back(std::move(p));
    }

    loaded_ = true;
    return true;
}

}  // namespace bf6
