#ifndef BF6_EXPRESSION_VM_H
#define BF6_EXPRESSION_VM_H

#include "expression_graph.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6 { namespace expression {

struct Value {
    std::vector<uint8_t> bytes;
    bool known = false;
    bool tainted = false;

    static Value from_u32(uint32_t value);
    static Value from_bool(bool value);
    uint32_t as_u32() const;
    bool as_bool() const;
};

struct OperatorSignature {
    std::vector<uint32_t> input_widths;
    uint32_t output_width = 0; // zero means a side-effect-only call
};

class Host {
public:
    virtual ~Host() = default;
    // Width and arity are part of the ABI. Refusing a description is safer
    // than reading every bare operand as a float-sized value.
    virtual bool describe(uint32_t key, OperatorSignature& out) = 0;
    virtual bool invoke(uint32_t key, const std::vector<Value>& args,
                        Value& out) = 0;
};

/* Pure named operators keyed by names recovered from the current executable.
 * Registering a name does not assert support: describe() returns false for
 * state-backed names such as tweakable reads. No copied key table is used.
 */
class NamedBuiltins final : public Host {
public:
    void add(uint32_t key, const std::string& current_exe_name);
    bool describe(uint32_t key, OperatorSignature& out) override;
    bool invoke(uint32_t key, const std::vector<Value>& args,
                Value& out) override;
private:
    std::map<uint32_t, std::string> names_;
};

struct Instance {
    const Graph* graph = nullptr;
    std::vector<uint8_t> image;
    std::map<uint32_t, Value> variables;
};

enum class Termination {
    Complete,
    Return,
    StepLimit,
    UntiledGraph,
    InvalidGraph
};

struct Evaluation {
    Termination termination = Termination::InvalidGraph;
    Value result;
    uint32_t steps = 0;
    uint32_t guessed_branches = 0;
    uint32_t approximated_indirect_jumps = 0;
    std::vector<uint32_t> unresolved_keys;
    std::vector<std::string> diagnostics;
};

bool make_instance(const Graph& graph, Instance& out, std::string& error);

/* Execute only a graph whose record region was tiled exactly. Operator calls
 * require a host-supplied exact signature; unknown operators/widths are
 * externalized in Evaluation and taint forwarded values. No neutral defaults
 * are fabricated by this core.
 */
Evaluation evaluate(const Graph& graph, Instance* instance,
                    const std::vector<Value>& arguments, Host* host);

}} // namespace bf6::expression

#endif
