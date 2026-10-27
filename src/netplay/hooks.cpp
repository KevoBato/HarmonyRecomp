// harmonyrecomp netplay - replacements for the game's Xbox Live imports.
//
// REX_HOOK defines the import symbol in the game executable, so these take
// precedence over the SDK's versions in rexruntime.dll. With netplay off they
// behave like the SDK (offline profile, loopback XNet, no sessions).

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xam/app_manager.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "gamesock.h"
#include "guest.h"
#include "lobby.h"
#include "netplay.h"
#include "sessions.h"
#include "udp.h"

// The SDK's X_ERROR_* / X_E_* macros expand to these types.
using rex::X_HRESULT;
using rex::X_RESULT;
using rex::X_STATUS;

// SDK implementations used when netplay is off (exported by rexruntime).
namespace rex::kernel::xam {
u32 XamUserGetSigninState_entry(u32 user_index);
u32 XMsgInProcessCall_entry(u32 app, u32 message, u32 arg1, u32 arg2);
i32 XamUserGetXUID_entry(u32 user_index, u32 type_mask, mapped_u64 xuid_ptr);
u32 XamUserGetName_entry(u32 user_index, mapped_string buffer, u32 buffer_len);
u32 XamUserCheckPrivilege_entry(u32 user_index, u32 mask, mapped_u32 out_value);
}  // namespace rex::kernel::xam

namespace harmony::netplay {
namespace {

using namespace guest;
namespace sdk = rex::kernel::xam;

constexpr uint32_t kSignedInToLive = 2;

// ---- Users ----------------------------------------------------------------------

u32 XamUserGetSigninState(u32 user_index) {
  if (Enabled() && user_index == 0) return kSignedInToLive;
  return sdk::XamUserGetSigninState_entry(user_index);
}

struct X_USER_SIGNIN_INFO {
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> flags;
  rex::be<uint32_t> signin_state;
  rex::be<uint32_t> guest_number;
  rex::be<uint32_t> sponsor_user_index;
  char name[16];
};
static_assert(sizeof(X_USER_SIGNIN_INFO) == 40);

i32 XamUserGetSigninInfo(u32 user_index, u32 flags, ppc_ptr_t<X_USER_SIGNIN_INFO> info) {
  if (!info) return X_E_INVALIDARG;
  std::memset(info.host_address(), 0, sizeof(X_USER_SIGNIN_INFO));
  if (user_index) return X_E_NO_SUCH_USER;
  const auto& profile = REX_KERNEL_STATE()->user_profile();
  std::string name = profile ? profile->name() : std::string("Player");
  uint64_t offline_xuid = profile ? profile->xuid() : 0;
  if (Enabled()) {
    // flags: 1 = online XUID only, 2 = offline XUID only.
    info->xuid = (flags & 2) ? offline_xuid : OnlineXuid();
    info->signin_state = kSignedInToLive;
    name = Gamertag();
  } else {
    info->xuid = offline_xuid;
    info->signin_state = profile ? profile->signin_state() : 0;
  }
  std::strncpy(info->name, name.c_str(), sizeof(info->name) - 1);
  return X_E_SUCCESS;
}

i32 XamUserGetXUID(u32 user_index, u32 type_mask, mapped_u64 xuid_ptr) {
  // Only the online XUID changes: the offline one (and with it anything tied
  // to the profile, such as save ownership) stays what the SDK reports.
  if (Enabled() && user_index == 0 && xuid_ptr && (type_mask & (2 | 4))) {
    *xuid_ptr = OnlineXuid();
    return X_E_SUCCESS;
  }
  return sdk::XamUserGetXUID_entry(user_index, type_mask, xuid_ptr);
}

u32 XamUserGetName(u32 user_index, mapped_string buffer, u32 buffer_len) {
  if (Enabled() && user_index == 0 && buffer && buffer_len) {
    std::string name = Gamertag();
    size_t n = std::min<size_t>(name.size(), std::min<u32>(buffer_len, 16) - 1);
    std::memcpy(buffer.host_address(), name.data(), n);
    buffer.host_address()[n] = 0;
    return X_E_SUCCESS;
  }
  return sdk::XamUserGetName_entry(user_index, buffer, buffer_len);
}

u32 XamUserCheckPrivilege(u32 user_index, u32 mask, mapped_u32 out_value) {
  if (Enabled() && user_index == 0 && out_value) {
    *out_value = 1;  // multiplayer, communications, ... all allowed
    return X_ERROR_SUCCESS;
  }
  return sdk::XamUserCheckPrivilege_entry(user_index, mask, out_value);
}

// ---- Sessions -------------------------------------------------------------------

u32 XamSessionCreateHandle(mapped_u32 handle_ptr) {
  if (!handle_ptr) return X_E_INVALIDARG;
  *handle_ptr = Enabled() ? sessions::CreateHandle() : 0xCAFEDEADu;
  return X_ERROR_SUCCESS;
}

u32 XamSessionRefObjByHandle(u32 handle, mapped_u32 obj_ptr) {
  if (!obj_ptr) return X_E_INVALIDARG;
  *obj_ptr = Enabled() ? sessions::ObjectForHandle(handle) : 0xDEADF00Du;
  return X_ERROR_SUCCESS;
}

u32 ForwardToSdk(u32 app, u32 message, u32 overlapped_ptr, u32 buffer_ptr, u32 buffer_length) {
  auto* ks = REX_KERNEL_STATE();
  uint32_t result =
      ks->app_manager()->DispatchMessageAsync(app, message, buffer_ptr, buffer_length);
  if (result == X_E_NOTFOUND) {
    REXKRNL_ERROR("XMsgStartIORequest: app {:08X} undefined", app);
    result = X_E_INVALIDARG;
    rex::system::XThread::SetLastError(X_ERROR_NOT_FOUND);
  }
  if (overlapped_ptr) {
    ks->CompleteOverlappedImmediate(overlapped_ptr, result);
    result = X_ERROR_IO_PENDING;
  }
  if (result == X_ERROR_SUCCESS || result == X_ERROR_IO_PENDING) {
    rex::system::XThread::SetLastError(0);
  }
  return result;
}

u32 XMsgStartIORequest(u32 app, u32 message, u32 overlapped_ptr, u32 buffer_ptr,
                       u32 buffer_length) {
  if (Enabled() && app == 0xFB) {
    uint32_t result = 0;
    if (sessions::HandleXgi(message, buffer_ptr, buffer_length, overlapped_ptr, &result)) {
      if (result == X_ERROR_SUCCESS || result == X_ERROR_IO_PENDING) {
        rex::system::XThread::SetLastError(0);
      }
      return result;
    }
  }
  return ForwardToSdk(app, message, overlapped_ptr, buffer_ptr, buffer_length);
}

// Synchronous XLiveBase calls the SDK does not implement.
constexpr u32 kXLiveBaseApp = 0xFC;
constexpr u32 kMsgUserMuteListQuery = 0x0005800E;

u32 XMsgInProcessCall(u32 app, u32 message, u32 arg1, u32 arg2) {
  if (app == kXLiveBaseApp && message == kMsgUserMuteListQuery) {
    // XUserMuteListQuery(user, remote xuid, &muted): arg1 = {u32 user; u64 xuid},
    // arg2 = BOOL* muted. The game checks it when a player joins; without an
    // answer the flag stays uninitialized and the joining player is dropped.
    // Nobody is muted here.
    if (auto* muted = REX_KERNEL_MEMORY()->TranslateVirtual<rex::be<uint32_t>*>(arg2); arg2) {
      *muted = 0u;
    }
    static bool logged = false;
    if (!logged && arg1) {
      logged = true;
      auto* req = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(arg1);
      uint64_t xuid = *reinterpret_cast<rex::be<uint64_t>*>(req + 8);
      NETPLAY_LOG("XUserMuteListQuery {:016X} -> not muted", xuid);
    }
    return X_ERROR_SUCCESS;
  }
  return sdk::XMsgInProcessCall_entry(app, message, arg1, arg2);
}

// ---- XNet ------------------------------------------------------------------------

constexpr u32 kXnAddrStatic = 0x04;
constexpr u32 kXnAddrDhcp = 0x08;
constexpr u32 kXnAddrGateway = 0x20;
constexpr u32 kXnAddrDns = 0x40;
constexpr u32 kXnAddrOnline = 0x80;
constexpr u32 kConnectStatusConnected = 2;

std::mutex g_xnet_mutex;
std::map<uint32_t, uint64_t> g_ip_sessions;  // IN_ADDR -> XNKID it was resolved with

u32 XNetGetTitleXnAddr(u32 caller, ppc_ptr_t<XNADDR> addr) {
  (void)caller;
  if (!addr) return 1;
  if (!Enabled()) {
    std::memset(addr.host_address(), 0, sizeof(XNADDR));
    addr->ina = 0x7F000001;
    std::memset(addr->ab_enet, 0xCC, sizeof(addr->ab_enet));
    return kXnAddrStatic;
  }
  FillXnAddr(LocalIp(), addr.host_address());
  return kXnAddrDhcp | kXnAddrGateway | kXnAddrDns | kXnAddrOnline;
}

u32 XNetXnAddrToInAddr(u32 caller, ppc_ptr_t<XNADDR> xna, ppc_ptr_t<XNKID> xnkid,
                       mapped_u32 in_addr) {
  (void)caller;
  if (!Enabled()) return 1;
  if (!xna || !in_addr) return X_E_INVALIDARG;
  uint32_t ip = IpFromXnAddr(*xna);
  *in_addr = ip;
  std::lock_guard lock(g_xnet_mutex);
  g_ip_sessions[ip] = xnkid ? ReadXnkid(*xnkid) : sessions::CurrentSessionId();
  return 0;
}

u32 XNetInAddrToXnAddr(u32 caller, u32 ina, ppc_ptr_t<XNADDR> xna, ppc_ptr_t<XNKID> xnkid) {
  (void)caller;
  if (!Enabled()) return 1;
  if (xna) FillXnAddr(ina, xna.host_address());
  if (xnkid) {
    uint64_t id = sessions::CurrentSessionId();
    {
      std::lock_guard lock(g_xnet_mutex);
      if (auto it = g_ip_sessions.find(ina); it != g_ip_sessions.end() && it->second) id = it->second;
    }
    WriteXnkid(id, xnkid.host_address());
  }
  return 0;
}

// Secure association setup: with plain UDP between players nothing to do.
u32 XNetConnect(u32 caller, u32 ina) {
  (void)caller;
  (void)ina;
  return 0;
}
u32 XNetGetConnectStatus(u32 caller, u32 ina) {
  (void)caller;
  (void)ina;
  return Enabled() ? kConnectStatusConnected : 0;
}
u32 XNetRegisterKey(u32 caller, ppc_ptr_t<XNKID> xnkid, ppc_ptr_t<XNKEY> key) {
  (void)caller;
  (void)xnkid;
  (void)key;
  return 0;
}
u32 XNetReplaceKey(u32 caller, ppc_ptr_t<XNKID> old_id, ppc_ptr_t<XNKID> new_id) {
  (void)caller;
  (void)old_id;
  (void)new_id;
  return 0;
}
u32 XNetUnregisterInAddr(u32 caller, u32 ina) {
  (void)caller;
  std::lock_guard lock(g_xnet_mutex);
  g_ip_sessions.erase(ina);
  return 0;
}

// ---- QoS ---------------------------------------------------------------------------

constexpr u32 kQosListenEnable = 0x01;
constexpr u32 kQosListenDisable = 0x02;
constexpr u32 kQosListenSetData = 0x04;
constexpr u32 kQosListenRelease = 0x10;

u32 XNetQosListen(u32 caller, ppc_ptr_t<XNKID> xnkid, u32 data_ptr, u32 data_size, u32 bps,
                  u32 flags) {
  (void)caller;
  (void)bps;
  if (!Enabled()) return X_ERROR_FUNCTION_FAILED;
  if (!xnkid) return X_E_INVALIDARG;
  uint64_t id = ReadXnkid(*xnkid);
  if (flags & kQosListenRelease) {
    lobby::ReleaseQosListen(id);
    NETPLAY_LOG("XNetQosListen {:016X} release", id);
    return 0;
  }
  std::optional<bool> enabled;
  if (flags & kQosListenEnable) enabled = true;
  if (flags & kQosListenDisable) enabled = false;
  std::vector<uint8_t> data;
  bool set_data = flags & kQosListenSetData;
  if (set_data) {
    auto* src = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(data_ptr);
    if (data_ptr && data_size <= 1024) data.assign(src, src + data_size);
  }
  lobby::SetQosListen(id, enabled, set_data ? &data : nullptr);
  NETPLAY_LOG("XNetQosListen {:016X} flags {:02X} data {} bytes", id, flags, data.size());
  return 0;
}

// Lookups in flight, so XNetQosRelease can wait for the worker to finish.
struct QosLookup {
  std::mutex mutex;
  bool done = false;
  bool release_requested = false;
  std::vector<uint32_t> data_blocks;
};
std::mutex g_qos_mutex;
std::map<uint32_t, std::shared_ptr<QosLookup>> g_qos_lookups;

void FreeQos(uint32_t qos_ptr, const std::vector<uint32_t>& blocks) {
  auto* mem = REX_KERNEL_MEMORY();
  for (uint32_t b : blocks) mem->SystemHeapFree(b);
  mem->SystemHeapFree(qos_ptr);
}

u32 XNetQosLookup(u32 caller, u32 count, u32 xnaddrs_ptr, u32 xnkids_ptr, u32 xnkeys_ptr,
                  u32 gateway_count, u32 gateways_ptr, u32 service_ids_ptr, u32 probes,
                  u32 bps, u32 flags, u32 event_handle, mapped_u32 out_qos) {
  (void)caller;
  (void)xnkeys_ptr;
  (void)gateways_ptr;
  (void)service_ids_ptr;
  (void)bps;
  (void)flags;
  if (!out_qos) return X_E_INVALIDARG;
  auto* mem = REX_KERNEL_MEMORY();

  struct Target {
    uint32_t ip = 0;
    uint64_t session = 0;
  };
  std::vector<Target> targets;
  auto* addr_ptrs = mem->TranslateVirtual<rex::be<uint32_t>*>(xnaddrs_ptr);
  auto* kid_ptrs = mem->TranslateVirtual<rex::be<uint32_t>*>(xnkids_ptr);
  for (uint32_t i = 0; i < count; ++i) {
    Target t;
    if (xnaddrs_ptr && addr_ptrs[i]) t.ip = IpFromXnAddr(*mem->TranslateVirtual<XNADDR*>(addr_ptrs[i]));
    if (xnkids_ptr && kid_ptrs[i]) t.session = ReadXnkid(*mem->TranslateVirtual<XNKID*>(kid_ptrs[i]));
    targets.push_back(t);
  }

  uint32_t total = count + gateway_count;
  uint32_t size = sizeof(XNQOS) + sizeof(XNQOSINFO) * (total ? total - 1 : 0);
  uint32_t qos_ptr = mem->SystemHeapAlloc(size);
  if (!qos_ptr) return X_ERROR_FUNCTION_FAILED;
  std::memset(mem->TranslateVirtual<uint8_t*>(qos_ptr), 0, size);
  auto* qos = mem->TranslateVirtual<XNQOS*>(qos_ptr);
  qos->count = total;
  qos->count_pending = total;
  *out_qos = qos_ptr;

  auto lookup = std::make_shared<QosLookup>();
  {
    std::lock_guard lock(g_qos_mutex);
    g_qos_lookups[qos_ptr] = lookup;
  }
  NETPLAY_LOG("XNetQosLookup {} target(s), {} gateway(s)", count, gateway_count);

  uint16_t probe_count = static_cast<uint16_t>(probes ? probes : 1);
  auto run = [=] {
    auto* m = REX_KERNEL_MEMORY();
    auto* q = m->TranslateVirtual<XNQOS*>(qos_ptr);
    for (uint32_t i = 0; i < total; ++i) {
      XNQOSINFO& info = q->info[i];
      if (i < targets.size()) {
        auto r = lobby::Qos(targets[i].ip, targets[i].session, 900);
        if (r.contacted) {
          info.probes_xmit = probe_count;
          info.probes_recv = probe_count;
          info.rtt_min_ms = static_cast<uint16_t>(std::min<uint32_t>(r.rtt_ms, 0xFFFF));
          info.rtt_med_ms = static_cast<uint16_t>(std::min<uint32_t>(r.rtt_ms, 0xFFFF));
          info.up_bits_per_sec = 10'000'000;
          info.down_bits_per_sec = 10'000'000;
          uint8_t f = kQosComplete | kQosTargetContacted;
          if (!r.enabled) f |= kQosTargetDisabled;
          if (!r.data.empty()) {
            uint32_t block = m->SystemHeapAlloc(static_cast<uint32_t>(r.data.size()));
            if (block) {
              std::memcpy(m->TranslateVirtual<uint8_t*>(block), r.data.data(), r.data.size());
              info.data_ptr = block;
              info.data_len = static_cast<uint16_t>(r.data.size());
              f |= kQosDataReceived;
              std::lock_guard lock(lookup->mutex);
              lookup->data_blocks.push_back(block);
            }
          }
          info.flags = f;
        } else {
          info.probes_xmit = probe_count;
          info.flags = kQosComplete;
        }
        NETPLAY_LOG("QoS {} session {:016X}: {} rtt {} ms, {} data bytes",
                    udp::ToString(targets[i].ip), targets[i].session,
                    r.contacted ? (r.enabled ? "ok" : "disabled") : "no answer", r.rtt_ms,
                    r.data.size());
      } else {
        // Security gateways (Live data centers) do not exist; report them
        // reachable so the game does not treat the connection as broken.
        info.probes_xmit = probe_count;
        info.probes_recv = probe_count;
        info.rtt_min_ms = 20;
        info.rtt_med_ms = 20;
        info.up_bits_per_sec = 10'000'000;
        info.down_bits_per_sec = 10'000'000;
        info.flags = kQosComplete | kQosTargetContacted;
      }
      q->count_pending = q->count_pending - 1;
    }
    if (event_handle) {
      if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event_handle)) {
        ev->Set(0, false);
      }
    }
    bool free_now = false;
    std::vector<uint32_t> blocks;
    {
      std::lock_guard lock(lookup->mutex);
      lookup->done = true;
      free_now = lookup->release_requested;
      blocks = lookup->data_blocks;
    }
    if (free_now) {
      FreeQos(qos_ptr, blocks);
      std::lock_guard lock(g_qos_mutex);
      g_qos_lookups.erase(qos_ptr);
    }
  };
  std::thread(run).detach();
  return 0;
}

u32 XNetQosRelease(u32 caller, u32 qos_ptr) {
  (void)caller;
  if (!qos_ptr) return X_STATUS_INVALID_PARAMETER;
  std::shared_ptr<QosLookup> lookup;
  {
    std::lock_guard lock(g_qos_mutex);
    auto it = g_qos_lookups.find(qos_ptr);
    if (it != g_qos_lookups.end()) {
      lookup = it->second;
      std::lock_guard l2(lookup->mutex);
      if (!lookup->done) {
        lookup->release_requested = true;  // the worker frees it when finished
        return 0;
      }
      g_qos_lookups.erase(it);
    }
  }
  // Ours (with its data blocks) or one from the SDK's XNetQosServiceLookup.
  FreeQos(qos_ptr, lookup ? lookup->data_blocks : std::vector<uint32_t>{});
  return 0;
}

}  // namespace
}  // namespace harmony::netplay

using namespace harmony::netplay;

REX_HOOK(__imp__XamUserGetSigninState, XamUserGetSigninState);
REX_HOOK(__imp__XamUserGetSigninInfo, XamUserGetSigninInfo);
REX_HOOK(__imp__XamUserGetXUID, XamUserGetXUID);
REX_HOOK(__imp__XamUserGetName, XamUserGetName);
REX_HOOK(__imp__XamUserCheckPrivilege, XamUserCheckPrivilege);
REX_HOOK(__imp__XamSessionCreateHandle, XamSessionCreateHandle);
REX_HOOK(__imp__XamSessionRefObjByHandle, XamSessionRefObjByHandle);
REX_HOOK(__imp__XMsgStartIORequest, XMsgStartIORequest);
REX_HOOK(__imp__XMsgInProcessCall, XMsgInProcessCall);
REX_HOOK(__imp__NetDll_XNetGetTitleXnAddr, XNetGetTitleXnAddr);
REX_HOOK(__imp__NetDll_XNetXnAddrToInAddr, XNetXnAddrToInAddr);
REX_HOOK(__imp__NetDll_XNetInAddrToXnAddr, XNetInAddrToXnAddr);
REX_HOOK(__imp__NetDll_XNetConnect, XNetConnect);
REX_HOOK(__imp__NetDll_XNetGetConnectStatus, XNetGetConnectStatus);
REX_HOOK(__imp__NetDll_XNetRegisterKey, XNetRegisterKey);
REX_HOOK(__imp__NetDll_XNetReplaceKey, XNetReplaceKey);
REX_HOOK(__imp__NetDll_XNetUnregisterInAddr, XNetUnregisterInAddr);
REX_HOOK(__imp__NetDll_XNetQosListen, XNetQosListen);
REX_HOOK(__imp__NetDll_XNetQosLookup, XNetQosLookup);
REX_HOOK(__imp__NetDll_XNetQosRelease, XNetQosRelease);
