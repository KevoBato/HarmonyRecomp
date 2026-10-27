// harmonyrecomp - runtime fixes for the recompiled game code.
#pragma once

namespace rex {
class Runtime;
}

namespace harmony {

// Replaces entries in the indirect-call table (call after the main module is
// registered, e.g. from OnPostSetup).
void InstallGameFixes(rex::Runtime* runtime);

}  // namespace harmony
