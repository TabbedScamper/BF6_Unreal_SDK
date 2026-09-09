/* ebxdump_ext.inc - read one EBX partition THROUGH THE REFLECTION and hand
 * back a readable tree.
 *
 * A translation-unit fragment, like raw_ext.inc: it needs bf6_ctx and the
 * TypeDb the context owns, and neither exists outside bf6_core.cpp. Wire it
 * beside the others, before the closing `}  // extern "C"`.
 *
 * WHY IT EXISTS. bf6_read_raw hands back bytes, and reading a field out of
 * bytes means knowing a fixed offset. Fixed offsets are how you write a reader
 * that works today and breaks on the next patch WITHOUT SAYING SO: the
 * attachment cost sits at 0x80 in an attachment partition, but 0x80 in a slot
 * definition partition is the middle of the partition's own name string. The
 * same read produces a plausible integer from both.
 *
 * The schema is right there in the executable. Using it costs one call and
 * removes the entire class of error, so this exposes it rather than shipping
 * another offset table.
 *
 * This is a DIAGNOSTIC door: it formats to text, which is what a person
 * exploring a record wants. A consumer that needs values in a loop should get
 * a typed entry point instead of parsing this back.
 */

#include <cstdlib>

namespace {
/* A 64-entry cap silently truncates the arrays that matter most: a soldier
 * skeleton carries 291 bones per array, and a truncated dump is
 * indistinguishable from a short one. This knob lets a caller ask for all of
 * it; the default stays 64 so ordinary dumps stay readable. */
size_t bf6__dump_limit_value()
{
    static size_t n = []() -> size_t {
        std::string value;
#ifdef _MSC_VER
        char* raw = nullptr;
        size_t count = 0;
        if (_dupenv_s(&raw, &count, "BF6_DUMP_LIMIT") == 0 && raw)
        {
            value.assign(raw);
            std::free(raw);
        }
#else
        const char* raw = std::getenv("BF6_DUMP_LIMIT");
        if (raw) value.assign(raw);
#endif
        if (value.empty()) return 64;
        long v = std::strtol(value.c_str(), nullptr, 10);
        return v > 0 ? (size_t)v : (size_t)-1;   /* 0 or negative means no cap */
    }();
    return n;
}
#define bf6__dump_limit bf6__dump_limit_value()


void bf6__dump_val(const bf6::EbxValue& v, int depth, int max_depth,
                   std::string& out)
{
    char buf[256];
    const std::string pad((size_t)depth * 2 + 4, ' ');

    using K = bf6::EbxValue::Kind;
    switch (v.kind)
    {
    case K::Bool:  out += v.b ? "true" : "false"; break;
    case K::Int:   std::snprintf(buf, sizeof(buf), "%lld", (long long)v.i); out += buf; break;
    case K::Uint:  std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v.u); out += buf; break;
    case K::Real:  std::snprintf(buf, sizeof(buf), "%.6g", v.f); out += buf; break;
    case K::Str:   out += "\"" + v.s + "\""; break;
    case K::Guid:  out += "guid"; break;
    case K::ResRef:
        std::snprintf(buf, sizeof(buf), "resref %016llx", (unsigned long long)v.u);
        out += buf; break;
    case K::InstanceRef:
        std::snprintf(buf, sizeof(buf), "-> instance %d", (int)v.instance);
        out += buf; break;
    case K::ImportRef:
        // The resolved path is the useful half: it is how one record names
        // another, and following it by hand is the whole job of a walk.
        out += "import " + (v.import_path.empty() ? v.s : v.import_path);
        break;
    case K::Array:
        std::snprintf(buf, sizeof(buf), "[%d]", (int)v.items.size());
        out += buf;
        if (depth >= max_depth) break;
        for (size_t i = 0; i < v.items.size() && i < bf6__dump_limit; i++)
        {
            out += "\n" + pad;
            std::snprintf(buf, sizeof(buf), "%2d: ", (int)i);
            out += buf;
            bf6__dump_val(v.items[i], depth + 1, max_depth, out);
        }
        if (v.items.size() > bf6__dump_limit) out += "\n" + pad + "...";
        break;
    case K::Struct:
        std::snprintf(buf, sizeof(buf), "{%d}", (int)v.fields.size());
        out += buf;
        if (depth >= max_depth) break;
        for (const auto& kv : v.fields)
        {
            out += "\n" + pad;
            {
                /* Name first when we have it: a reader should see
                 * "Identifier" and not have to carry 0xDE6F63B3 in their head. */
                const char* fn = bf6_field_name(kv.first);
                if (fn) std::snprintf(buf, sizeof(buf), "%-28s ", fn);
                else    std::snprintf(buf, sizeof(buf), "0x%08x %-17s ", kv.first, "");
                out += buf;
            }
            bf6__dump_val(kv.second, depth + 1, max_depth, out);
        }
        break;
    case K::Unknown:
        std::snprintf(buf, sizeof(buf), "unknown(te=%u raw=%u)", v.te, (unsigned)v.u);
        out += buf; break;
    default: out += "null"; break;
    }
}

}  // namespace


/* ------------------------------------------------------- field-name boundary
 *
 * BF6 strips type and field NAMES from its reflection and keys everything by
 * an opaque u32. It also CHANGED THE HASH FUNCTION, so the names cannot be
 * recovered by hashing candidates: djb2, fnv1, fnv1a, crc32, murmur, xxhash
 * and city all fail over every basis, prime, case and encoding.
 *
 * The research corpus recovers names by joining stable Frostbite type GUIDs
 * and field offsets against named older-title dumps. That dictionary is an
 * evidence oracle, not data the runtime core may silently consume. Functional
 * readers carry the proven hashes they need; generic dumps expose raw hashes.
 */
const char* bf6_field_name(uint32_t name_hash)
{
    (void)name_hash;
    /* The installed reflection carries hashes, not names. The former lookup
     * loaded a research-derived JSON file beside the executable, making core
     * behavior depend on a staged oracle. Keep this ABI for compatibility and
     * report the live truth. fieldnames.py remains the evidence-side join. */
    return nullptr;
}

int64_t bf6_ebx_dump(bf6_ctx* c, const char* name, int max_depth,
                     char* out, int out_len)
{
    if (!c || !name || !out || out_len <= 0) return -1;

    /* Load the schema on demand. bf6_open does not build one - the level
     * readers do - so a caller that only wants to read a record would
     * otherwise be told the schema is missing when it is merely unbuilt.
     * ensure_types already exists for exactly this and reports the EA App
     * encrypted case properly rather than resolving everything to null. */
    std::string terr;
    if (!ensure_types(c, terr))
    {
        std::snprintf(out, (size_t)out_len, "no type schema: %s", terr.c_str());
        return -1;
    }

    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(name) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(name, err);
    if (raw.empty())
    {
        std::snprintf(out, (size_t)out_len, "not in the mount: %s", name);
        return -1;
    }

    bf6::Ebx e(*c->types);
    /* WITHOUT THIS EVERY CROSS-PARTITION REFERENCE PRINTS AS "<not indexed>".
     *
     * The dump is the tool anyone reaches for when working out how one record
     * finds another, and an unresolved import is exactly the thing that cannot
     * answer that. Building the index costs a scan, but it is cached on the
     * context, and a dump that hides the references is worth less than the
     * seconds it saves. */
    /* THE FULL INDEX, not the armory one.
     *
     * The armory index is bounded to authored hardware, gameplay and UI, which
     * is the right trade for the armory readers. It is the wrong trade here: a
     * character partition's imports are outside that name space, so every
     * reference in a CharacterDefinition printed "<not indexed>" - which reads
     * as "this record has no references" rather than as "this tool cannot see
     * them", and that is exactly the question a dump is opened to answer.
     * The full index costs one pass over every partition's EFIX header, cached
     * on the Source, against a dump that would otherwise hide what it was
     * asked to show. */
    e.set_guid_index(&c->src.partition_index());
    if (!e.parse(std::move(raw), err))
    {
        std::snprintf(out, (size_t)out_len, "parse failed: %s", err.c_str());
        return -1;
    }

    std::string s;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s\n  partition %s\n  instances %d (exported %d)\n",
                  name, e.partition_guid().c_str(),
                  (int)e.instance_count(), (int)e.exported_count());
    s += buf;

    for (size_t i = 0; i < e.instance_count(); i++)
    {
        /* Retail reflection strips type NAMES, so a type is only ever its
         * guid here. Printing the guid is not a fallback, it is the identity. */
        const bf6::TypeGuid tg = e.instance_type(i);
        const std::string tn = bf6::TypeDb::guid_str(tg);
        std::snprintf(buf, sizeof(buf), "\n  [%d] type %s\n    ", (int)i, tn.c_str());
        s += buf;
        bf6::EbxValue v = e.read_instance(i);
        bf6__dump_val(v, 0, max_depth <= 0 ? 3 : max_depth, s);
        s += "\n";
    }

    const int64_t need = (int64_t)s.size() + 1;
    std::snprintf(out, (size_t)out_len, "%s", s.c_str());
    return need;
}
