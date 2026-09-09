#include "expression_registry.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include "stdio_compat.h"

namespace bf6 { namespace expression {
namespace {

struct Section {
    std::string name;
    uint32_t va = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_offset = 0;
    uint32_t raw_size = 0;
    uint32_t characteristics = 0;
};

static bool fits(const std::vector<uint8_t>& d, size_t off, size_t bytes)
{
    return off <= d.size() && bytes <= d.size() - off;
}

static uint16_t rd16(const std::vector<uint8_t>& d, size_t off)
{
    uint16_t v = 0; std::memcpy(&v, d.data() + off, sizeof(v)); return v;
}

static uint32_t rd32(const std::vector<uint8_t>& d, size_t off)
{
    uint32_t v = 0; std::memcpy(&v, d.data() + off, sizeof(v)); return v;
}

static uint64_t rd64(const std::vector<uint8_t>& d, size_t off)
{
    uint64_t v = 0; std::memcpy(&v, d.data() + off, sizeof(v)); return v;
}

static bool executable_va(uint64_t va, uint64_t image_base,
                          const std::vector<Section>& sections)
{
    if (va < image_base) return false;
    const uint64_t rva = va - image_base;
    for (const Section& s : sections) {
        if (!(s.characteristics & 0x20000000u)) continue;
        const uint64_t span = std::max(s.virtual_size, s.raw_size);
        if (rva >= s.va && rva < (uint64_t)s.va + span) return true;
    }
    return false;
}

static int64_t va_to_file(uint64_t va, uint64_t image_base,
                          const std::vector<Section>& sections,
                          size_t file_size)
{
    if (va < image_base) return -1;
    const uint64_t rva = va - image_base;
    for (const Section& s : sections) {
        const uint64_t span = std::max(s.virtual_size, s.raw_size);
        if (rva < s.va || rva >= (uint64_t)s.va + span) continue;
        const uint64_t file = (uint64_t)s.raw_offset + (rva - s.va);
        return file < file_size ? (int64_t)file : -1;
    }
    return -1;
}

} // namespace

bool read_descriptor_operators(const std::string& exe_path,
                               std::vector<DescriptorOperator>& out,
                               std::string& error)
{
    out.clear(); error.clear();
    FILE* f = fopen_binary_read(exe_path.c_str());
    if (!f) { error = "cannot open " + exe_path; return false; }
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size < 0x100) {
        std::fclose(f); error = "executable is too small to be a PE"; return false;
    }
    std::vector<uint8_t> data((size_t)file_size);
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) {
        error = "short executable read"; return false;
    }
    if (rd16(data, 0) != 0x5a4d) { error = "missing MZ header"; return false; }
    const uint32_t pe = rd32(data, 0x3c);
    if (!fits(data, pe, 24) || rd32(data, pe) != 0x00004550u) {
        error = "missing PE header"; return false;
    }
    const uint16_t section_count = rd16(data, pe + 6);
    const uint16_t optional_size = rd16(data, pe + 20);
    const size_t optional = (size_t)pe + 24;
    if (!fits(data, optional, optional_size) || optional_size < 32 ||
        rd16(data, optional) != 0x20b) {
        error = "unsupported PE optional header"; return false;
    }
    const uint64_t image_base = rd64(data, optional + 24);
    const size_t section_table = optional + optional_size;
    std::vector<Section> sections;
    for (uint16_t i = 0; i < section_count; ++i) {
        const size_t at = section_table + (size_t)i * 40;
        if (!fits(data, at, 40)) { error = "truncated PE section table"; return false; }
        Section s;
        char name[9] = {};
        std::memcpy(name, data.data() + at, 8);
        s.name = name;
        s.virtual_size = rd32(data, at + 8);
        s.va = rd32(data, at + 12);
        s.raw_size = rd32(data, at + 16);
        s.raw_offset = rd32(data, at + 20);
        s.characteristics = rd32(data, at + 36);
        sections.push_back(s);
    }

    for (const Section& s : sections) {
        // Registry records are writable data. Scanning executable sections
        // would create millions of instruction-immediate false candidates.
        if (!(s.characteristics & 0x80000000u) ||
            (s.characteristics & 0x20000000u)) continue;
        if (!fits(data, s.raw_offset, s.raw_size)) continue;
        const size_t begin = s.raw_offset;
        const size_t end = begin + s.raw_size;
        for (size_t at = begin; at + 32 <= end; at += 8) {
            const uint64_t implementation = rd64(data, at);
            const uint32_t key = rd32(data, at + 8);
            const uint32_t flags = rd32(data, at + 12);
            const uint64_t self = rd64(data, at + 16);
            const uint64_t zero = rd64(data, at + 24);
            const uint64_t record_va = image_base + s.va + (at - begin);
            if (self != record_va || zero != 0 || flags > 1 ||
                !executable_va(implementation, image_base, sections)) continue;
            out.push_back({key, flags, implementation, record_va});
        }
    }
    std::sort(out.begin(), out.end(), [](const DescriptorOperator& a,
                                         const DescriptorOperator& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.record_va < b.record_va;
    });
    if (out.empty()) {
        error = "no expression descriptor records passed the structural scan";
        return false;
    }
    return true;
}

bool read_method_operators(const std::string& exe_path,
                           const std::vector<uint32_t>& query_keys,
                           std::vector<MethodOperator>& out,
                           std::string& error)
{
    out.clear(); error.clear();
    const std::set<uint32_t> wanted(query_keys.begin(), query_keys.end());
    if (wanted.empty()) return true;
    FILE* f = fopen_binary_read(exe_path.c_str());
    if (!f) { error = "cannot open " + exe_path; return false; }
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size < 0x100) {
        std::fclose(f); error = "executable is too small to be a PE"; return false;
    }
    std::vector<uint8_t> data((size_t)file_size);
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) { error = "short executable read"; return false; }
    if (rd16(data, 0) != 0x5a4d) { error = "missing MZ header"; return false; }
    const uint32_t pe = rd32(data, 0x3c);
    if (!fits(data, pe, 24) || rd32(data, pe) != 0x00004550u) {
        error = "missing PE header"; return false;
    }
    const uint16_t section_count = rd16(data, pe + 6);
    const uint16_t optional_size = rd16(data, pe + 20);
    const size_t optional = (size_t)pe + 24;
    if (!fits(data, optional, optional_size) || optional_size < 32 ||
        rd16(data, optional) != 0x20b) {
        error = "unsupported PE optional header"; return false;
    }
    const uint64_t image_base = rd64(data, optional + 24);
    const size_t section_table = optional + optional_size;
    std::vector<Section> sections;
    for (uint16_t i = 0; i < section_count; ++i) {
        const size_t at = section_table + (size_t)i * 40;
        if (!fits(data, at, 40)) { error = "truncated PE section table"; return false; }
        Section s;
        char name[9] = {};
        std::memcpy(name, data.data() + at, 8);
        s.name = name;
        s.virtual_size = rd32(data, at + 8);
        s.va = rd32(data, at + 12);
        s.raw_size = rd32(data, at + 16);
        s.raw_offset = rd32(data, at + 20);
        s.characteristics = rd32(data, at + 36);
        sections.push_back(s);
    }

    for (const Section& s : sections) {
        if (!(s.characteristics & 0x80000000u) ||
            (s.characteristics & 0x20000000u) ||
            !fits(data, s.raw_offset, s.raw_size)) continue;
        const size_t begin = s.raw_offset;
        const size_t end = begin + s.raw_size;
        for (size_t at = begin; at + 16 <= end; at += 8) {
            const uint64_t implementation = rd64(data, at);
            const uint32_t key = rd32(data, at + 8);
            const uint32_t flags = rd32(data, at + 12);
            if (wanted.find(key) == wanted.end() || flags > 1u ||
                !executable_va(implementation, image_base, sections)) continue;
            const uint64_t record_va = image_base + s.va + (at - begin);
            // The 32-byte descriptor registry begins with this same 16-byte
            // prefix. Exclude its independently proven self+zero suffix.
            if (at + 32 <= end && rd64(data, at + 16) == record_va &&
                rd64(data, at + 24) == 0) continue;
            MethodOperator row;
            row.implementation_va = implementation;
            row.key = key;
            row.flags = flags;
            row.record_va = record_va;
            out.push_back(row);
        }
    }
    std::sort(out.begin(), out.end(), [](const MethodOperator& a,
                                         const MethodOperator& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.record_va < b.record_va;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const MethodOperator& a,
                                                     const MethodOperator& b) {
        return a.key == b.key && a.record_va == b.record_va;
    }), out.end());
    return true;
}

bool read_reflected_operators(const std::string& exe_path,
                              std::vector<ReflectedOperator>& out,
                              std::string& error)
{
    out.clear(); error.clear();
    FILE* f = fopen_binary_read(exe_path.c_str());
    if (!f) { error = "cannot open " + exe_path; return false; }
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size < 0x100) {
        std::fclose(f); error = "executable is too small to be a PE"; return false;
    }
    std::vector<uint8_t> data((size_t)file_size);
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) { error = "short executable read"; return false; }
    const uint32_t pe = rd32(data, 0x3c);
    if (!fits(data, pe, 24) || rd32(data, pe) != 0x00004550u) {
        error = "missing PE header"; return false;
    }
    const uint16_t section_count = rd16(data, pe + 6);
    const uint16_t optional_size = rd16(data, pe + 20);
    const size_t optional = (size_t)pe + 24;
    if (!fits(data, optional, optional_size) || optional_size < 32 ||
        rd16(data, optional) != 0x20b) {
        error = "unsupported PE optional header"; return false;
    }
    const uint64_t image_base = rd64(data, optional + 24);
    const size_t section_table = optional + optional_size;
    std::vector<Section> sections;
    for (uint16_t i = 0; i < section_count; ++i) {
        const size_t at = section_table + (size_t)i * 40;
        if (!fits(data, at, 40)) { error = "truncated PE section table"; return false; }
        Section s;
        char name[9] = {};
        std::memcpy(name, data.data() + at, 8);
        s.name = name;
        s.virtual_size = rd32(data, at + 8);
        s.va = rd32(data, at + 12);
        s.raw_size = rd32(data, at + 16);
        s.raw_offset = rd32(data, at + 20);
        s.characteristics = rd32(data, at + 36);
        sections.push_back(s);
    }
    const Section* typeinfo = nullptr;
    for (const Section& s : sections)
        if (s.name == "typeinfo") { typeinfo = &s; break; }
    if (!typeinfo || !fits(data, typeinfo->raw_offset, typeinfo->raw_size)) {
        error = "typeinfo section is missing or truncated"; return false;
    }
    const size_t begin = typeinfo->raw_offset;
    const size_t end = begin + typeinfo->raw_size;
    for (size_t guid_at = begin + 8; guid_at + 96 <= end; guid_at += 8) {
        const uint16_t flags = rd16(data, guid_at - 4);
        const uint16_t object_size = rd16(data, guid_at - 2);
        if (flags != 0x030du || object_size != 8) continue;
        bool guid_nonzero = false;
        for (size_t i = 0; i < 16; ++i) guid_nonzero |= data[guid_at + i] != 0;
        if (!guid_nonzero) continue;
        const uint64_t namespace_va = rd64(data, guid_at + 16);
        const uint64_t array_type_va = rd64(data, guid_at + 24);
        const uint16_t alignment = rd16(data, guid_at + 32);
        const uint16_t parameter_count = rd16(data, guid_at + 34);
        const uint32_t signature = rd32(data, guid_at + 36);
        const uint64_t unused_1 = rd64(data, guid_at + 48);
        const uint64_t unused_4 = rd64(data, guid_at + 72);
        const uint64_t parameters_va = rd64(data, guid_at + 80);
        if (array_type_va != 0 || alignment != 8 || parameter_count > 256 ||
            unused_1 != 0 || unused_4 != 0 ||
            va_to_file(namespace_va, image_base, sections, data.size()) < 0 ||
            (parameter_count == 0
                ? parameters_va != 0
                : va_to_file(parameters_va, image_base, sections, data.size()) < 0))
            continue;
        ReflectedOperator row;
        row.key = rd32(data, guid_at - 8);
        row.parameter_count = parameter_count;
        row.signature = signature;
        row.descriptor_va = image_base + typeinfo->va + (guid_at - begin);
        row.parameters_va = parameters_va;
        out.push_back(row);
    }
    std::sort(out.begin(), out.end(), [](const ReflectedOperator& a,
                                         const ReflectedOperator& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.descriptor_va < b.descriptor_va;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const ReflectedOperator& a,
                                                     const ReflectedOperator& b) {
        return a.key == b.key;
    }), out.end());
    if (out.empty()) { error = "no reflected Function descriptors found"; return false; }
    return true;
}

uint32_t operator_name_crc32(const uint8_t* bytes, size_t size)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i << 24;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc & 0x80000000u) ?
                    (crc << 1) ^ 0x04c11db7u : crc << 1;
            result[i] = crc;
        }
        return result;
    }();
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i)
        crc = (crc << 8) ^ table[((crc >> 24) ^ bytes[i]) & 0xffu];
    return ~crc;
}

bool resolve_named_operators(const std::string& exe_path,
                             const std::vector<uint32_t>& query_keys,
                             std::vector<NamedOperator>& out,
                             std::string& error)
{
    out.clear(); error.clear();
    std::set<uint32_t> wanted(query_keys.begin(), query_keys.end());
    if (wanted.empty()) return true;
    FILE* f = fopen_binary_read(exe_path.c_str());
    if (!f) { error = "cannot open " + exe_path; return false; }
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size <= 0) { std::fclose(f); error = "empty executable"; return false; }
    std::vector<uint8_t> data((size_t)file_size);
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) { error = "short executable read"; return false; }

    std::map<uint32_t, std::set<std::string>> matches;
    size_t at = 0;
    while (at < data.size()) {
        if (data[at] < 0x20 || data[at] > 0x7e) { ++at; continue; }
        const size_t begin = at;
        while (at < data.size() && data[at] >= 0x20 && data[at] <= 0x7e &&
               at - begin <= 127) ++at;
        const size_t length = at - begin;
        // Registration names are ordinary C literals. Requiring the NUL is a
        // rejection gate against hashing instruction/data fragments.
        if (length >= 2 && length <= 127 && at < data.size() && data[at] == 0) {
            const uint32_t key = operator_name_crc32(data.data() + begin, length);
            if (wanted.find(key) != wanted.end())
                matches[key].insert(std::string((const char*)data.data() + begin, length));
        }
        // A run longer than 127 was deliberately not accepted. Advance past
        // the rest of it so suffixes cannot masquerade as separate literals.
        while (at < data.size() && data[at] >= 0x20 && data[at] <= 0x7e) ++at;
        if (at < data.size()) ++at;
    }
    for (uint32_t key : wanted) {
        const auto it = matches.find(key);
        if (it == matches.end()) continue;
        NamedOperator row;
        row.key = key;
        row.match_count = (uint32_t)it->second.size();
        if (row.match_count == 1) row.name = *it->second.begin();
        out.push_back(std::move(row));
    }
    return true;
}

}} // namespace bf6::expression
