#include "terrainjoin.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <regex>
#include <sstream>
#include <unordered_map>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <objidl.h>
#include <oleauto.h>
#include <dxcapi.h>
#endif

namespace bf6 {
namespace {

struct Resource {
    int space = 0;
    int low = 0;
    int size = 0;
    int kind = 0;
};

struct Handle {
    int cls = -1;
    int range = -1;
};

std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<int> metadata_refs(const std::string& text)
{
    static const std::regex re("!([0-9]+)");
    std::vector<int> out;
    for (std::sregex_iterator i(text.begin(), text.end(), re), e; i != e; ++i)
        out.push_back(std::atoi((*i)[1].str().c_str()));
    return out;
}

std::vector<int> i32_values(const std::string& text)
{
    static const std::regex re("i32 (-?[0-9]+)");
    std::vector<int> out;
    for (std::sregex_iterator i(text.begin(), text.end(), re), e; i != e; ++i)
        out.push_back(std::atoi((*i)[1].str().c_str()));
    return out;
}

#if defined(_WIN32)
bool disassemble(const std::string& game_dir,
                 const std::vector<unsigned char>& bytecode,
                 std::string& out, std::string& err)
{
    out.clear();
    std::string dll = game_dir;
    if (!dll.empty() && dll.back() != '\\' && dll.back() != '/') dll += '\\';
    dll += "dxcompiler.dll";
    HMODULE module = LoadLibraryA(dll.c_str());
    if (!module) {
        err = "the mounted BF6 install does not provide dxcompiler.dll";
        return false;
    }
    DxcCreateInstanceProc create = reinterpret_cast<DxcCreateInstanceProc>(
        GetProcAddress(module, "DxcCreateInstance"));
    if (!create) {
        FreeLibrary(module);
        err = "BF6 dxcompiler.dll exports no DxcCreateInstance";
        return false;
    }

    IDxcCompiler3* compiler = nullptr;
    HRESULT hr = create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3),
                        reinterpret_cast<void**>(&compiler));
    if (FAILED(hr) || !compiler) {
        FreeLibrary(module);
        err = "BF6 dxcompiler.dll could not create IDxcCompiler3";
        return false;
    }

    DxcBuffer input{};
    input.Ptr = bytecode.data();
    input.Size = bytecode.size();
    input.Encoding = DXC_CP_ACP;
    IDxcResult* result = nullptr;
    hr = compiler->Disassemble(&input, __uuidof(IDxcResult),
                               reinterpret_cast<void**>(&result));
    if (FAILED(hr) || !result) {
        compiler->Release();
        FreeLibrary(module);
        err = "BF6 dxcompiler.dll rejected the live terrain bytecode";
        return false;
    }

    IDxcBlobUtf8* text = nullptr;
    hr = result->GetOutput(DXC_OUT_DISASSEMBLY, __uuidof(IDxcBlobUtf8),
                           reinterpret_cast<void**>(&text), nullptr);
    if (SUCCEEDED(hr) && text && text->GetStringPointer())
        out.assign(text->GetStringPointer(), text->GetStringLength());
    if (text) text->Release();
    result->Release();
    compiler->Release();
    FreeLibrary(module);
    if (out.empty()) {
        err = "BF6 dxcompiler.dll returned an empty terrain disassembly";
        return false;
    }
    return true;
}
#endif

bool analyze(const std::string& text, TerrainDxilJoin& out, std::string& err)
{
    out = TerrainDxilJoin();
    const std::vector<std::string> lines = lines_of(text);
    std::map<int, std::string> metadata;
    static const std::regex md_re("^!([0-9]+) = !\\{(.*)\\}\\s*$");
    std::smatch m;
    for (const std::string& line : lines)
        if (std::regex_match(line, m, md_re))
            metadata[std::atoi(m[1].str().c_str())] = m[2].str();

    int resource_top = -1;
    for (const std::string& line : lines) {
        if (line.rfind("!dx.resources", 0) != 0) continue;
        const std::vector<int> refs = metadata_refs(line);
        if (!refs.empty()) resource_top = refs.front();
    }
    if (resource_top < 0 || !metadata.count(resource_top)) {
        err = "live terrain DXIL has no resource metadata";
        return false;
    }

    std::map<int, Resource> srvs;
    const std::vector<int> resource_lists = metadata_refs(metadata[resource_top]);
    if (!resource_lists.empty() && metadata.count(resource_lists[0])) {
        for (int entry : metadata_refs(metadata[resource_lists[0]])) {
            auto it = metadata.find(entry);
            if (it == metadata.end()) continue;
            const std::vector<int> nums = i32_values(it->second);
            if (nums.size() < 5) continue;
            Resource r;
            const int range = nums[0];
            r.space = nums[1]; r.low = nums[2]; r.size = nums[3]; r.kind = nums[4];
            srvs[range] = r;
            if (r.space == 0 && r.size == 1 && r.kind == 2)
                out.static_texture_registers.insert(r.low);
        }
    }
    if (srvs.empty()) {
        err = "live terrain DXIL declares no SRV metadata";
        return false;
    }

    size_t function_start = lines.size(), function_end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("define void @", 0) == 0 ||
            lines[i].rfind("define internal void", 0) == 0) {
            function_start = i;
            break;
        }
    }
    if (function_start == lines.size()) {
        err = "live terrain DXIL disassembly has no function body";
        return false;
    }
    for (size_t i = function_start + 1; i < lines.size(); ++i)
        if (lines[i] == "}") { function_end = i; break; }
    if (function_end == lines.size()) {
        err = "live terrain DXIL function body is truncated";
        return false;
    }

    std::vector<int> order(1, -1);
    std::map<int, std::vector<std::string>> blocks;
    blocks[-1] = {};
    int current = -1;
    static const std::regex label_re("^; <label>:([0-9]+)");
    for (size_t i = function_start + 1; i < function_end; ++i) {
        if (std::regex_search(lines[i], m, label_re)) {
            current = std::atoi(m[1].str().c_str());
            blocks[current] = {};
            order.push_back(current);
        } else {
            blocks[current].push_back(lines[i]);
        }
    }

    static const std::regex branch_re(
        "^\\s*br (?:i1 [^,]+, )?label %([0-9]+)(?:, label %([0-9]+))?");
    static const std::regex switch_re(
        "^\\s*switch i32 %[0-9]+, label %([0-9]+) \\[");
    static const std::regex case_re("^\\s*i32 (-?[0-9]+), label %([0-9]+)");
    std::map<int, std::vector<int>> successors;
    int switch_block = -2, default_label = -1;
    std::vector<std::pair<int, int>> best_cases;
    for (const auto& kv : blocks) {
        successors[kv.first] = {};
        for (size_t i = 0; i < kv.second.size(); ++i) {
            const std::string& line = kv.second[i];
            if (std::regex_search(line, m, branch_re)) {
                successors[kv.first].push_back(std::atoi(m[1].str().c_str()));
                if (m[2].matched)
                    successors[kv.first].push_back(std::atoi(m[2].str().c_str()));
            }
            if (!std::regex_search(line, m, switch_re)) continue;
            const int def = std::atoi(m[1].str().c_str());
            std::vector<std::pair<int, int>> cases;
            successors[kv.first].push_back(def);
            for (size_t j = i + 1; j < kv.second.size() &&
                 kv.second[j].find(']') == std::string::npos; ++j) {
                if (!std::regex_search(kv.second[j], m, case_re)) continue;
                const int value = std::atoi(m[1].str().c_str());
                const int label = std::atoi(m[2].str().c_str());
                cases.emplace_back(value, label);
                successors[kv.first].push_back(label);
            }
            if (cases.size() > best_cases.size()) {
                switch_block = kv.first;
                default_label = def;
                best_cases = std::move(cases);
            }
        }
    }
    if (switch_block == -2 || best_cases.empty()) {
        err = "live terrain DXIL has no top-level layer switch";
        return false;
    }

    std::map<int, int> entries;
    entries[0] = default_label;
    for (const auto& c : best_cases) entries[c.first] = c.second;
    std::map<int, std::set<int>> reachable;
    for (const auto& entry : entries) {
        std::deque<int> todo(1, entry.second);
        while (!todo.empty()) {
            const int b = todo.back(); todo.pop_back();
            if (reachable[entry.first].count(b)) continue;
            reachable[entry.first].insert(b);
            if (b == switch_block) continue;
            for (int next : successors[b]) todo.push_back(next);
        }
    }
    std::map<int, int> owner;
    for (const auto& block : blocks) {
        int found = 0, value = 0;
        for (const auto& entry : entries) {
            if (!reachable[entry.first].count(block.first)) continue;
            found++; value = entry.first;
        }
        if (found == 1) owner[block.first] = value;
    }

    static const std::regex handle_re(
        "%([0-9]+) = call %dx\\.types\\.Handle @dx\\.op\\.createHandle\\(i32 57, i8 ([0-9]+), i32 (-?[0-9]+), i32 [^,]+, i1 (?:true|false)\\)");
    static const std::regex sample_re(
        "@dx\\.op\\.(?:sampleGrad|sample|sampleLevel|sampleBias|sampleCmp[^.]*)\\.[a-z0-9]+\\(i32 [0-9]+, %dx\\.types\\.Handle %([0-9]+)");
    std::unordered_map<int, Handle> handles;
    for (const auto& block : blocks)
        for (const std::string& line : block.second)
            if (std::regex_search(line, m, handle_re)) {
                Handle h;
                const int ssa = std::atoi(m[1].str().c_str());
                h.cls = std::atoi(m[2].str().c_str());
                h.range = std::atoi(m[3].str().c_str());
                handles[ssa] = h;
            }

    for (const auto& block : blocks) {
        auto own = owner.find(block.first);
        for (const std::string& line : block.second) {
            if (!std::regex_search(line, m, sample_re)) continue;
            const int ssa = std::atoi(m[1].str().c_str());
            auto hi = handles.find(ssa);
            if (hi == handles.end() || hi->second.cls != 0) continue;
            auto ri = srvs.find(hi->second.range);
            if (ri == srvs.end() || ri->second.space != 0 ||
                ri->second.size != 1 || ri->second.kind != 2) continue;
            if (own == owner.end()) {
                out.unattributed_samples++;
                continue;
            }
            out.registers_by_layer[own->second].insert(ri->second.low);
            out.attributed_samples++;
        }
    }
    if (out.registers_by_layer.empty()) {
        err = "live terrain DXIL sample sites could not be attributed to layers";
        return false;
    }
    return true;
}

} // namespace

bool recover_terrain_dxil_join(const std::string& game_dir,
                               const std::vector<unsigned char>& bytecode,
                               TerrainDxilJoin& out, std::string& err)
{
    out = TerrainDxilJoin();
    err.clear();
    if (bytecode.empty()) {
        err = "live terrain bytecode is empty";
        return false;
    }
#if defined(_WIN32)
    std::string text;
    if (!disassemble(game_dir, bytecode, text, err)) return false;
    return analyze(text, out, err);
#else
    (void)game_dir;
    err = "live terrain DXIL disassembly is currently implemented on Windows";
    return false;
#endif
}

} // namespace bf6
