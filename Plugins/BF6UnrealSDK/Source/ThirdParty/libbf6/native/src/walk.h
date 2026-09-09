/* libbf6 internal - the placement walk (module 9).
 *
 * Ported from bf6_walk.gd. This is what fills a LEVEL: it starts at the level
 * root partition and follows the object graph - subworld references, blueprint
 * references, StaticModelGroups - composing transforms on the way down, and
 * emits one row per placed thing.
 *
 * It is the last of the readers because it needs all of them: the mount to
 * reach a partition, module 5 to decode it, and module 4 to tell module 5 what
 * the fields are.
 *
 * The comments carried over from the GDScript record bugs that each cost real
 * placements on a real map. Every one of them produced a map that LOOKED fine,
 * which is why they are kept verbatim rather than summarised.
 */
#ifndef LIBBF6_WALK_H
#define LIBBF6_WALK_H

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "ebx.h"
#include "source.h"
#include "types.h"

namespace bf6 {

struct Vec3 { float x = 0, y = 0, z = 0; };

// Rows 0..2 are the basis (right, up, forward), row 3 the translation. The same
// shape the game's LinearTransform carries, and the same shape the plugin
// writes into its cache, so a row here compares to a row there directly.
struct Mat34 { Vec3 m[4]; };

Mat34 mat_identity();
Mat34 mat_mul(const Mat34& a, const Mat34& b);

struct WalkRow {
    std::string mesh;     // the asset this row places
    Mat34       xf;       // world transform
    std::string src;      // the partition the placement was found in
    std::string kind;     // smg | smg0 | leaf | ref
    std::string var;      // per-instance ObjectVariation, empty for none
    std::string scope;    // depot scope inherited down the walk
    std::string bundle;
    // THE PLACING BUNDLE: the bundle whose placement pulled this mesh in, which
    // is the bundle the row's `src` partition came in. This is what a material
    // lookup keys on - NOT the bundle the mesh resource lives in. Stamped here
    // because the walk is the only thing that knows it.
};

class Walk {
public:
    Walk(Source& src, TypeDb& types) : src_(src), types_(types) {}

    // Asset name -> ebx ref, plus the partition guid index import resolution
    // needs. Both come straight out of the mount, so nothing here depends on a
    // dump or a shipped table.
    void build_catalog(bool bounded_frontend = false);
    bool catalog_bounded_frontend() const { return bounded_frontend_; }

    // (placements found, instances seen) as the traversal runs. There is no
    // honest denominator - nothing knows how many instances a level has until
    // the walk ends - so total is the instances seen so far.
    using Progress = std::function<bool(const char*, int, int)>;
    void set_progress(Progress p) { progress_ = std::move(p); }

    // A playable-level import excludes GlacierFlow's non-playable front-end
    // subworlds. A caller explicitly walking the front-end scene opts in so
    // its Content subworld is traversed and placed. Default remains false.
    void set_include_frontend(bool v) { include_frontend_ = v; }

    // level_rel is the level's asset path, e.g. "game/glaciermp/levels/mp_dumbo",
    // or just the leaf. False when the root cannot be resolved.
    bool run(const std::string& level_rel, std::string& err);

    const std::vector<WalkRow>& rows() const { return rows_; }

    // Partition asset path (lower, no .ebx) -> an opaque scope id. Left empty,
    // every row's scope is "" and the walk behaves as it otherwise would. The
    // caller fills this with the bundles that own a depot.
    std::map<std::string, std::string> scope_index;

    // Counters. Each of these exists because its absence once made a broken
    // read look like an empty map.
    uint64_t n_instances = 0, n_skipped = 0, n_unresolved = 0;
    uint64_t n_partitions = 0, n_cycles = 0, n_missing = 0, n_parse_fail = 0;
    uint64_t n_smg = 0, n_smg_hidden = 0, n_smg_unresolved = 0;
    uint64_t n_leaf = 0, n_excluded = 0, n_destruction = 0;
    uint64_t n_subworld = 0, n_subworld_skipped = 0, n_subworld_unresolved = 0;
    std::string root;

private:
    void walk_ref(const std::string& ref, const Mat34& parent,
                  std::set<std::string> guard, int depth);
    void visit(const EbxValue& inst, const Mat34& parent, const std::string& ref,
               const std::set<std::string>& guard, int depth);
    void emit_smg(const EbxValue& inst, const Mat34& parent, const std::string& src_ref);
    bool type_matters(Ebx& e, size_t i);
    std::string resolve_name(const std::string& name) const;

    Source& src_;
    TypeDb& types_;

    std::unordered_map<std::string, std::string> by_name_;   // lower name -> "<name>.ebx"
    // Source owns and caches this immutable index for the mounted generation.
    // Borrow it: copying the 100k+ entry map for every small asset walk made
    // bounded front-end traversals spend seconds duplicating identical data.
    const std::map<std::string, std::string>*    gi_ = nullptr;
    std::vector<WalkRow>                         rows_;
    std::map<TypeGuid, bool>                     matters_;
    std::string                                  scope_;
    std::string                                  bundle_;
    Progress                                     progress_;
    bool                                         include_frontend_ = false;
    bool                                         bounded_frontend_ = false;
};

}  // namespace bf6

#endif
