// tw2de (2026-10-25): leaving the software from HOME before the game runs (the boot menu, the shader pre-pass).
// std::exit there crashed: the SDK's crash hooks (sdk/src/ui/switch_crash_hooks.c) abort on an exit() outside the
// SDK's own shutdown, and the HOME close is still held (appletLockExit, windowed_app_context_switch.cpp). This is the
// SDK's own exit (ExitProcess, sdk/src/ui/windowed_app_main_switch.cpp): the log written out, the close let go, and
// libnx asked to close the application (__nx_applet_exit_mode 1: ISelfController::Exit) after closing its services.
// Nothing of the game exists yet (no threads but the main one), so there is nothing else to stop.
#pragma once

#include <cstdio>

#include <switch.h>

#include <rex/logging.h>

extern "C" u32 __nx_applet_exit_mode;
extern "C" NX_NORETURN void __libnx_exit(int rc);

namespace renut::nxs {

[[noreturn]] inline void ExitBeforeGame() {
  REXLOG_INFO("Switch: closed from HOME before the game started: exiting");
  rex::FlushLogging();
  std::fflush(nullptr);
  appletUnlockExit();
  __nx_applet_exit_mode = 1;
  __libnx_exit(0);
}

}  // namespace renut::nxs
