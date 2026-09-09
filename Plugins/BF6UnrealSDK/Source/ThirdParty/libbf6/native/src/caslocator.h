/* libbf6 internal - CasFileId -> path on disk (module 2, mount).
 *
 * Ported from bf6_container.gd's CasLocator. Reads Data/layout.toc, indexes the
 * install chunks by persistentIndex, and joins an (install_chunk_id, cas_index)
 * to the cas_NN.cas file under Data/ or an Update package.
 */
#ifndef LIBBF6_CASLOCATOR_H
#define LIBBF6_CASLOCATOR_H

#include <cstdint>
#include <map>
#include <string>

#include "dbobject.h"

namespace bf6 {

class CasLocator {
public:
    bool open(const std::string& game_dir, std::string& err);
    // "" if the chunk id is unknown or no matching cas file exists on disk.
    std::string cas_path(uint32_t install_chunk_id, int cas_index) const;

private:
    std::string game_;
    std::map<uint32_t, DbValue> by_index_;   // persistentIndex (u32) -> installChunk
};

}  // namespace bf6
#endif
