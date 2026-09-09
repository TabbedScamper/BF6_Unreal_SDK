#include "types.h"
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "stdio_compat.h"

namespace bf6 {
namespace {

template <typename T>
T rd(const std::vector<uint8_t>& d, size_t off)
{
    T v{};
    std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

bool fits(const std::vector<uint8_t>& d, size_t off, size_t n)
{
    return off + n <= d.size();
}

bool all_zero(const TypeGuid& g)
{
    for (uint8_t b : g)
        if (b) return false;
    return true;
}

}  // namespace

/* The OOA licence lift lives in its own file; it needs Section and data_. */
#include "ooa_lift.inc"

std::vector<std::string> TypeDb::exe_candidates(const std::string& game_dir)
{
    // THE MULTIPLAYER EXECUTABLE FIRST, and the order is not cosmetic.
    //
    // The two builds ship DIFFERENT reflection schemas for the same classes.
    // WaterOceanSimulationEntityData is 21 fields and 368 bytes in the MP
    // build, and 19 fields and 352 bytes, in a different order, in SP. Reading
    // MP level data through the SP schema therefore returns wrong values from
    // the RIGHT bytes, which is the worst kind of wrong: every field parses,
    // nothing errors, and the numbers are quietly from neighbouring fields.
    //
    // Proved by instance stride: 384 on all 14 MP levels, 352 or 368 on SP
    // levels, with a clean double dissociation on how often each layout lands
    // on a field's default (26.6% vs 2.5% for MP levels, 5.0% vs 52.1% for SP).
    //
    // Portal levels are MP levels, so MP is the right default. SP remains a
    // fallback for an install that has no MP build.
    //
    // BF6_EXE OVERRIDES BOTH, and it exists because of a measured failure, not
    // as a convenience. The shipping build's `typeinfo`/`fieldinf` sections are
    // now ENCRYPTED - entropy 8.00 bits/byte against 3.44 for a build that
    // reads - and an encrypted table does NOT fail to open. It opens, reports a
    // section of the right size, and resolves every type to ZERO FIELDS, which
    // reaches a caller as an empty level rather than as an error. Measured on
    // the 2026-08-18 install: three types known to ship (BTSequenceNode,
    // BasicAffectorAsset, ActionMessageAsset) resolve 0/0/0 fields there and
    // 11/6/2 from a research copy of an earlier build.
    //
    // This still reads a real executable's real reflection tables. It is not a
    // staged schema file, and nothing here consumes an exported table.
    std::vector<std::string> out;
    if (const char* env = std::getenv("BF6_EXE"))
        if (*env) out.push_back(env);
    out.push_back(game_dir + "/bf6.exe");
    out.push_back(game_dir + "/SP/bf6.exe");
    return out;
}

std::string TypeDb::guid_str(const TypeGuid& g)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        (unsigned)(g[0] | (g[1] << 8) | (g[2] << 16) | ((unsigned)g[3] << 24)),
        (unsigned)(g[4] | (g[5] << 8)), (unsigned)(g[6] | (g[7] << 8)),
        g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return buf;
}

bool TypeDb::open(const std::string& exe_path, std::string& err)
{
    err.clear();
    FILE* f = fopen_binary_read(exe_path.c_str());
    if (!f) { err = "no exe at " + exe_path; return false; }

    // How big is it on disk, asked BEFORE the read rather than trusted after.
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0) { std::fclose(f); err = exe_path + " is empty"; return false; }
    file_size_ = (uint64_t)len;

    data_.resize((size_t)len);
    const size_t got = std::fread(data_.data(), 1, (size_t)len, f);
    std::fclose(f);
    if (got < (size_t)len)
    {
        // A short read is an error, not a smaller database. See the header.
        char msg[256];
        std::snprintf(msg, sizeof(msg),
            "only %zu of %ld bytes of the executable could be read, so the type "
            "schema is incomplete. This usually means the machine was short of memory.",
            got, len);
        err = msg;
        data_.clear();
        return false;
    }
    if (data_.size() < 0x40) { err = exe_path + " is too small to be a PE"; return false; }

    const uint32_t pe = rd<uint32_t>(data_, 0x3C);
    if (pe == 0 || !fits(data_, pe, 24)) { err = "no PE header"; return false; }
    const uint16_t nsec  = rd<uint16_t>(data_, pe + 6);
    const uint16_t optsz = rd<uint16_t>(data_, pe + 20);
    const size_t   opt   = pe + 24;
    if (!fits(data_, opt + 24, 8)) { err = "truncated optional header"; return false; }
    image_base_ = rd<uint64_t>(data_, opt + 24);

    const size_t so = opt + optsz;
    sections_.clear();
    for (uint16_t i = 0; i < nsec; i++)
    {
        const size_t o = so + (size_t)i * 40;
        if (!fits(data_, o, 40)) break;
        char nm[9] = {0};
        std::memcpy(nm, data_.data() + o, 8);
        Section s;
        s.name     = nm;
        s.vsize    = rd<uint32_t>(data_, o + 8);
        s.va       = rd<uint32_t>(data_, o + 12);
        s.raw_size = rd<uint32_t>(data_, o + 16);
        s.raw_off  = rd<uint32_t>(data_, o + 20);
        sections_.push_back(s);
    }

    ti_found_ = false;
    for (const Section& s : sections_)
        if (s.name == "typeinfo")
        {
            ti_found_ = true;
            ti_size_  = s.raw_size;
            ti_off_   = s.raw_off;
            ti_end_   = (size_t)s.raw_off + s.raw_size;
            break;
        }
    if (!ti_found_)
    {
        // Not fatal: the search falls back to the whole file, which is slower
        // but correct. The caller is told, because a silent 30x slowdown reads
        // as "the tool is slow" rather than "this exe is laid out differently".
        ti_off_  = 0;
        ti_end_  = data_.size();
        ti_size_ = 0;
    }
    if (ti_end_ > data_.size()) ti_end_ = data_.size();

    /* IF THE TABLES ARE CIPHERTEXT, TRY THE LIFT BEFORE HANDING BACK A DATABASE
     * THAT RESOLVES NOTHING. Doing it here means every reader in libbf6 gets it
     * without knowing it exists, which is the whole point - the alternative was
     * every caller needing BF6_EXE pointed at a decrypted copy. */
    lifted_ = 0;
    lift_note_.clear();
    if (looks_encrypted()) {
        const int n = ooa_lift(lift_note_);
        if (n > 0 && !looks_encrypted()) {
            lifted_ = n;
            lift_note_ = "lifted " + std::to_string(n) + " section(s) in memory via " + lift_note_;
        } else if (n > 0) {
            lift_note_ = "attempted the lift but the type table is still encrypted";
        }
    }
    return true;
}

// Virtual address -> file offset, or -1.
//
// A VA WITH THE HIGH BIT SET IS JUNK, and arrives here as an enormous unsigned
// value. A valid image is based around 0x140000000; these are uninitialised
// pointers in inherited field lists. They land in no section and so return -1,
// which is the same answer the GDScript reader reaches from the other direction
// (its signed int makes them enormously negative). Verified equal on 957 fields
// there. Anyone tightening this should keep that symmetry: clamping a bad VA to
// zero would make it look like a valid low address instead of an invalid one.
int64_t TypeDb::offset_of(uint64_t va) const
{
    if (va < image_base_) return -1;
    const uint64_t rva = va - image_base_;
    for (const Section& s : sections_)
    {
        const uint64_t span = std::max(s.vsize, s.raw_size);
        if (rva >= s.va && rva < (uint64_t)s.va + span)
        {
            const uint64_t fo = (uint64_t)s.raw_off + (rva - s.va);
            if (fo < data_.size()) return (int64_t)fo;
        }
    }
    return -1;
}

// The guid's own offset in the file. The typeinfo section first, then the whole
// file, because guid bytes can appear anywhere but a type that genuinely lives
// elsewhere is better found than missed. The fallback is counted: it costs
// about 30x the section search, and a reader quietly taking it every time would
// look like the search was not worth having.
int64_t TypeDb::find_guid(const TypeGuid& guid)
{
    n_find_++;
    auto scan = [&](size_t from, size_t to) -> int64_t
    {
        if (to <= from || to - from < 16) return -1;
        const uint8_t* base = data_.data();
        const uint8_t* p    = base + from;
        const uint8_t* end  = base + to - 15;
        while (p < end)
        {
            const uint8_t* hit = (const uint8_t*)std::memchr(p, guid[0], (size_t)(end - p));
            if (!hit) return -1;
            if (std::memcmp(hit, guid.data(), 16) == 0) return (int64_t)(hit - base);
            p = hit + 1;
        }
        return -1;
    };

    int64_t fo = scan(ti_off_, ti_end_);
    if (fo < 0)
    {
        n_fallback_++;
        fo = scan(0, data_.size());
    }
    return fo;
}

const TypeLayout& TypeDb::layout(const TypeGuid& guid)
{
    auto it = layout_cache_.find(guid);
    if (it != layout_cache_.end()) return it->second;

    TypeLayout lay;
    const int64_t fo = find_guid(guid);
    if (fo < 8 || !fits(data_, (size_t)fo, 56))
    {
        n_miss_++;
        return layout_cache_.emplace(guid, lay).first->second;
    }

    // Layout at the guid's file offset fo:
    //   nameHash u32 @ fo-8, flags u16 @ fo-4, size u16 @ fo-2, guid[16] @ fo,
    //   align u8 @ +32, fieldCount u16 @ +34, signature u32 @ +36,
    //   superClass u64 @ +40, pFieldInfos u64 @ +48 (class) or +88 (struct)
    const size_t o = (size_t)fo;
    lay.valid       = true;
    lay.guid        = guid;
    lay.name_hash   = rd<uint32_t>(data_, o - 8);
    lay.flags       = rd<uint16_t>(data_, o - 4);
    lay.size        = rd<uint16_t>(data_, o - 2);
    lay.align       = data_[o + 32];
    lay.type_enum   = (uint8_t)((lay.flags >> 5) & 0x1F);
    lay.field_count = rd<uint16_t>(data_, o + 34);
    lay.signature   = rd<uint32_t>(data_, o + 36);
    lay.super_va    = rd<uint64_t>(data_, o + 40);

    const size_t   pf_off   = (lay.type_enum == 3) ? 48 : 88;
    const uint64_t p_fields = fits(data_, o + pf_off, 8) ? rd<uint64_t>(data_, o + pf_off) : 0;
    const int64_t  fo_pf    = (p_fields > image_base_) ? offset_of(p_fields) : -1;
    if (fo_pf >= 0)
    {
        // FieldInfo, stride 24: nameHash u32 @ +0, flags u16 @ +4,
        //                       offset u32 @ +8, typeVA u64 @ +16
        for (uint16_t i = 0; i < lay.field_count; i++)
        {
            const size_t b = (size_t)fo_pf + (size_t)i * 24;
            if (!fits(data_, b, 24)) break;
            const uint32_t foff = rd<uint32_t>(data_, b + 8);
            // 0xFFFF is the "no serialized slot" sentinel, NOT an offset. These
            // fields exist on the runtime class but are never written to the
            // EBX payload, and reading one lands at base + 65535. On one street
            // light that is 22 of a type's 43 fields, and the first to land
            // past the end of the file killed the whole instance, taking the
            // intact prop's blueprint reference with it 395 times. The ones
            // that landed INSIDE the file were worse: they returned whatever
            // bytes happened to be there, silently.
            if (foff >= 0xFFFF) continue;
            FieldInfo fi;
            fi.name_hash  = rd<uint32_t>(data_, b);
            fi.flags      = rd<uint16_t>(data_, b + 4);
            fi.offset     = foff;
            fi.type_va    = rd<uint64_t>(data_, b + 16);
            fi.ftype_enum = (uint8_t)((fi.flags >> 5) & 0x1F);
            fi.fcategory  = (uint8_t)((fi.flags >> 1) & 0x0F);
            lay.fields.push_back(fi);
        }
    }
    return layout_cache_.emplace(guid, std::move(lay)).first->second;
}

const TypeLayout& TypeDb::layout_full(const TypeGuid& guid)
{
    auto it = full_cache_.find(guid);
    if (it != full_cache_.end()) return it->second;
    return layout_full_depth(guid, 0);
}

const TypeLayout& TypeDb::layout_full_depth(const TypeGuid& guid, int depth)
{
    if (depth == 0)
    {
        auto it = full_cache_.find(guid);
        if (it != full_cache_.end()) return it->second;
    }

    TypeLayout lay = layout(guid);   // a copy: inherited fields are appended
    if (!lay.valid || depth > kMaxSuperDepth)
        return full_cache_.emplace(guid, std::move(lay)).first->second;

    if (lay.super_va > image_base_)
    {
        const TypeGuid sg = guid_at_typeinfo(lay.super_va);
        if (!all_zero(sg) && sg != guid)
        {
            const TypeLayout& sup = layout_full_depth(sg, depth + 1);
            if (sup.valid)
            {
                // DEDUP BY nameHash, which is the field's identity. Keying on
                // offset was wrong twice over: it silently dropped a superclass
                // field sharing a slot with a subclass one, and because
                // unserialized fields all carried the same 0xFFFF sentinel it
                // collapsed every one of them into a single entry.
                std::vector<uint32_t> have;
                have.reserve(lay.fields.size());
                for (const FieldInfo& f : lay.fields) have.push_back(f.name_hash);
                for (const FieldInfo& f : sup.fields)
                    if (std::find(have.begin(), have.end(), f.name_hash) == have.end())
                    {
                        have.push_back(f.name_hash);
                        lay.fields.push_back(f);
                    }
            }
        }
    }
    return full_cache_.emplace(guid, std::move(lay)).first->second;
}

// va -> TypeInfo struct; its first u64 is a typeInfoDataOffset, guid at +8.
TypeGuid TypeDb::guid_at_typeinfo(uint64_t va) const
{
    TypeGuid g{};
    const int64_t o = offset_of(va);
    if (o < 0 || !fits(data_, (size_t)o, 8)) return g;
    const uint64_t tido = rd<uint64_t>(data_, (size_t)o);
    if (tido <= image_base_) return g;
    const int64_t od = offset_of(tido);
    if (od < 0 || !fits(data_, (size_t)od, 24)) return g;
    std::memcpy(g.data(), data_.data() + od + 8, 16);
    return g;
}

ResolvedType TypeDb::resolve(uint64_t type_va) const
{
    ResolvedType out;
    const int64_t o = offset_of(type_va);
    if (o < 0 || !fits(data_, (size_t)o, 8)) return out;
    const uint64_t tido = rd<uint64_t>(data_, (size_t)o);
    if (tido <= image_base_) return out;
    const int64_t od = offset_of(tido);
    if (od < 0 || !fits(data_, (size_t)od, 24)) return out;

    out.valid = true;
    out.flags = rd<uint16_t>(data_, (size_t)od + 4);
    std::memcpy(out.guid.data(), data_.data() + od + 8, 16);
    out.te  = (uint8_t)((out.flags >> 5) & 0x1F);
    out.cat = (uint8_t)((out.flags >> 1) & 0x0F);

    if (out.te == 0x04)
    {
        // Array: the element type pointer. Its offset varies for anonymous
        // arrays, so try the known slots and take the first that dereferences
        // to a plausible TypeInfoData.
        static const int kSlots[] = { 48, 40, 56, 32, 24 };
        for (int k : kSlots)
        {
            if (!fits(data_, (size_t)od + k, 8)) continue;
            const uint64_t cand = rd<uint64_t>(data_, (size_t)od + k);
            TypeGuid probe;
            if (cand > image_base_ && type_guid_only(cand, probe))
            {
                out.elem_va = cand;
                break;
            }
        }
    }
    return out;
}

// Does type_va dereference to a TypeInfoData with a sane type enum?
bool TypeDb::type_guid_only(uint64_t type_va, TypeGuid& out) const
{
    const int64_t o = offset_of(type_va);
    if (o < 0 || !fits(data_, (size_t)o, 8)) return false;
    const uint64_t tido = rd<uint64_t>(data_, (size_t)o);
    if (tido <= image_base_) return false;
    const int64_t od = offset_of(tido);
    if (od < 0 || !fits(data_, (size_t)od, 24)) return false;
    const uint8_t te = (uint8_t)((rd<uint16_t>(data_, (size_t)od + 4) >> 5) & 0x1F);
    // struct / class / cstring / enum / string / guid / resref
    if (te == 0x02 || te == 0x03 || te == 0x07 || te == 0x08 ||
        te == 0x06 || te == 0x15 || te == 0x17)
    {
        std::memcpy(out.data(), data_.data() + od + 8, 16);
        return true;
    }
    return false;
}

void TypeDb::entropy(double& out_bits, double& out_zero_pct, size_t sample) const
{
    out_bits = 0.0;
    out_zero_pct = 0.0;
    if (ti_end_ <= ti_off_) return;
    const size_t count = std::min(sample, ti_end_ - ti_off_);
    if (count == 0 || ti_off_ + count > data_.size()) return;

    uint64_t counts[256] = {0};
    for (size_t i = ti_off_; i < ti_off_ + count; i++) counts[data_[i]]++;

    double bits = 0.0;
    for (int i = 0; i < 256; i++)
        if (counts[i])
        {
            const double p = (double)counts[i] / (double)count;
            bits -= p * std::log2(p);
        }
    out_bits     = bits;
    out_zero_pct = 100.0 * (double)counts[0] / (double)count;
}

bool TypeDb::looks_encrypted() const
{
    double bits = 0.0, zeros = 0.0;
    entropy(bits, zeros);
    return bits > 7.0;
}

}  // namespace bf6
