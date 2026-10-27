// harmonyrecomp netplay - Xbox 360 structures as the game sees them.
//
// Big-endian guest layouts for XNet addresses, Live sessions, QoS results and
// the XGI request buffers the game's xonline library sends to XAM. Sizes are
// checked against the Xbox 360 SDK layouts (as also used by Xenia).

#pragma once

#include <cstdint>

#include <rex/types.h>

namespace harmony::netplay::guest {

using rex::be;

struct XNKID {
  uint8_t ab[8];
};
static_assert(sizeof(XNKID) == 0x8);

struct XNKEY {
  uint8_t ab[16];
};
static_assert(sizeof(XNKEY) == 0x10);

#pragma pack(push, 4)
// Security gateway address (part of XNADDR).
struct SGADDR {
  be<uint32_t> ina;
  be<uint32_t> security_parameter_index;
  be<uint64_t> xbox_id;
  uint8_t platform_type;
  uint8_t reserved[3];
};
#pragma pack(pop)
static_assert(sizeof(SGADDR) == 0x14);

struct XNADDR {
  be<uint32_t> ina;        // local IP (network order in memory)
  be<uint32_t> ina_online; // public IP
  be<uint16_t> port_online;
  uint8_t ab_enet[6];      // MAC address
  SGADDR ab_online;
};
static_assert(sizeof(XNADDR) == 0x24);

struct XSESSION_INFO {
  XNKID session_id;
  XNADDR host_address;
  XNKEY key_exchange_key;
};
static_assert(sizeof(XSESSION_INFO) == 0x3C);

struct XSESSION_SEARCHRESULT {
  XSESSION_INFO info;
  be<uint32_t> open_public_slots;
  be<uint32_t> open_private_slots;
  be<uint32_t> filled_public_slots;
  be<uint32_t> filled_private_slots;
  be<uint32_t> properties_count;
  be<uint32_t> contexts_count;
  be<uint32_t> properties_ptr;
  be<uint32_t> contexts_ptr;
};
static_assert(sizeof(XSESSION_SEARCHRESULT) == 0x5C);

struct XSESSION_SEARCHRESULT_HEADER {
  be<uint32_t> count;
  be<uint32_t> results_ptr;
};
static_assert(sizeof(XSESSION_SEARCHRESULT_HEADER) == 0x8);

enum class SessionState : uint32_t { kLobby = 0, kRegistration, kInGame, kReporting, kDeleted };

struct XSESSION_LOCAL_DETAILS {
  be<uint32_t> user_index_host;
  be<uint32_t> game_type;
  be<uint32_t> game_mode;
  be<uint32_t> flags;
  be<uint32_t> max_public_slots;
  be<uint32_t> max_private_slots;
  be<uint32_t> available_public_slots;
  be<uint32_t> available_private_slots;
  be<uint32_t> actual_member_count;
  be<uint32_t> returned_member_count;
  be<uint32_t> state;
  be<uint64_t> nonce;
  XSESSION_INFO session_info;
  XNKID arbitration_id;
  be<uint32_t> members_ptr;
};
static_assert(sizeof(XSESSION_LOCAL_DETAILS) == 0x80);

struct XSESSION_MEMBER {
  be<uint64_t> online_xuid;
  be<uint32_t> user_index;
  be<uint32_t> flags;  // 1 = private slot, 2 = zombie
};
static_assert(sizeof(XSESSION_MEMBER) == 0x10);

struct XSESSION_REGISTRANT {
  be<uint64_t> machine_id;
  be<uint32_t> trustworthiness;
  be<uint32_t> num_users;
  be<uint32_t> users_ptr;
};
static_assert(sizeof(XSESSION_REGISTRANT) == 0x18);

struct XSESSION_REGISTRATION_RESULTS {
  be<uint32_t> count;
  be<uint32_t> registrants_ptr;
};
static_assert(sizeof(XSESSION_REGISTRATION_RESULTS) == 0x8);

struct XUSER_CONTEXT {
  be<uint32_t> id;
  be<uint32_t> value;
};
static_assert(sizeof(XUSER_CONTEXT) == 0x8);

// XUSER_PROPERTY: id, then an 8-byte aligned XUSER_DATA {type; union}.
struct XUSER_PROPERTY {
  be<uint32_t> id;
  uint8_t pad0[4];
  uint8_t type;
  uint8_t pad1[7];
  uint8_t data[8];  // big-endian scalar, or {be32 size, be32 ptr} for strings/binary
};
static_assert(sizeof(XUSER_PROPERTY) == 0x18);

struct XNQOSINFO {
  uint8_t flags;
  uint8_t reserved;
  be<uint16_t> probes_xmit;
  be<uint16_t> probes_recv;
  be<uint16_t> data_len;
  be<uint32_t> data_ptr;
  be<uint16_t> rtt_min_ms;
  be<uint16_t> rtt_med_ms;
  be<uint32_t> up_bits_per_sec;
  be<uint32_t> down_bits_per_sec;
};
static_assert(sizeof(XNQOSINFO) == 0x18);

struct XNQOS {
  be<uint32_t> count;
  be<uint32_t> count_pending;
  XNQOSINFO info[1];
};
static_assert(sizeof(XNQOS) == 0x20);

constexpr uint8_t kQosComplete = 0x01;
constexpr uint8_t kQosTargetContacted = 0x02;
constexpr uint8_t kQosTargetDisabled = 0x04;
constexpr uint8_t kQosDataReceived = 0x08;

// ---- XGI request buffers (app 0xFB) -------------------------------------------

struct XGI_SESSION_CREATE {
  be<uint32_t> obj_ptr;
  be<uint32_t> flags;
  be<uint32_t> num_slots_public;
  be<uint32_t> num_slots_private;
  be<uint32_t> user_index;
  be<uint32_t> session_info_ptr;
  be<uint32_t> nonce_ptr;
};
static_assert(sizeof(XGI_SESSION_CREATE) == 0x1C);

struct XGI_SESSION_STATE {
  be<uint32_t> obj_ptr;
  be<uint32_t> flags;
  be<uint64_t> nonce;
};
static_assert(sizeof(XGI_SESSION_STATE) == 0x10);

struct XGI_SESSION_MANAGE {
  be<uint32_t> obj_ptr;
  be<uint32_t> count;
  be<uint32_t> xuids_ptr;
  be<uint32_t> indices_ptr;
  be<uint32_t> private_slots_ptr;
};
static_assert(sizeof(XGI_SESSION_MANAGE) == 0x14);

struct XGI_SESSION_MODIFY {
  be<uint32_t> obj_ptr;
  be<uint32_t> flags;
  be<uint32_t> max_public_slots;
  be<uint32_t> max_private_slots;
};
static_assert(sizeof(XGI_SESSION_MODIFY) == 0x10);

struct XGI_SESSION_SEARCH {
  be<uint32_t> proc_index;
  be<uint32_t> user_index;
  be<uint32_t> num_results;
  be<uint16_t> num_props;
  be<uint16_t> num_ctx;
  be<uint32_t> props_ptr;
  be<uint32_t> ctx_ptr;
  be<uint32_t> results_buffer_size;  // a value (the game checked it already)
  be<uint32_t> results_ptr;
};
static_assert(sizeof(XGI_SESSION_SEARCH) == 0x20);

struct XGI_SESSION_SEARCH_BYID {
  be<uint32_t> user_index;
  XNKID session_id;
  be<uint32_t> results_buffer_size;
  be<uint32_t> results_ptr;
};
static_assert(sizeof(XGI_SESSION_SEARCH_BYID) == 0x14);

struct XGI_SESSION_SEARCH_BYIDS {
  be<uint32_t> user_index;
  be<uint32_t> num_ids;
  be<uint32_t> ids_ptr;
  be<uint32_t> results_buffer_size;
  be<uint32_t> results_ptr;
  be<uint32_t> reserved[3];
};
static_assert(sizeof(XGI_SESSION_SEARCH_BYIDS) == 0x20);

struct XGI_SESSION_DETAILS {
  be<uint32_t> obj_ptr;
  be<uint32_t> buffer_size_ptr;  // pointer to the size
  be<uint32_t> details_ptr;
  be<uint32_t> reserved[3];
};
static_assert(sizeof(XGI_SESSION_DETAILS) == 0x18);

struct XGI_SESSION_MIGRATE {
  be<uint32_t> obj_ptr;
  be<uint32_t> session_info_ptr;
  be<uint32_t> user_index;
  be<uint32_t> reserved[3];
};
static_assert(sizeof(XGI_SESSION_MIGRATE) == 0x18);

struct XGI_SESSION_ARBITRATION {
  be<uint32_t> obj_ptr;
  be<uint32_t> flags;
  be<uint64_t> nonce;
  be<uint32_t> duration_sec;
  be<uint32_t> results_buffer_size;
  be<uint32_t> results_ptr;
};
static_assert(sizeof(XGI_SESSION_ARBITRATION) == 0x20);

// Session flags.
constexpr uint32_t kFlagHost = 0x01;
constexpr uint32_t kFlagPresence = 0x02;
constexpr uint32_t kFlagStats = 0x04;
constexpr uint32_t kFlagMatchmaking = 0x08;
constexpr uint32_t kFlagArbitration = 0x10;
constexpr uint32_t kFlagPeerNetwork = 0x20;
constexpr uint32_t kFlagJoinInProgressDisabled = 0x0400;

// Session id type in the top nibble of XNKID byte 0.
constexpr uint8_t kXnkidOnlinePeer = 0x80;

// Standard contexts / properties.
constexpr uint32_t kContextGameType = 0x0000800A;
constexpr uint32_t kContextGameMode = 0x0000800B;
constexpr uint32_t kPropertyGamerHostname = 0x40008109;

}  // namespace harmony::netplay::guest
