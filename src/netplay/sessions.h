// harmonyrecomp netplay - Xbox Live sessions (XGI app 0xFB) without a server.

#pragma once

#include <cstdint>
#include <functional>

namespace harmony::netplay::sessions {

// XamSessionCreateHandle / XamSessionRefObjByHandle replacements.
uint32_t CreateHandle();
uint32_t ObjectForHandle(uint32_t handle);

// Handles an XGI message for a session or the local user's matchmaking
// contexts/properties. Returns false when the message should go to the SDK's
// XGI app instead (contexts/properties are recorded here and still forwarded).
bool HandleXgi(uint32_t message, uint32_t buffer_ptr, uint32_t buffer_length,
               uint32_t overlapped_ptr, uint32_t* result);

// Session id the local player is currently in (0 = none), for XNKID outputs.
uint64_t CurrentSessionId();

// Completes an XAM request: inline without an overlapped, otherwise
// immediately (fast work) or from a background thread (network work).
// Returns what the XMsg call itself should return.
uint32_t RunRequest(uint32_t overlapped_ptr, bool background, std::function<uint32_t()> work);

}  // namespace harmony::netplay::sessions
