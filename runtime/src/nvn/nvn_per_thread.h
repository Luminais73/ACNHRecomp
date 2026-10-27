// The replay's per-thread state (docs/PARALLEL_REPLAY.md): a copy per replaying thread, the replay thread's in slot 0
// (also every other thread's: what one global was) and the parallel replay's workers' in 1-3. The slot comes from the
// thread pointer register: thread_local in a shared library goes through the dynamic TLS resolver on each access,
// which made the replay 15% slower on a Snapdragon 865, and Android refuses the initial-exec model in an app's library.
#pragma once

#include <cstdint>

namespace nx::nvn {

#if defined(__aarch64__) && defined(__ANDROID__)
constexpr int kReplaySlots = 4;
inline uintptr_t g_replay_tp[kReplaySlots] = {};  // [1..3]: the workers' thread pointers (nvn_parallel.cpp)
inline uintptr_t thread_pointer() {
    uintptr_t tp;
    asm("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
}
inline int replay_slot() {
    const uintptr_t tp = thread_pointer();
    return tp == g_replay_tp[1] ? 1 : tp == g_replay_tp[2] ? 2 : tp == g_replay_tp[3] ? 3 : 0;
}
#else
constexpr int kReplaySlots = 1;  // one replaying thread
inline int replay_slot() { return 0; }
#endif

template <typename T>
struct PerThread {
    T v[kReplaySlots];
    PerThread() = default;
    explicit PerThread(const T& init) {
        for (T& x : v) x = init;
    }
    T& operator*() { return v[replay_slot()]; }
    T* operator->() { return &v[replay_slot()]; }
};

}  // namespace nx::nvn
