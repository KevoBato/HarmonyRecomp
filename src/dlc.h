// harmonyrecomp - DLC packages the player provides (see README, "DLC").
#pragma once

namespace rex {
class Runtime;
}

namespace harmony {

// Installs the DLC packages found in the dlc/ folder into the runtime's content
// folder, where the game's content enumeration finds them. Already installed
// packages are skipped. Call once the game executable is loaded (its title ID
// decides the content folder), before the game's main thread runs.
void InstallDlcPackages(rex::Runtime* runtime);

}  // namespace harmony
