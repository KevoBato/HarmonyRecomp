// harmonyrecomp netplay - the game's own UDP sockets (see gamesock.cpp).
#pragma once

#include <cstdint>

namespace harmony::netplay::gamesock {

// Last error set by a hook called from a non-guest thread (tests).
uint32_t HostLastError();

// Stops the receive poll thread (app shutdown).
void Shutdown();

}  // namespace harmony::netplay::gamesock
