/* libbf6 internal - exact local VisualEnvironment trigger geometry.
 *
 * A local VE carries no bounds.  Its AreaProximityEntityData is connected to
 * OBBData or VolumeVectorShapeData through the owning blueprint's
 * LinkConnections array.  This reader walks the installed level graph, keeps
 * the prefab placement transform, follows the connection graph to a local VE
 * reference, and emits only shapes for which that complete join exists.
 */
#ifndef LIBBF6_LIGHTINGZONES_H
#define LIBBF6_LIGHTINGZONES_H

#include <cstdint>
#include <string>
#include <vector>

#include "source.h"
#include "types.h"
#include "walk.h"

namespace bf6 {

enum LightingZoneKind { kZoneObb = 0, kZonePolygon = 1 };

struct LightingZone {
    int kind = kZoneObb;
    Mat34 xf = mat_identity();       // world placement; OBB includes its local transform
    Vec3 half_extents{};             // OBB only
    std::vector<Vec3> points;        // polygon only, local to xf
    float height = 0.f;              // polygon extrusion along local +Y
    float fade_distance = 0.f;       // AreaProximityEntityData.ProximityDistance
    std::string preset;              // exact imported VE partition
    std::string source;              // owning blueprint partition
    int proximity_instance = -1;
    int shape_instance = -1;
};

struct LightingZoneStats {
    uint64_t partitions = 0, instances = 0, parse_fail = 0, missing = 0, cycles = 0;
    uint64_t proximity = 0, link_edges = 0, non_geometry_links = 0;
    uint64_t non_shape_geometry_links = 0;
    uint64_t target_obb = 0, target_polygon = 0, target_other = 0;
    uint64_t joined_preset = 0, omitted_no_preset = 0;
    uint64_t malformed_shape = 0, unresolved_types = 0;
    std::vector<std::string> target_other_examples;
    /* Negative control: rotate each real source instance by one before the
     * source->shape join.  This must not reproduce real AreaProximity links. */
    uint64_t rotated_control_hits = 0;
};

bool level_lighting_zones(Source& src, TypeDb& types, const std::string& level,
                          std::vector<LightingZone>& out,
                          LightingZoneStats& stats, std::string& err);

} // namespace bf6
#endif
