/* placeables.h - the SDK's placeable object catalogue, filtered per level.
 *
 * The Portal Advanced Editor's object library is not something we reverse out of
 * the archives: the SDK SHIPS it as two JSON files under FbExportData/, and its
 * own scene-library plugin (generate_library.gd) builds the library from them.
 * We read the same two files and apply the same rule:
 *
 *   a placeable P is available on level L  <=>  P has no levelRestrictions
 *                                               (universal) OR L is in them.
 *
 * level_info.json  -> the level list (its top-level keys).
 * asset_types.json -> { "AssetTypes": [ {type, directory, constants[],
 *                       levelRestrictions[]}, ... ] }.
 */
#ifndef BF6_PLACEABLES_H
#define BF6_PLACEABLES_H

#include <string>
#include <vector>

namespace bf6 {

// One editable field the SDK exposes on a placeable (from its properties[]).
// e.g. {name:"CameraFOV", type:"float", def:"70"} or {name:"Team",
// type:"selection", def:""}. Types seen: bool, int, float, string, vector,
// selection (enum), PolygonVolume / SpawnPoint / Array[...] (object links).
struct PlaceProp {
    std::string name;
    std::string type;
    std::string def;        // default, stringified ("" if none/complex)
    std::string selections; // "selection" enum options, newline-joined ("" if none)
};

struct Placeable {
    std::string              type;         // "AAGun_01" - the placeable's name
    std::string              directory;    // "Generic/Common/Props" - UI category
    std::string              mesh;         // the 'mesh' constant, a resource stem
    int                      physics_cost = 0;
    bool                     universal = false;   // no level restrictions
    std::vector<std::string> levels;       // restriction list (empty == universal)
    std::vector<PlaceProp>   props;        // editable fields (properties[])
};

class PlaceableDB {
public:
    // Read <fbexport_dir>/level_info.json and asset_types.json. Returns false and
    // sets err if either is missing or malformed. Safe to call once per ctx.
    bool load(const std::string& fbexport_dir, std::string& err);

    const std::vector<std::string>& levels() const { return levels_; }
    const std::vector<Placeable>&   items()  const { return items_;  }
    bool loaded() const { return loaded_; }

    // Is `type` allowed on `level`? level "" -> everything counts as allowed.
    static bool allowed_on(const Placeable& p, const std::string& level);

private:
    std::vector<std::string> levels_;
    std::vector<Placeable>   items_;
    bool                     loaded_ = false;
};

}  // namespace bf6

#endif  // BF6_PLACEABLES_H
