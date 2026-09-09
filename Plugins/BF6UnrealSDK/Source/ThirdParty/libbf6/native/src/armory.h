// The armory: weapons, their slots, what fits each slot, and what it costs.
//
// THE COMPATIBILITY TABLE IS NOT BINARY. The game ships one EBX partition per
// valid (weapon, slot, attachment) combination, named for the combination:
//
//     attachment_<weapon>_<slot>_<attachment>    art and logic binding
//     u_prg_<weapon>_<slot>_<attachment>         progression / unlock row
//
// so enumerating the mount's name table IS reading the table. No EBX decode is
// needed for the roster, which is why this is cheap enough to do at startup
// against a live install rather than staging a tree first.
//
// Cost is the one field that needs the payload, and even that does not need
// the schema: it is Int32 at absolute file offset 0x80 in the attachment
// partition, verified against the reflected read on 5,502 of 5,502 rows.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bf6 {

struct ArmoryAttachment {
    std::string slot;        // three-letter code as shipped: scp, mzl, mag...
    std::string name;        // the attachment's own token
    std::string ebx;         // the attachment_ partition, "" when only u_prg_ exists
    std::string prg;         // the u_prg_ partition, "" when absent
    int32_t     cost = -1;   // points; -1 = not read, 0 = free and authored so
};

struct ArmoryWeapon {
    std::string cls;         // carbine, assaultrifle, smg, ...
    std::string name;        // m4a1
    std::string dir;         // the bundle folder the rows came from
    std::vector<ArmoryAttachment> attachments;
    // TWO TABLES, NOT ONE. attachment_ rows are the art and logic binding;
    // u_prg_ rows are the progression and unlock offer. They do NOT agree -
    // 1,270 combinations ship an unlock row with no art binding - so merging
    // them silently inflates every weapon's fitted list. The reference reader
    // keeps them apart and so does this.
    std::map<std::string, std::vector<std::string>> slots;  // from attachment_
    std::map<std::string, std::vector<std::string>> prg;    // from u_prg_
};

// Slot code -> the name a person recognises. Shipped codes, not invented ones.
const char* armory_slot_label(const std::string& code);

struct Armory {
    std::vector<ArmoryWeapon> weapons;
    std::set<std::string>     slot_codes;   // every code actually seen
    int64_t                   rows = 0;     // attachment_ + u_prg_ partitions matched
};

// Build the table from an already-mounted context's EBX name list.
//
// `names` is every EBX partition name in the mount; the caller gets that from
// bf6_list_ebx. Passing it in rather than calling out keeps this testable
// against a fixture list with no install present.
Armory armory_from_names(const std::vector<std::string>& names);

}  // namespace bf6
