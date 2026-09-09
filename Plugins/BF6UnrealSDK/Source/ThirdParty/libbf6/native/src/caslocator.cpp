#include "caslocator.h"

#include <cstdio>
#include <filesystem>

#include "stdio_compat.h"

namespace fs = std::filesystem;

namespace bf6 {

static std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = fopen_binary_read(path.c_str());
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize((size_t)n);
        size_t got = std::fread(out.data(), 1, (size_t)n, f);
        out.resize(got);
    }
    std::fclose(f);
    return out;
}

bool CasLocator::open(const std::string& game_dir, std::string& err) {
    game_ = game_dir;
    std::string layout = game_dir + "/Data/layout.toc";
    std::vector<uint8_t> raw = read_file(layout);
    if (raw.empty()) { err = "cannot read " + layout; return false; }

    DbValue lay = read_dbobject(raw.data(), raw.size(), err);
    if (lay.type != DbValue::Object) { err = "layout.toc is not a DbObject"; return false; }

    const DbValue* im = lay.find("installManifest");
    if (!im || im->type != DbValue::Object) { err = "no installManifest"; return false; }

    const DbValue* chunks = im->find("installChunks");
    if (!chunks || chunks->type != DbValue::Array) { err = "no installChunks"; return false; }

    for (const DbValue& c : chunks->arr) {
        if (c.type != DbValue::Object) continue;
        const DbValue* pi = c.find("persistentIndex");
        if (pi && pi->type == DbValue::Int) {
            // persistentIndex is a signed i32; the TOC reads the chunk id as an
            // unsigned u32. Same bits, so mask to u32 for the lookup to match.
            by_index_[(uint32_t)(pi->i & 0xFFFFFFFF)] = c;
        }
    }
    return true;
}

std::string CasLocator::cas_path(uint32_t install_chunk_id, int cas_index) const {
    auto it = by_index_.find(install_chunk_id & 0xFFFFFFFFu);
    if (it == by_index_.end()) return "";
    const DbValue& c = it->second;

    std::string bundle;
    const DbValue* ib = c.find("installBundle");
    if (ib && ib->type == DbValue::String) bundle = ib->s;

    char leaf_name[32];
    std::snprintf(leaf_name, sizeof(leaf_name), "cas_%02d.cas", cas_index);
    std::string leaf = bundle.empty() ? std::string(leaf_name)
                                      : bundle + "/" + leaf_name;

    // Data/<leaf> first, then Update/<pkg>/Data/<leaf> for every Update package.
    std::string cand = game_ + "/Data/" + leaf;
    if (fs::exists(cand)) return cand;

    std::string upd = game_ + "/Update";
    std::error_code ec;
    if (fs::is_directory(upd, ec)) {
        for (const auto& e : fs::directory_iterator(upd, ec)) {
            if (!e.is_directory()) continue;
            std::string p = e.path().string() + "/Data/" + leaf;
            if (fs::exists(p)) return p;
        }
    }
    return "";
}

}  // namespace bf6
