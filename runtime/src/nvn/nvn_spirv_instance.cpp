// Draw merging (nvn_draw.cpp, docs/PARALLEL_REPLAY.md): on the island ~95% of the fast path's draws repeat the
// previous draw's mesh and state, and differ only in one uniform block the vertex shader reads (an object's transform),
// uploaded at a constant step in the ring. Such a run becomes one instanced draw: the block is bound once over the
// whole run and the vertex module reads instance k's copy, k * step bytes further.
//
// patch_instance_blocks() rewrites the vertex module so that every read of set 0 `bindings[j]` (the translator's blocks
// are struct { vec4 data[4096]; }, read through access chains base, 0, index) indexes data[index + gl_InstanceIndex *
// steps[j]] (in vec4s). Refused (false) when the module already reads gl_InstanceIndex (its value would change) or uses
// a block other than through such chains.
#include <cstring>
#include <map>
#include <set>

#include "nvn_pipeline.h"

namespace nx::nvn {

namespace {

enum : uint32_t {
    OpEntryPoint = 15, OpTypeInt = 21, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpVariable = 59,
    OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpIAdd = 128,
    OpIMul = 132, OpBitcast = 124, OpLabel = 248, DecorationBuiltIn = 11, DecorationBinding = 33,
    DecorationDescriptorSet = 34, BuiltInInstanceIndex = 43, StorageInput = 1, StorageUniform = 2,
};

bool header_op(uint32_t op) {  // capabilities .. annotations: before the types
    switch (op) {
        case 17: case 10: case 11: case 14: case 15: case 16: case 330: case 3: case 4: case 5: case 6: case 7: case 8:
        case 71: case 72: case 73: case 74: case 75: return true;
        default: return false;
    }
}

}  // namespace

bool patch_instance_block(const std::vector<uint32_t>& in, uint32_t binding, uint32_t step_vec4, std::vector<uint32_t>& out) {
    return patch_instance_blocks(in, &binding, &step_vec4, 1, out);
}

bool patch_instance_blocks(const std::vector<uint32_t>& in, const uint32_t* bindings, const uint32_t* steps, uint32_t nb,
                           std::vector<uint32_t>& out) {
    if (in.size() < 5 || !nb) return false;
    for (uint32_t j = 0; j < nb; j++)
        if (!steps[j]) return false;
    struct Inst {
        size_t at;
        uint32_t op, len;
    };
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3], i32 = 0;
    std::map<uint32_t, uint32_t> binding_of, set_of, type_of;  // id -> Binding, DescriptorSet; value -> its type
    std::set<uint32_t> ints;                                      // 32-bit integer types
    std::map<uint32_t, uint32_t> vars;                            // the blocks' variables -> their step
    std::set<uint32_t> uniform_vars;
    size_t first_function = in.size(), first_type = in.size();
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (first_type == in.size() && !header_op(n.op)) first_type = n.at;
        if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBuiltIn && w[3] == BuiltInInstanceIndex) {
            static int logged = 0;
            if (logged++ < 3) log("instance patch: the module reads gl_InstanceIndex");
            return false;
        }
        if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBinding) binding_of[w[1]] = w[3];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationDescriptorSet) set_of[w[1]] = w[3];
        else if (n.op == OpTypeInt && n.len == 4 && w[2] == 32) {
            ints.insert(w[1]);
            if (w[3] == 1) i32 = w[1];
        } else if (n.op == OpVariable && n.len >= 4 && w[3] == StorageUniform) uniform_vars.insert(w[2]);
        else if (n.op == OpConstant && n.len >= 4) type_of[w[2]] = w[1];
        else if (n.op == OpFunction && first_function == in.size()) first_function = n.at;
        if (n.at > first_function && n.len >= 3 && n.op != OpStore && n.op != OpDecorate && n.op != OpLabel &&
            n.op != OpFunction && (n.op < 246 || n.op > 256))
            type_of.emplace(w[2], w[1]);
    }
    for (uint32_t j = 0; j < nb; j++) {
        bool found = false;
        for (uint32_t v : uniform_vars)
            if (binding_of.count(v) && binding_of[v] == bindings[j] && set_of[v] == 0) vars[v] = steps[j], found = true;
        if (!found || first_function == in.size() || first_type == in.size()) {
            static int logged = 0;
            if (logged++ < 3) log("instance patch: binding %u: no variable (%zu uniform variables)", bindings[j], uniform_vars.size());
            return false;
        }
    }

    std::vector<uint32_t> types;  // new declarations, just before the first function
    if (!i32) i32 = bound++, types.insert(types.end(), {(4u << 16) | OpTypeInt, i32, 32, 1});
    const uint32_t ptr = bound++, var = bound++;
    types.insert(types.end(), {(4u << 16) | OpTypePointer, ptr, StorageInput, i32});
    types.insert(types.end(), {(4u << 16) | OpVariable, ptr, var, StorageInput});
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> step_of;  // (index type, step) -> its constant
    auto step = [&](uint32_t type, uint32_t value) {
        uint32_t& id = step_of[{type, value}];
        if (!id) id = bound++, types.insert(types.end(), {(4u << 16) | OpConstant, type, id, value});
        return id;
    };
    std::vector<uint32_t> body;
    int patched = 0;
    for (const Inst& n : insts) {
        if (n.at < first_function) continue;
        const uint32_t* w = &in[n.at];
        for (uint32_t k = 1; k < n.len; k++) {
            if (!vars.count(w[k])) continue;
            const bool chain = (n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && k == 3 && n.len >= 6;
            if (!chain) {  // the block used some other way: not patched
                static int logged = 0;
                if (logged++ < 3) log("instance patch: block used by op %u (operand %u, length %u)", n.op, k, n.len);
                return false;
            }
        }
        if ((n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && n.len >= 6 && vars.count(w[3])) {
            auto t = type_of.find(w[5]);
            if (t == type_of.end() || !ints.count(t->second)) {
                static int logged = 0;
                if (logged++ < 3) log("instance patch: index %u of unknown type", w[5]);
                return false;
            }
            const uint32_t type = t->second, k = step(type, vars[w[3]]);
            const uint32_t loaded = bound++, scaled = bound++, sum = bound++;
            uint32_t inst = loaded;
            body.insert(body.end(), {(4u << 16) | OpLoad, i32, loaded, var});
            if (type != i32) inst = bound++, body.insert(body.end(), {(4u << 16) | OpBitcast, type, inst, loaded});
            body.insert(body.end(), {(5u << 16) | OpIMul, type, scaled, inst, k});
            body.insert(body.end(), {(5u << 16) | OpIAdd, type, sum, w[5], scaled});
            body.insert(body.end(), w, w + n.len);
            body[body.size() - n.len + 5] = sum;
            patched++;
            continue;
        }
        body.insert(body.end(), w, w + n.len);
    }
    if (!patched) {
        static int logged = 0;
        if (logged++ < 3) log("instance patch: binding %u: nothing to patch", bindings[0]);
        return false;
    }
    // The header with the new input on the entry point's interface and its BuiltIn decoration, then the types.
    out.assign(in.begin(), in.begin() + 5);
    bool entry_done = false;
    for (const Inst& n : insts) {
        if (n.at >= first_type) break;
        const uint32_t* w = &in[n.at];
        if (n.op == OpEntryPoint && !entry_done) {
            out.push_back(((n.len + 1) << 16) | OpEntryPoint);
            out.insert(out.end(), w + 1, w + n.len);
            out.push_back(var);
            entry_done = true;
            continue;
        }
        out.insert(out.end(), w, w + n.len);
    }
    if (!entry_done) {
        log("instance patch: no entry point");
        return false;
    }
    out.insert(out.end(), {(4u << 16) | OpDecorate, var, DecorationBuiltIn, BuiltInInstanceIndex});
    out.insert(out.end(), in.begin() + first_type, in.begin() + first_function);
    out.insert(out.end(), types.begin(), types.end());
    out.insert(out.end(), body.begin(), body.end());
    out[3] = bound;
    return true;
}

}  // namespace nx::nvn
