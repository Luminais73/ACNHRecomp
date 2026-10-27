// The replay of recorded NVN command lists into a Vulkan command buffer (split from nvn_vk.cpp): render targets,
// clears, calls into other lists, copies; draws and state go to nvn_draw.cpp.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "nvn_draw_state.h"
#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {
// The lists called during one submit, by handle (PHASE4.md C5): command_list() takes a mutex and walks a map, and a
// course frame calls hundreds of lists, each looked up twice (the dispatch count, then the replay). The first lookup
// of a handle in a submit is kept for the rest of it: a list re-recorded meanwhile on the game's recording thread
// is not mixed into a submit already being replayed. NX_CALL_TABLE=0: every call looks up.
// Open addressing, a slot per handle; a slot of an older submit is empty (no clearing: a submit only bumps the
// generation). The lists themselves are kept alive by g_kept until the submit ends.
struct CallSlot {
    uint64_t handle = 0, generation = 0;
    CmdList* list = nullptr;
};
std::vector<CallSlot> g_calls(1 << 13);
uint64_t g_call_gen = 1;
size_t g_call_used = 0;
std::vector<std::shared_ptr<CmdList>> g_kept;  // what the submit's lookups returned, alive until it ends

void calls_grow() {
    std::vector<CallSlot> old(g_calls.size() * 2);
    old.swap(g_calls);
    for (const CallSlot& s : old)
        if (s.generation == g_call_gen) {
            size_t k = (s.handle >> 4) * 0x9E3779B97F4A7C15ull >> 40;
            while (g_calls[k & (g_calls.size() - 1)].generation == g_call_gen) k++;
            g_calls[k & (g_calls.size() - 1)] = s;
        }
}
}  // namespace

void submit_lists_reset() { g_call_gen++, g_call_used = 0, g_kept.clear(); }

// A raw pointer: the submit keeps the list alive until it ends (a shared_ptr returned by value was an atomic increment
// and decrement per call, thousands a frame). NX_CALL_TABLE=0: every call looks up.
CmdList* submit_command_list(uint64_t handle) {
    static const bool off = [] {
        const char* e = std::getenv("NX_CALL_TABLE");
        return e && e[0] == '0';
    }();
    if (off) {
        auto l = command_list(handle);
        if (l) g_kept.push_back(l);
        return l.get();
    }
    const size_t mask = g_calls.size() - 1;
    size_t k = (handle >> 4) * 0x9E3779B97F4A7C15ull >> 40;
    for (;; k++) {
        CallSlot& s = g_calls[k & mask];
        if (s.generation != g_call_gen) break;
        if (s.handle == handle) return s.list;
    }
    auto l = command_list(handle);
    if (!l) return nullptr;
    g_kept.push_back(l);
    if (++g_call_used * 2 > g_calls.size()) {
        calls_grow();
        return submit_command_list(handle);  // found or placed in the larger table; the list is kept twice: fine
    }
    g_calls[k & mask] = {handle, g_call_gen, l.get()};
    return l.get();
}

// Live: a file trace_cmds ("<count>") in the data folder traces the next <count> ops as NX_TRACE_CMDS does (checked
// once a second at a top-level list, then deleted).
int live_trace_cmds() {
    static int n = 0;
    static uint64_t next = 0;
    if (uint64_t now = plat::monotonic_ns(); now >= next && plat::debug_files()) {
        next = now + 1'000'000'000;
        const std::string path = plat::data_root() + "/trace_cmds";
        if (FILE* f = plat::fopen_utf8(path, "rb")) {
            if (std::fscanf(f, "%d", &n) != 1) n = 0;
            std::fclose(f);
            std::remove(path.c_str());
            log("trace_cmds: the next %d ops (from draw %llu)", n, (unsigned long long)(*replay::g_stats).done);
        }
    }
    int r = n;
    n = 0;
    return r;
}

// One command as replay_list replays it, CallList apart (nvn_parallel.cpp replays a flattened frame with it).
void replay_cmd(VkCommandBuffer& cb, Targets& st, const Cmd& c, const CmdList& list) {
    if (c.op == Cmd::SetRenderTargets || c.op == Cmd::CopyTextureToTexture || c.op == Cmd::CopyBufferToTexture ||
        c.op == Cmd::CopyTextureToBuffer)
        (*replay::g_state_gen)++;
    switch (c.op) {
        case Cmd::SetRenderTargets: {  // the pass ends only when a draw or clear needs other targets
            static const bool always = std::getenv("NX_TARGETS_BREAK") != nullptr;  // bisecting: the old way
            if (always) draw_end_rendering(cb, 7);
            st.color_count = c.n;
            for (uint32_t k = 0; k < 8; k++) st.colors[k] = k < c.n ? c.a[k] : 0;
            st.depth = c.a[8];
            st.views = {};
            if (c.blob < list.blobs.size() && list.blobs[c.blob].size() == sizeof(TargetViews))
                std::memcpy(&st.views, list.blobs[c.blob].data(), sizeof st.views);
            break;
        }
        case Cmd::ClearColor:
        case Cmd::ClearDepthStencil: draw_clear(cb, st, c); break;
        case Cmd::CopyTextureToTexture:
            draw_end_rendering(cb, 5);
            if (gpu_profile_on()) gpu_profile_mark(cb, "texture copies");
            replay_copy(cb, c, list);
            if (gpu_profile_on()) gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
            break;
        case Cmd::CopyBufferToTexture:
        case Cmd::CopyTextureToBuffer:
            draw_end_rendering(cb, 5);
            replay_buffer_copy(cb, c, list);
            break;
        default:
            draw_command(cb, c, list, st);
            // NX_PIPELINED: after the submit's last dispatch the rest goes into a second command buffer.
            if (c.op == Cmd::DispatchCompute) cb = frame_after_dispatch(cb);
            break;
    }
}

void replay_list(VkCommandBuffer cb, Targets& st, const CmdList& list, int depth) {
    if (skin_lists_on()) skin_list_replayed(list, depth);
    static int live_n = 0;
    if (depth == 0 && live_n <= 0) live_n = live_trace_cmds();
    for (const Cmd& c : list.cmds) {
        cb = frame_chunk(frame_cb(cb), (*replay::g_stats).done, pass_open());  // part of the submit queued early (PLAN.md R1)
        // NX_TRACE_CMDS=<first draw>: the ops replayed from that draw on (20000), with n, mask and a[0].
        static const char* tc = std::getenv("NX_TRACE_CMDS");
        static const uint64_t tc_from = tc ? std::strtoull(tc, nullptr, 10) : 0;
        static int tc_n = 0;
        if ((tc && (*replay::g_stats).done >= tc_from && tc_n++ < 20000) || live_n-- > 0)
            log("cmd after draw %llu: op %u n %u mask %x a0 %llx a1 %llx i %d %d %d %d f %g %g %g %g depth %d",
                (unsigned long long)(*replay::g_stats).done, unsigned(c.op), c.n, c.mask, (unsigned long long)c.a[0],
                (unsigned long long)c.a[1], c.i[0], c.i[1], c.i[2], c.i[3], c.f[0], c.f[1], c.f[2], c.f[3], depth);
        if (c.op == Cmd::CallList) {
            if (depth < 8) {
                if (auto l = submit_command_list(c.a[0])) {
                    replay_list(cb, st, *l, depth + 1);
                    cb = frame_cb(cb);  // NX_PIPELINED: the called list may have split the submit
                } else {
                    static uint64_t missing = 0;
                    if (++missing <= 20 || missing % 1000 == 0)
                        log("nvn: called handle %llx is unknown (%llu calls of unknown handles): %s",
                            (unsigned long long)c.a[0], (unsigned long long)missing,
                            explain_handle(c.a[0]).c_str());
                }
            }
            continue;
        }
        replay_cmd(cb, st, c, list);
    }
}

}  // namespace nx::nvn
