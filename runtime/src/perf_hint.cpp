// Performance hints (docs/nvn/ANDROID-PORT.md A5, DEBUG-2.md #84): on Android the threads that make a frame (the
// game's render thread, which also replays NVN, and the presenting one) join an ADPF session (APerformanceHint,
// Android 13+) with a 16.7 ms target, and each frame's duration is reported. The system then raises the big cores'
// clocks when a frame runs late instead of guessing from load, which on phones keeps a steady 60 fps cooler and faster
// than the default governor. The API is looked up with dlsym, so older Android versions just go without.
// NX_ADPF=0: off. Other platforms: nothing.
#include "platform.h"

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include <cerrno>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include <nx/runtime.h>
#endif

namespace nx::plat {

#if defined(__ANDROID__)
namespace {

using GetManager = void* (*)();
using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
using ReportActual = int (*)(void*, int64_t);
using CloseSession = void (*)(void*);

struct Api {
    GetManager get_manager = nullptr;
    CreateSession create = nullptr;
    ReportActual report = nullptr;
    CloseSession close = nullptr;
    bool ok = false;
};

const Api& api() {
    static const Api a = [] {
        Api x;
        const char* e = std::getenv("NX_ADPF");
        if (e && e[0] == '0') return x;
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) return x;
        x.get_manager = reinterpret_cast<GetManager>(dlsym(lib, "APerformanceHint_getManager"));
        x.create = reinterpret_cast<CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
        x.report = reinterpret_cast<ReportActual>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
        x.close = reinterpret_cast<CloseSession>(dlsym(lib, "APerformanceHint_closeSession"));
        x.ok = x.get_manager && x.create && x.report && x.close;
        return x;
    }();
    return a;
}

std::mutex g_lock;
std::vector<int32_t> g_tids;
void* g_session = nullptr;
bool g_dirty = false;

}  // namespace

// NX_GPU_TURBO=1 (an experiment, off by default): the Adreno kernel driver's power control off, so the GPU runs at its
// highest clock (thermal limits still apply). The idea is libadrenotools' adrenotools_set_turbo (BSD-2-Clause); this
// is written from the kernel's msm_kgsl UAPI: IOCTL_KGSL_SETPROPERTY (type 0x09, nr 0x32) with KGSL_PROP_PWRCTRL (0xE)
// and the value 0. The 30-fps lock lowered the GPU clock and frames then took longer (MEASUREMENTS.md, phase 4).
// The setting outlives the process (the GPU stayed at its top clock with the game closed), so every start sets it:
// off with NX_GPU_TURBO=1, back on (the driver's normal clock control) otherwise.
void gpu_turbo() {
    const char* e = std::getenv("NX_GPU_TURBO");
    const bool turbo = e && e[0] == '1';
    struct Property {
        unsigned int type;
        void* value;
        size_t sizebytes;
    };
    int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        log("gpu turbo: /dev/kgsl-3d0 not opened (errno %d)", errno);
        return;
    }
    unsigned int enable = turbo ? 0 : 1;
    Property p{0xE, &enable, sizeof enable};
    int r = ioctl(fd, _IOW(0x09, 0x32, Property), &p);
    log("gpu turbo: KGSL power control %s: %s (errno %d)", turbo ? "off (NX_GPU_TURBO=1)" : "on (normal)",
        r == 0 ? "done" : "refused", r == 0 ? 0 : errno);
    if (!turbo) close(fd);
    // The descriptor stays open: the driver ties the setting to it.
}

// NX_ROOT_CLOCKS=1 (Android, rooted phones only; video.root_clocks in the settings): Samsung's system manager (SSRM)
// caps the GPU through /sys/kernel/gpu/gpu_max_clock (441 of 587 MHz on a Galaxy S20 FE, even cold), which left the
// game GPU-bound at ~25 fps. A root shell (su) puts the GPU's top clock back every few seconds, as SSRM sets it again.
// The CPU caps are left alone: the replay did not get faster with them lifted (memory-bound), only hotter. Nothing
// else is touched: the kernel's own thermal limits stay.
void root_clocks() {
    const char* e = std::getenv("NX_ROOT_CLOCKS");
    if (!(e && e[0] == '1')) return;
    std::thread([] {
        set_thread_name("root clocks");
        FILE* su = popen("su", "w");
        if (!su) {
            log("root clocks: su not available");
            return;
        }
        log("root clocks: on (NX_ROOT_CLOCKS=1): the GPU's top clock kept available");
        const char* cmd =
            "G=/sys/kernel/gpu; K=/sys/class/kgsl/kgsl-3d0; "
            "if [ -w $G/gpu_max_clock ] && [ -r $K/gpu_available_frequencies ]; then "
            "m=0; for f in $(cat $K/gpu_available_frequencies); do [ $f -gt $m ] && m=$f; done; m=$((m / 1000000)); "
            "[ \"$(cat $G/gpu_max_clock)\" = \"$m\" ] || echo $m > $G/gpu_max_clock; fi\n";
        for (;;) {
            if (std::fputs(cmd, su) < 0 || std::fflush(su) != 0) {
                log("root clocks: the root shell ended");
                pclose(su);
                return;
            }
            sleep_ns(3'000'000'000ull);
        }
    }).detach();
}

void perf_hint_thread() {
    if (!api().ok) return;
    thread_local bool joined = false;
    if (joined) return;
    joined = true;
    std::lock_guard lk(g_lock);
    int32_t tid = int32_t(syscall(SYS_gettid));
    if (g_tids.size() < 8 && std::find(g_tids.begin(), g_tids.end(), tid) == g_tids.end())
        g_tids.push_back(tid), g_dirty = true;
}

void perf_hint_frame(int64_t ns) {
    const Api& a = api();
    if (!a.ok || ns <= 0) return;
    std::lock_guard lk(g_lock);
    if (g_dirty && !g_tids.empty()) {  // a new thread made part of the frame: a session with all of them
        if (g_session) a.close(g_session);
        // The ADPF target is the frame period: 33.3 ms when NX_FPS_CAP holds the game at 30 fps (nvn_queue.cpp).
        const char* cap = std::getenv("NX_FPS_CAP");
        int64_t target = cap && std::atoi(cap) > 0 && std::atoi(cap) <= 30 ? 33'333'333 : 16'666'667;
        g_session = a.create(a.get_manager(), g_tids.data(), g_tids.size(), target);
        g_dirty = false;
        log("adpf: %s for %zu threads", g_session ? "session made" : "no session", g_tids.size());
    }
    if (g_session) a.report(g_session, ns);
}
#else
void gpu_turbo() {}
void root_clocks() {}
void perf_hint_thread() {}
void perf_hint_frame(int64_t) {}
#endif

}  // namespace nx::plat
