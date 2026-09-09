#include "armory.h"

#include <algorithm>
#include <cstring>

namespace bf6 {

namespace {

// The classes the hardware tree actually uses as folder names. Kept as a list
// rather than "whatever sits under weapons/" so a stray folder cannot invent a
// weapon class, and so the order of the output is stable between runs.
const char* kClasses[] = {
    "assaultrifle", "carbine", "dmr", "boltaction", "mg", "smg",
    "secondary", "shotgun", "melee", "vehicle", "battlepickup",
};

struct SlotLabel { const char* code; const char* label; };
const SlotLabel kSlots[] = {
    { "scp", "Scope" },      { "sca", "SecondarySight" }, { "brl", "Barrel" },
    { "mzl", "Muzzle" },     { "mag", "Magazine" },       { "amo", "Ammo" },
    { "erg", "Ergonomic" },  { "btm", "Bottom" },         { "top", "Top" },
    { "lft", "Left" },       { "rgt", "Right" },
};

std::string lower(std::string s)
{
    for (char& c : s) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return s;
}

// The last path component, without any extension.
std::string stem(const std::string& path)
{
    size_t a = path.find_last_of("/\\");
    a = (a == std::string::npos) ? 0 : a + 1;
    size_t b = path.find_last_of('.');
    if (b == std::string::npos || b < a) b = path.size();
    return path.substr(a, b - a);
}

std::string dir_of(const std::string& path)
{
    const size_t a = path.find_last_of("/\\");
    return a == std::string::npos ? std::string() : path.substr(0, a);
}

// "common/hardware/weapons/carbine/m4a1/..." -> ("carbine", "m4a1").
// Returns false for anything that is not under a known class folder, which is
// what keeps shared and _attachments trees out of the weapon roster.
bool class_and_weapon(const std::string& path, std::string& cls, std::string& wep)
{
    static const std::string kMark = "/weapons/";
    const size_t at = path.find(kMark);
    if (at == std::string::npos) return false;
    std::string rest = path.substr(at + kMark.size());

    size_t s1 = rest.find('/');
    if (s1 == std::string::npos) return false;
    cls = rest.substr(0, s1);

    bool known = false;
    for (const char* c : kClasses) if (cls == c) { known = true; break; }
    if (!known) return false;

    rest = rest.substr(s1 + 1);
    const size_t s2 = rest.find('/');
    wep = (s2 == std::string::npos) ? rest : rest.substr(0, s2);
    return !wep.empty();
}

// Split "<weapon>_<slot>_<attachment>" using the SLOT as the anchor.
//
// Splitting on underscores positionally does not work: both weapon tokens and
// attachment tokens contain underscores in the wild. The slot code is a fixed
// three-letter vocabulary, so find it as a whole token and cut there. The
// attachment name keeps every underscore to its right, which is what the
// filenames intend.
bool split_row(const std::string& body, const std::string& weapon,
               std::string& slot, std::string& att)
{
    auto known = [](const std::string& code) -> bool
    {
        for (const SlotLabel& s : kSlots) if (code == s.code) return true;
        return false;
    };

    // THE FORMAT IS POSITIONAL: <weapon>_<slot>_<attachment>. Read the slot
    // where it actually sits, immediately after the weapon prefix.
    //
    // Searching for a slot token instead is what a first cut does, and it is
    // wrong: attachment names contain slot codes too. "improved_mag_catch" in
    // the ERG slot carries "mag_", so a token search that reached "mag" first
    // filed 21 rows under Magazine as an attachment literally named "catch".
    // Anchoring removes the ordering dependency entirely.
    if (body.size() > weapon.size() + 5 &&
        body.compare(0, weapon.size(), weapon) == 0 && body[weapon.size()] == '_')
    {
        const size_t p = weapon.size() + 1;
        const std::string code = body.substr(p, 3);
        if (body.size() > p + 3 && body[p + 3] == '_' && known(code))
        {
            slot = code;
            att  = body.substr(p + 4);
            return true;
        }
    }

    // The file prefix does not match its folder. Fall back to the LEFTMOST
    // slot token at a boundary - leftmost, not first-in-our-table, for the
    // same reason as above.
    size_t best = std::string::npos;
    for (const SlotLabel& s : kSlots)
    {
        const std::string tok = std::string(s.code) + "_";
        size_t at = body.find(tok);
        while (at != std::string::npos)
        {
            const bool boundary = (at == 0) || body[at - 1] == '_';
            if (boundary && at + tok.size() < body.size() && at < best)
            {
                best = at; slot = s.code; att = body.substr(at + tok.size());
                break;
            }
            at = body.find(tok, at + 1);
        }
    }
    return best != std::string::npos;
}

}  // namespace

const char* armory_slot_label(const std::string& code)
{
    for (const SlotLabel& s : kSlots) if (code == s.code) return s.label;
    return "";
}

// Everything under hardware/gadgets that has art, as roster entries.
//
// THESE HAVE NO ATTACHMENT ROWS, which is exactly why they were invisible: the
// roster is built from attachment_/u_prg_ partitions and a grenade has
// neither. They are still 116 real, selectable items - throwables, launchers,
// mines, battle pickups - and leaving them out of a weapon browser because
// they cannot be customised is the wrong call.
static void gadgets_from_names(const std::vector<std::string>& names, Armory& out)
{
    static const std::string kMark = "/hardware/gadgets/";
    std::map<std::string, ArmoryWeapon> found;
    for (const std::string& raw : names)
    {
        const std::string path = lower(raw);
        const size_t at = path.find(kMark);
        if (at == std::string::npos) continue;
        std::string rest = path.substr(at + kMark.size());

        size_t s1 = rest.find('/');
        if (s1 == std::string::npos) continue;
        const std::string cat = rest.substr(0, s1);
        // Shared folders are not items.
        if (cat.empty() || cat[0] == '_') continue;
        rest = rest.substr(s1 + 1);
        const size_t s2 = rest.find('/');
        if (s2 == std::string::npos) continue;          // a loose file, not an item
        const std::string name = rest.substr(0, s2);
        if (name.empty() || name[0] == '_') continue;

        const std::string key = cat + "/" + name;
        ArmoryWeapon& g = found[key];
        if (g.name.empty()) { g.cls = cat; g.name = name; g.dir = dir_of(raw); }
    }
    for (auto& kv : found) out.weapons.push_back(std::move(kv.second));
}

// Melee equipment follows the same one-folder-per-item organization as
// gadgets, but lives below hardware/weapons and has no attachment/progression
// rows.  Keep it in the selectable install roster instead of silently
// dropping the entire equipment-slot family.
static void melee_from_names(const std::vector<std::string>& names, Armory& out)
{
    static const std::string kMark = "/hardware/weapons/melee/";
    std::map<std::string, ArmoryWeapon> found;
    for (const std::string& raw : names)
    {
        const std::string path = lower(raw);
        const size_t at = path.find(kMark);
        if (at == std::string::npos) continue;
        const size_t itemAt = at + kMark.size();
        const size_t itemEnd = path.find('/', itemAt);
        if (itemEnd == std::string::npos) continue;
        const std::string item = path.substr(itemAt, itemEnd - itemAt);
        if (item.empty() || item[0] == '_') continue;

        ArmoryWeapon& weapon = found[item];
        if (weapon.name.empty())
        {
            weapon.cls = "melee";
            weapon.name = item;
            weapon.dir = path.substr(0, itemEnd);
        }
    }
    for (auto& kv : found) out.weapons.push_back(std::move(kv.second));
}

Armory armory_from_names(const std::vector<std::string>& names)
{
    // key: class/weapon -> weapon, and within it slot|att -> row.
    std::map<std::string, ArmoryWeapon> byWeapon;
    std::map<std::string, std::map<std::string, ArmoryAttachment>> byRow;

    Armory out;
    for (const std::string& raw : names)
    {
        const std::string path = lower(raw);
        const std::string st   = stem(path);

        bool is_att = st.compare(0, 11, "attachment_") == 0;
        bool is_prg = st.compare(0, 6,  "u_prg_") == 0;
        if (!is_att && !is_prg) continue;

        std::string cls, wep;
        if (!class_and_weapon(path, cls, wep)) continue;

        const std::string body = is_att ? st.substr(11) : st.substr(6);
        std::string slot, att;
        if (!split_row(body, wep, slot, att)) continue;

        const std::string wkey = cls + "/" + wep;
        ArmoryWeapon& W = byWeapon[wkey];
        if (W.name.empty()) { W.cls = cls; W.name = wep; W.dir = dir_of(raw); }

        ArmoryAttachment& A = byRow[wkey][slot + "|" + att];
        A.slot = slot;
        A.name = att;
        if (is_att) A.ebx = raw; else A.prg = raw;

        out.slot_codes.insert(slot);
        out.rows++;
    }

    out.weapons.reserve(byWeapon.size());
    for (auto& kv : byWeapon)
    {
        ArmoryWeapon W = std::move(kv.second);
        auto it = byRow.find(kv.first);
        if (it != byRow.end())
            for (auto& rk : it->second)
            {
                if (!rk.second.ebx.empty()) W.slots[rk.second.slot].push_back(rk.second.name);
                if (!rk.second.prg.empty()) W.prg[rk.second.slot].push_back(rk.second.name);
                W.attachments.push_back(std::move(rk.second));
            }
        for (auto& sk : W.slots) std::sort(sk.second.begin(), sk.second.end());
        for (auto& sk : W.prg)   std::sort(sk.second.begin(), sk.second.end());
        std::sort(W.attachments.begin(), W.attachments.end(),
                  [](const ArmoryAttachment& a, const ArmoryAttachment& b)
                  { return a.slot != b.slot ? a.slot < b.slot : a.name < b.name; });
        out.weapons.push_back(std::move(W));
    }

    gadgets_from_names(names, out);
    melee_from_names(names, out);
    // One stable order for the whole roster, weapons and gadgets together.
    std::sort(out.weapons.begin(), out.weapons.end(),
              [](const ArmoryWeapon& a, const ArmoryWeapon& b)
              { return a.cls != b.cls ? a.cls < b.cls : a.name < b.name; });
    return out;
}

}  // namespace bf6
