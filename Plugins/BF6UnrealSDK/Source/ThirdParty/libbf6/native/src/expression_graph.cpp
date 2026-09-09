#include "expression_graph.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace bf6 { namespace expression {
namespace {

static uint16_t u16(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t u64(const uint8_t* p)
{
    return (uint64_t)u32(p) | ((uint64_t)u32(p + 4) << 32);
}

static bool add_ok(size_t a, size_t b, size_t& out)
{
    if (b > std::numeric_limits<size_t>::max() - a) return false;
    out = a + b;
    return true;
}

static bool mul_ok(size_t a, size_t b, size_t& out)
{
    if (a && b > std::numeric_limits<size_t>::max() / a) return false;
    out = a * b;
    return true;
}

static size_t align16(size_t v)
{
    return (v + 15u) & ~size_t(15u);
}

static bool parse_typed_table(const uint8_t* data, size_t begin, size_t bytes,
                              uint32_t destination_size,
                              std::vector<TypedValueGroup>& out,
                              std::string& error)
{
    const size_t end = begin + bytes;
    size_t at = begin;
    while (at < end) {
        if (end - at < 0x28) {
            error = "typed-value table ends inside a group header";
            return false;
        }
        // Constructor/context and destructor/context are relocation holes on
        // disk. A non-zero pointer is evidence that this is not the raw image.
        if (u64(data + at + 4) || u64(data + at + 0x0c) ||
            u64(data + at + 0x14) || u64(data + at + 0x1c)) {
            error = "typed-value group contains a patched runtime pointer";
            return false;
        }
        const uint32_t count = u32(data + at + 0x24);
        size_t offset_bytes = 0;
        if (!mul_ok((size_t)count, 4, offset_bytes) ||
            offset_bytes > end - at - 0x28) {
            error = "typed-value offset count exceeds its declared table";
            return false;
        }
        TypedValueGroup group;
        group.data_type_id = u32(data + at);
        group.offsets.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t off = u32(data + at + 0x28 + (size_t)i * 4);
            if (off >= destination_size && destination_size != 0) {
                error = "typed-value offset exceeds its destination region";
                return false;
            }
            group.offsets.push_back(off);
        }
        out.push_back(std::move(group));
        at += 0x28 + offset_bytes;
    }
    if (at != end) {
        error = "typed-value table did not walk to its declared end";
        return false;
    }
    return true;
}

// Fixed lengths measured without exception in the published 379-graph
// independent corpus. Unmeasured kinds deliberately return zero.
static uint32_t proven_fixed_length(uint8_t k)
{
    if (k <= 0x09) return 20u + 8u * (uint32_t)k;
    switch (k) {
    case 0x15: return 36; case 0x18: return 28; case 0x19: return 36;
    case 0x1b: return 44; case 0x1c: return 44; case 0x1d: return 52;
    case 0x1e: return 20; case 0x20: return 20; case 0x21: return 20;
    case 0x22: return 20;
    case 0x24: return 24; case 0x25: return 24; case 0x26: return 16;
    case 0x27: return 12; case 0x2a: return 16;
    case 0x2b: return 4;  case 0x2c: return 4;  case 0x2d: return 4;
    case 0x2e: return 16; case 0x2f: return 20; case 0x30: return 28;
    case 0x31: return 28; case 0x32: return 24; case 0x34: return 24;
    case 0x35: return 24; case 0x36: return 24; case 0x38: return 40;
    case 0x39: return 36; case 0x3b: return 36; case 0x3d: return 36;
    case 0x3e: return 40;
    default: return 0;
    }
}

static bool record_target_ok(uint32_t target, size_t region_bytes)
{
    return (target & 3u) == 0 && target < region_bytes;
}

static void add_control_targets(const uint8_t* region, size_t region_bytes,
                                uint32_t off, std::set<uint32_t>& pending)
{
    if (!record_target_ok(off, region_bytes) || region_bytes - off < 4) return;
    const uint32_t h = u32(region + off);
    const uint8_t k = (uint8_t)(h & 0xffu);
    const uint32_t next = h >> 8;
    if (next > off && record_target_ok(next, region_bytes)) pending.insert(next);
    if (k == 0x26 && region_bytes - off >= 16) {
        const uint32_t target = u32(region + off + 12);
        if (record_target_ok(target, region_bytes)) pending.insert(target);
    } else if (k == 0x28 && region_bytes - off >= 16) {
        const uint32_t count = u32(region + off + 12);
        if (count == 0 || count >= 64) return;
        const size_t bytes = 16u + (size_t)count * 8u;
        if (bytes > region_bytes - off) return;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t target = u32(region + off + 16u +
                                        (size_t)count * 4u + (size_t)i * 4u);
            if (record_target_ok(target, region_bytes)) pending.insert(target);
        }
    }
}

static std::vector<uint32_t> discover_records(const uint8_t* region,
                                              size_t region_bytes,
                                              const std::vector<Fixup>& fixups)
{
    std::set<uint32_t> pending;
    std::set<uint32_t> seen;
    if (region_bytes >= 4) pending.insert(0);
    for (const Fixup& f : fixups) pending.insert(f.record_offset);
    while (!pending.empty()) {
        const uint32_t off = *pending.begin();
        pending.erase(pending.begin());
        if (!seen.insert(off).second) continue;
        add_control_targets(region, region_bytes, off, pending);
    }
    return std::vector<uint32_t>(seen.begin(), seen.end());
}

static bool mandatory_starts_ok(size_t off, size_t next,
                                const std::set<uint32_t>& mandatory)
{
    auto it = mandatory.upper_bound((uint32_t)off);
    return it == mandatory.end() || *it >= next;
}

// The exact tiler is intentionally conservative: it uses only lengths whose
// evidence is published as measured. It never guesses a length for a rare
// kind. A false result is therefore a named coverage gap, not parse failure.
static bool tile_records(const uint8_t* region, size_t limit,
                         const std::set<uint32_t>& mandatory,
                         std::vector<uint32_t>& starts)
{
    if (limit == 0) return true;
    std::vector<uint8_t> ways(limit + 1, 0);
    std::vector<uint32_t> choice(limit + 1, 0);
    ways[limit] = 1;
    for (size_t off = limit; off-- > 0;) {
        if ((off & 3u) || limit - off < 4) continue;
        const uint8_t kind = region[off];
        if (kind == 0x28) {
            if (limit - off < 16) continue;
            const uint32_t count = u32(region + off + 12);
            if (count == 0 || count >= 64) continue;
            const size_t len = 16u + (size_t)count * 8u;
            const size_t next = off + len;
            if (next <= limit && ways[next] && mandatory_starts_ok(off, next, mandatory)) {
                ways[off] = ways[next]; choice[off] = (uint32_t)len;
            }
        } else if (kind == 0x23) {
            // Observed range 24..232, exactly in 8-byte steps.
            for (uint32_t len = 24; len <= 232 && off + len <= limit; len += 8) {
                const size_t next = off + len;
                if (!ways[next] || !mandatory_starts_ok(off, next, mandatory)) continue;
                const unsigned sum = (unsigned)ways[off] + (unsigned)ways[next];
                ways[off] = (uint8_t)std::min(sum, 2u); // 2 means ambiguous
                if (choice[off] == 0) choice[off] = len;
            }
        } else {
            const uint32_t len = proven_fixed_length(kind);
            const size_t next = off + len;
            if (len && next <= limit && ways[next] && mandatory_starts_ok(off, next, mandatory)) {
                ways[off] = ways[next]; choice[off] = len;
            }
        }
    }
    if (ways[0] != 1) return false;
    size_t off = 0;
    while (off < limit) {
        const uint32_t len = choice[off];
        if (!len) return false;
        starts.push_back((uint32_t)off);
        off += len;
    }
    return off == limit;
}

static bool has_trailing_dword(uint8_t k)
{
    switch (k) {
    case 0x23: case 0x24: case 0x25: case 0x2a: case 0x2e:
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36:
    case 0x38: case 0x3e: return true;
    default: return false;
    }
}

static void materialize_records(const uint8_t* region, size_t region_bytes,
                                const std::vector<uint32_t>& starts,
                                const std::map<uint32_t, uint32_t>& key_by_offset,
                                std::vector<Record>& records,
                                size_t& covered)
{
    records.clear();
    covered = 0;
    for (size_t i = 0; i < starts.size(); ++i) {
        const size_t off = starts[i];
        const size_t end = i + 1 < starts.size() ? starts[i + 1] : region_bytes;
        if (end <= off || end > region_bytes || end - off < 4) continue;
        const uint32_t h = u32(region + off);
        Record r;
        r.offset = (uint32_t)off;
        r.byte_length = (uint32_t)(end - off);
        r.kind = (uint8_t)(h & 0xffu);
        r.next = h >> 8;
        r.has_operator = has_operator_pointer(r.kind);
        const auto key = key_by_offset.find(r.offset);
        if (key != key_by_offset.end()) r.operator_key = key->second;
        if (r.kind == 0x26 && end - off >= 16) {
            r.operands.push_back({u32(region + off + 4),
                                  u32(region + off + 8)});
            r.control_target = u32(region + off + 12);
            covered += end - off;
            records.push_back(std::move(r));
            continue;
        }
        if (r.kind == 0x28 && end - off >= 16) {
            r.operands.push_back({u32(region + off + 4),
                                  u32(region + off + 8)});
            const uint32_t count = u32(region + off + 12);
            if (count > 0 && count < 64 &&
                16u + (size_t)count * 8u <= end - off) {
                for (uint32_t j = 0; j < count; ++j)
                    r.dispatch_labels.push_back(u32(region + off + 16u +
                                                     (size_t)j * 4u));
                for (uint32_t j = 0; j < count; ++j)
                    r.dispatch_targets.push_back(u32(region + off + 16u +
                        (size_t)count * 4u + (size_t)j * 4u));
            }
            covered += end - off;
            records.push_back(std::move(r));
            continue;
        }
        size_t body = off + (r.has_operator ? 12u : 4u);
        size_t body_end = end;
        if (has_trailing_dword(r.kind) && body_end >= body + 4) {
            body_end -= 4;
            r.has_trailing_dword = true;
            r.trailing_dword = u32(region + body_end);
        }
        while (body + 8 <= body_end) {
            r.operands.push_back({u32(region + body), u32(region + body + 4)});
            body += 8;
        }
        covered += end - off;
        records.push_back(std::move(r));
    }
}

} // namespace

bool has_operator_pointer(uint8_t kind)
{
    return kind <= 0x1d || kind == 0x23;
}

uint32_t proven_record_length(uint8_t kind)
{
    return proven_fixed_length(kind);
}

bool parse(const uint8_t* data, size_t size, Graph& out, std::string& error)
{
    out = Graph{};
    error.clear();
    if (!data || size < 0x50) {
        error = "payload is smaller than the 0x50-byte graph header";
        return false;
    }
    for (size_t i = 0; i < 0x10; ++i) {
        if (data[i] != 0) {
            error = "leading relocation-pointer holes are not zero";
            return false;
        }
    }

    Header& h = out.header;
    h.content_hash = u32(data + 0x10);
    h.instance_header_size = u32(data + 0x20);
    h.constant_pool_size = u32(data + 0x24);
    h.slot_file_size = u32(data + 0x28);
    h.record_dwords = u32(data + 0x2c);
    h.pointer_table_entries = u32(data + 0x30);
    h.external_bindings = u32(data + 0x34);
    h.relocation_count = u16(data + 0x38);
    h.secondary_relocation_count = u16(data + 0x3a);
    h.fixup_count = u16(data + 0x3c);
    h.type_table_dwords = u16(data + 0x3e);
    h.instance_value_dwords = u16(data + 0x40);
    h.slot_value_dwords = u16(data + 0x42);
    h.instance_buffer_count = data[0x44];
    h.register_groups[0] = data[0x45];
    h.register_groups[1] = data[0x46];
    h.register_groups[2] = data[0x47];

    const size_t typed_dwords = (size_t)h.type_table_dwords +
                                h.instance_value_dwords + h.slot_value_dwords;
    size_t typed_bytes = 0;
    if (!mul_ok(typed_dwords, 4, typed_bytes)) {
        error = "typed-table size overflow"; return false;
    }
    size_t typed_span = 0;
    if (!add_ok(typed_bytes, 0x5f, typed_span)) {
        error = "typed-table span overflow"; return false;
    }
    typed_span &= ~size_t(0x0f);
    size_t rb0 = 0;
    if (!add_ok((size_t)h.constant_pool_size, 3, rb0) ||
        !add_ok(rb0, typed_span, rb0)) {
        error = "record-base overflow"; return false;
    }
    out.region_base = rb0 & ~size_t(3);
    if (out.region_base < h.constant_pool_size) {
        error = "constant pool precedes payload"; return false;
    }
    out.constant_base = out.region_base - h.constant_pool_size;

    size_t record_bytes = 0, relocation_count = 0, relocation_bytes = 0;
    if (!mul_ok((size_t)h.record_dwords, 4, record_bytes) ||
        !add_ok((size_t)h.relocation_count, h.secondary_relocation_count,
                relocation_count) ||
        !mul_ok(relocation_count, 8, relocation_bytes) ||
        !add_ok(out.region_base, record_bytes, out.relocation_table) ||
        !add_ok(out.relocation_table, relocation_bytes, out.fixup_table)) {
        error = "record/table offset overflow"; return false;
    }
    size_t fixup_bytes = 0, after_fixups = 0;
    if (!mul_ok((size_t)h.fixup_count, 8, fixup_bytes) ||
        !add_ok(out.fixup_table, fixup_bytes, after_fixups)) {
        error = "fixup-table offset overflow"; return false;
    }
    out.image_at = align16(after_fixups);
    size_t image_end = 0;
    if (!add_ok(out.image_at, h.instance_header_size, image_end) || image_end > size) {
        error = "instance image exceeds payload"; return false;
    }
    if (after_fixups > size || out.region_base > size ||
        out.constant_base > out.region_base) {
        error = "derived graph table lies outside payload"; return false;
    }

    const size_t type_begin = 0x50;
    size_t type_bytes = (size_t)h.type_table_dwords * 4;
    const size_t instance_begin = type_begin + type_bytes;
    const size_t instance_bytes = (size_t)h.instance_value_dwords * 4;
    const size_t slot_begin = instance_begin + instance_bytes;
    const size_t slot_bytes = (size_t)h.slot_value_dwords * 4;
    if (slot_begin + slot_bytes > out.constant_base) {
        error = "typed tables overlap the constant pool"; return false;
    }
    if (type_bytes % 20u) {
        error = "type table is not an exact sequence of 20-byte records";
        return false;
    }
    for (size_t at = type_begin; at < type_begin + type_bytes; at += 20) {
        TypeRecord t;
        t.type_id = u32(data + at);
        for (int i = 0; i < 4; ++i) t.carried[i] = u32(data + at + 4 + i * 4);
        out.types.push_back(t);
    }
    if (!parse_typed_table(data, instance_begin, instance_bytes,
                           h.instance_header_size, out.instance_values, error) ||
        !parse_typed_table(data, slot_begin, slot_bytes,
                           h.slot_file_size, out.slot_values, error)) return false;

    out.constant_pool.assign(data + out.constant_base, data + out.region_base);
    for (size_t i = 0; i < relocation_count; ++i) {
        const size_t at = out.relocation_table + i * 8;
        Relocation rel{u32(data + at), u32(data + at + 4)};
        if ((rel.pointer_field & 3u) || rel.pointer_field > h.constant_pool_size ||
            h.constant_pool_size - rel.pointer_field < 8 ||
            rel.target >= h.constant_pool_size) {
            error = "pool relocation points outside the constant pool";
            return false;
        }
        if (u64(data + out.constant_base + rel.pointer_field) != 0) {
            error = "pool relocation pointer field is already patched";
            return false;
        }
        out.relocations.push_back(rel);
    }

    std::map<uint32_t, uint32_t> key_by_offset;
    const uint8_t* region = data + out.region_base;
    for (uint32_t i = 0; i < h.fixup_count; ++i) {
        const size_t at = out.fixup_table + (size_t)i * 8;
        Fixup f{u32(data + at), u32(data + at + 4)};
        if (!record_target_ok(f.record_offset, record_bytes) ||
            record_bytes - f.record_offset < 12) {
            error = "operator fixup does not name a complete record";
            return false;
        }
        const uint8_t kind = region[f.record_offset];
        if (!has_operator_pointer(kind)) {
            error = "operator fixup targets a kind with no operator pointer";
            return false;
        }
        if (u64(region + f.record_offset + 4) != 0) {
            error = "operator pointer field is already patched";
            return false;
        }
        if (!key_by_offset.emplace(f.record_offset, f.key).second) {
            error = "two operator fixups target the same record";
            return false;
        }
        out.fixups.push_back(f);
    }

    std::set<uint32_t> mandatory;
    mandatory.insert(0);
    for (const Fixup& f : out.fixups) mandatory.insert(f.record_offset);
    // Control targets are constraints too. The discovery pass is complete
    // enough to supply them even when a rare kind prevents exact tiling.
    const std::vector<uint32_t> discovered =
        discover_records(region, record_bytes, out.fixups);
    mandatory.insert(discovered.begin(), discovered.end());

    std::vector<uint32_t> starts;
    out.exact_record_tiling = tile_records(region, record_bytes, mandatory, starts);
    if (!out.exact_record_tiling) starts = discovered;
    materialize_records(region, record_bytes, starts, key_by_offset,
                        out.records, out.discovered_record_bytes);

    out.instance_image.assign(data + out.image_at, data + image_end);
    return true;
}

}} // namespace bf6::expression
