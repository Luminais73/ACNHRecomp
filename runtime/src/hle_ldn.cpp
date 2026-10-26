// nn::ldn, local wireless play (the airport's "local play": visiting other islands nearby).
//
// Off by default, as on a console in airplane mode: Initialize answers ResultDeviceDisabled and the game says local
// play is unavailable (before, it reached the ipc stub, got no session object back and read through a null pointer).
//
// NX_LDN=1: step 1 of local play over the local network (docs/LOCAL_PLAY.md): the API answers as a console with
// wireless on, finds no other islands and lets the game open its own network, and logs every call with what the game
// passed (configurations as hex), to learn which calls the game makes, in what order and with what.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include <nx/runtime.h>

#include "kernel.h"

namespace nx {

namespace {

constexpr uint32_t kOk = 0;
constexpr uint32_t kLdnDeviceDisabled = (22u << 9) | 203u;  // ldn ResultDeviceDisabled (wireless off)
constexpr uint32_t kLdnInvalidState = (22u << 9) | 32u;     // ldn ResultInvalidState

// nn::ldn::State
enum State : uint32_t { None = 0, Initialized = 1, AccessPoint = 2, AccessPointCreated = 3, Station = 4, StationConnected = 5, Error = 6 };
const char* state_name(uint32_t s) {
    static const char* n[] = {"None", "Initialized", "AccessPoint", "AccessPointCreated", "Station", "StationConnected", "Error"};
    return s < 7 ? n[s] : "?";
}

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_LDN");
        return e && e[0] == '1';
    }();
    return on;
}

std::mutex g_lock;
uint32_t g_state = None;
std::shared_ptr<EventState> g_state_event;  // AttachStateChangeEvent's: signaled at every change of state
uint8_t g_advertise[0x180] = {};
uint64_t g_advertise_size = 0;

std::string hex(const void* p, size_t n) {
    std::string s;
    if (!p) return "(null)";
    const auto* b = static_cast<const uint8_t*>(p);
    char t[4];
    for (size_t i = 0; i < n; i++) {
        std::snprintf(t, sizeof t, "%02x", b[i]);
        s += t;
        if (i % 4 == 3 && i + 1 < n) s += ' ';
    }
    return s;
}

void set_state(uint32_t s) {
    if (g_state == s) return;
    log("ldn: state %s -> %s", state_name(g_state), state_name(s));
    g_state = s;
    if (g_state_event) {
        g_state_event->signaled = true;
        notify_all();
    }
}

void disabled(CPU& c) {
    if (!enabled()) return void(c.x[0] = kLdnDeviceDisabled);
    c.x[0] = kOk;
}

void Initialize(CPU& c) {
    if (!enabled()) return void(c.x[0] = kLdnDeviceDisabled);
    std::lock_guard lk(g_lock);
    log("ldn: Initialize (%s)", hle_current_name());
    if (g_state == None) set_state(Initialized);
    c.x[0] = kOk;
}

void Finalize(CPU& c) {
    std::lock_guard lk(g_lock);
    if (enabled()) log("ldn: Finalize");
    g_state = None;
    c.x[0] = kOk;
}

void GetState(CPU& c) {
    std::lock_guard lk(g_lock);
    c.x[0] = enabled() ? g_state : None;
}

// AttachStateChangeEvent(os::SystemEventType*): a system event of ours, signaled at each change of state.
void AttachStateChangeEvent(CPU& c) {
    const uint64_t out = c.x[0];
    auto e = std::make_shared<EventState>();
    auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
    r->state = w->state = e;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    static const uint64_t attach =
        find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[0] = out, c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // auto clear
    if (attach) call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    std::lock_guard lk(g_lock);
    g_state_event = e;
    if (enabled()) log("ldn: AttachStateChangeEvent(%#llx)", (unsigned long long)out);
}

void OpenAccessPoint(CPU& c) {
    std::lock_guard lk(g_lock);
    log("ldn: OpenAccessPoint (state %s)", state_name(g_state));
    if (g_state != Initialized) return void(c.x[0] = kLdnInvalidState);
    set_state(AccessPoint);
    c.x[0] = kOk;
}

void CloseAccessPoint(CPU& c) {
    std::lock_guard lk(g_lock);
    log("ldn: CloseAccessPoint (state %s)", state_name(g_state));
    set_state(Initialized);
    c.x[0] = kOk;
}

void OpenStation(CPU& c) {
    std::lock_guard lk(g_lock);
    log("ldn: OpenStation (state %s)", state_name(g_state));
    if (g_state != Initialized) return void(c.x[0] = kLdnInvalidState);
    set_state(Station);
    c.x[0] = kOk;
}

void CloseStation(CPU& c) {
    std::lock_guard lk(g_lock);
    log("ldn: CloseStation (state %s)", state_name(g_state));
    set_state(Initialized);
    c.x[0] = kOk;
}

// CreateNetwork(const NetworkConfig&, const SecurityConfig&, const UserConfig&)
void CreateNetwork(CPU& c) {
    std::lock_guard lk(g_lock);
    log("ldn: CreateNetwork network config %s", hex(reinterpret_cast<void*>(c.x[0]), 0x20).c_str());
    log("ldn:   security config %s", hex(reinterpret_cast<void*>(c.x[1]), 0x44).c_str());
    log("ldn:   user config %s", hex(reinterpret_cast<void*>(c.x[2]), 0x30).c_str());
    if (g_state != AccessPoint) return void(c.x[0] = kLdnInvalidState);
    set_state(AccessPointCreated);
    c.x[0] = kOk;
}

// CreateNetworkPrivate(network, security, SecurityParameter, user, int count, const AddressEntry*)
void CreateNetworkPrivate(CPU& c) {
    std::lock_guard lk(g_lock);
    log("ldn: CreateNetworkPrivate network %s count %d", hex(reinterpret_cast<void*>(c.x[0]), 0x20).c_str(), int(c.x[4]));
    if (g_state != AccessPoint) return void(c.x[0] = kLdnInvalidState);
    set_state(AccessPointCreated);
    c.x[0] = kOk;
}

void DestroyNetwork(CPU& c) {
    std::lock_guard lk(g_lock);
    log("ldn: DestroyNetwork (state %s)", state_name(g_state));
    if (g_state == AccessPointCreated) set_state(AccessPoint);
    c.x[0] = kOk;
}

// SetAdvertiseData(const void*, size_t): what the host shows to stations that scan (the island, the player).
void SetAdvertiseData(CPU& c) {
    std::lock_guard lk(g_lock);
    const uint64_t n = c.x[1];
    log("ldn: SetAdvertiseData %llu bytes: %s", (unsigned long long)n,
        hex(reinterpret_cast<void*>(c.x[0]), size_t(std::min<uint64_t>(n, 0x180))).c_str());
    if (n > sizeof g_advertise) return void(c.x[0] = kLdnInvalidState);
    if (n && c.x[0]) std::memcpy(g_advertise, reinterpret_cast<void*>(c.x[0]), n);
    g_advertise_size = n;
    c.x[0] = kOk;
}

void SetStationAcceptPolicy(CPU& c) {
    log("ldn: SetStationAcceptPolicy(%u)", uint32_t(c.x[0]));
    c.x[0] = kOk;
}

// Scan(NetworkInfo* out, int* count, int buffer count, const ScanFilter&, int channel): nobody around yet.
void Scan(CPU& c) {
    log("ldn: %s(out %#llx, count %#llx, max %d, filter %s, channel %d)", hle_current_name(), (unsigned long long)c.x[0],
        (unsigned long long)c.x[1], int(c.x[2]), hex(reinterpret_cast<void*>(c.x[3]), 0x60).c_str(), int(c.x[4]));
    if (c.x[1]) *reinterpret_cast<int32_t*>(c.x[1]) = 0;
    c.x[0] = kOk;
}

// Connect(const NetworkInfo&, const SecurityConfig&, const UserConfig&, int version, ConnectOption)
void Connect(CPU& c) {
    log("ldn: %s(network %s ..., version %d, option %u)", hle_current_name(),
        hex(reinterpret_cast<void*>(c.x[0]), 0x40).c_str(), int(c.x[3]), uint32_t(c.x[4]));
    c.x[0] = kLdnInvalidState;
}

void Disconnect(CPU& c) {
    std::lock_guard lk(g_lock);
    log("ldn: Disconnect (state %s)", state_name(g_state));
    if (g_state == StationConnected) set_state(Station);
    c.x[0] = kOk;
}

// GetNetworkInfo(NetworkInfo*): the network this console made or joined (0x480 bytes).
void GetNetworkInfo(CPU& c) {
    log("ldn: GetNetworkInfo(%#llx) in state %s", (unsigned long long)c.x[0], state_name(g_state));
    if (c.x[0]) std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x480);
    c.x[0] = g_state == AccessPointCreated || g_state == StationConnected ? kOk : kLdnInvalidState;
}

void GetNetworkConfig(CPU& c) {
    log("ldn: GetNetworkConfig(%#llx)", (unsigned long long)c.x[0]);
    if (c.x[0]) std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x20);
    c.x[0] = kLdnInvalidState;
}

void GetSecurityParameter(CPU& c) {
    log("ldn: GetSecurityParameter(%#llx)", (unsigned long long)c.x[0]);
    if (c.x[0]) std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x20);
    c.x[0] = kLdnInvalidState;
}

// GetIpv4Address(Ipv4Address*, SubnetMask*): 169.254.1.1/24 while a network is up.
void GetIpv4Address(CPU& c) {
    log("ldn: GetIpv4Address in state %s", state_name(g_state));
    if (c.x[0]) *reinterpret_cast<uint32_t*>(c.x[0]) = 0xA9FE0101;
    if (c.x[1]) *reinterpret_cast<uint32_t*>(c.x[1]) = 0xFFFFFF00;
    c.x[0] = kOk;
}

// MakeBroadcastAddress(Ipv4Address, SubnetMask) -> Ipv4Address
void MakeBroadcastAddress(CPU& c) {
    const uint32_t ip = uint32_t(c.x[0]), mask = uint32_t(c.x[1]);
    c.x[0] = (ip & mask) | ~mask;
}

void GetDisconnectReason(CPU& c) {
    log("ldn: GetDisconnectReason");
    c.x[0] = 0;
}

void Reject(CPU& c) {
    log("ldn: Reject(%#x)", uint32_t(c.x[0]));
    c.x[0] = kOk;
}

}  // namespace

NX_HLE("_ZN2nn3ldn10InitializeEv", Initialize);
NX_HLE("_ZN2nn3ldn16InitializeSystemEv", Initialize);
NX_HLE("_ZN2nn3ldn16InitializeSystemEi", Initialize);
NX_HLE("_ZN2nn3ldn17InitializeMonitorEv", disabled);
NX_HLE("_ZN2nn3ldn8FinalizeEv", Finalize);
NX_HLE("_ZN2nn3ldn14FinalizeSystemEv", Finalize);
NX_HLE("_ZN2nn3ldn15FinalizeMonitorEv", Finalize);
NX_HLE("_ZN2nn3ldn8GetStateEv", GetState);
NX_HLE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", AttachStateChangeEvent);
NX_HLE("_ZN2nn3ldn15OpenAccessPointEv", OpenAccessPoint);
NX_HLE("_ZN2nn3ldn16CloseAccessPointEv", CloseAccessPoint);
NX_HLE("_ZN2nn3ldn11OpenStationEv", OpenStation);
NX_HLE("_ZN2nn3ldn12CloseStationEv", CloseStation);
NX_HLE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", CreateNetwork);
NX_HLE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_"
       "10UserConfigEiPKNS0_12AddressEntryE",
       CreateNetworkPrivate);
NX_HLE("_ZN2nn3ldn14DestroyNetworkEv", DestroyNetwork);
NX_HLE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", SetAdvertiseData);
NX_HLE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", SetStationAcceptPolicy);
NX_HLE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", Scan);
NX_HLE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", Scan);
NX_HLE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", Connect);
NX_HLE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_"
       "10UserConfigEiNS0_13ConnectOptionE",
       Connect);
NX_HLE("_ZN2nn3ldn10DisconnectEv", Disconnect);
NX_HLE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", GetNetworkInfo);
NX_HLE("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", GetNetworkConfig);
NX_HLE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", GetSecurityParameter);
NX_HLE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", GetIpv4Address);
NX_HLE("_ZN2nn3ldn20MakeBroadcastAddressENS0_11Ipv4AddressENS0_10SubnetMaskE", MakeBroadcastAddress);
NX_HLE("_ZN2nn3ldn19GetDisconnectReasonEv", GetDisconnectReason);
NX_HLE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", Reject);

}  // namespace nx
