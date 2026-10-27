// harmonyrecomp - runtime fixes for the recompiled game code.

#include "game_fixes.h"

#include <atomic>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

namespace harmony {
namespace {

// _purecall of the game's CRT (sub_82738630): calls the purecall handler and
// then abort(). It is only reachable through vtables.
//
// In online co-op the network library dispatches finished asynchronous
// operations from a list (sub_82789358) on its own thread. Rarely an
// operation's object is already being destroyed on the game thread at that
// point, its vtable is the abstract base, and the completion call lands here -
// which ended a co-op round after ~15 minutes with an abort. The object is
// going away anyway, so the completion is dropped instead.
constexpr uint32_t kPureCall = 0x82738630;

void PureCallGuard(PPCContext& ctx, uint8_t* base) {
  (void)base;
  static std::atomic<int> count{0};
  int n = ++count;
  if (n <= 20 || n % 1000 == 0) {
    REXLOG_WARN("[fix] pure virtual call from {:08X} (object {:08X}) ignored ({} so far)",
                static_cast<uint32_t>(ctx.lr), ctx.r3.u32, n);
  }
}

}  // namespace

void InstallGameFixes(rex::Runtime* runtime) {
  auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  if (!dispatcher) return;
  if (!dispatcher->SetFunction(kPureCall, &PureCallGuard)) {
    REXLOG_WARN("[fix] could not install the _purecall guard");
  }
}

}  // namespace harmony
