#ifndef BF6_EXPRESSION_REGISTRY_H
#define BF6_EXPRESSION_REGISTRY_H

#include <cstdint>
#include <string>
#include <vector>

namespace bf6 { namespace expression {

struct DescriptorOperator {
    uint32_t key = 0;
    uint32_t flags = 0;
    uint64_t implementation_va = 0;
    uint64_t record_va = 0;
};

struct MethodOperator {
    uint32_t key = 0;
    uint32_t flags = 0;
    uint64_t implementation_va = 0;
    uint64_t record_va = 0;
};

/* Scan the executable's 32-byte self-referential descriptor records:
 *   impl qword; key u32; flags u32; self qword; zero qword.
 * The implementation must point into an executable PE section. This is the
 * engine's current registry source, not a copied key table.
 */
bool read_descriptor_operators(const std::string& exe_path,
                               std::vector<DescriptorOperator>& out,
                               std::string& error);

/* Find compact EA::EX::MethodRegistry records used by the supplied raw-graph
 * operator keys. Its records are
 *   impl qword; key u32; flags u32
 * in writable, non-executable PE data. Querying by independently read graph
 * keys is required: the executable contains unrelated tables with the same
 * 16-byte shape, so a global longest-run heuristic is not sound.
 */
bool read_method_operators(const std::string& exe_path,
                           const std::vector<uint32_t>& query_keys,
                           std::vector<MethodOperator>& out,
                           std::string& error);

struct NamedOperator {
    uint32_t key = 0;
    uint32_t match_count = 0;
    std::string name; // populated only when the match is unique
};

struct ReflectedOperator {
    uint32_t key = 0;
    uint16_t parameter_count = 0;
    uint32_t signature = 0;
    uint64_t descriptor_va = 0;
    uint64_t parameters_va = 0;
};

bool read_reflected_operators(const std::string& exe_path,
                              std::vector<ReflectedOperator>& out,
                              std::string& error);

uint32_t operator_name_crc32(const uint8_t* data, size_t size);

/* Resolve only the requested keys against NUL-terminated printable literals
 * in the current executable. Ambiguous CRC matches are counted and withheld.
 */
bool resolve_named_operators(const std::string& exe_path,
                             const std::vector<uint32_t>& keys,
                             std::vector<NamedOperator>& out,
                             std::string& error);

}} // namespace bf6::expression

#endif
