// tw2de: when the game stops presenting frames, log what every thread of the process is doing (Windows only):
// each thread is suspended in turn and its stack walked with DbgHelp; the recompiled functions resolve to their
// sub_XXXXXXXX names from tw2de.pdb (next to the executable). Called by the heartbeat (d3d/hooks/device.cpp).
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <string>

#include <rex/logging/macros.h>

#pragma comment(lib, "dbghelp.lib")

namespace {

std::string Symbol(HANDLE process, DWORD64 address) {
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
  auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 255;
  DWORD64 displacement = 0;
  char text[400];
  if (SymFromAddr(process, address, &displacement, symbol)) {
    snprintf(text, sizeof(text), "%s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
    return text;
  }
  HMODULE module = nullptr;
  char module_name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module)) {
    GetModuleFileNameA(module, module_name, MAX_PATH);
    const char *slash = strrchr(module_name, '\\');
    snprintf(text, sizeof(text), "%s+0x%llx", slash ? slash + 1 : module_name,
             static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    return text;
  }
  snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
  return text;
}

}  // namespace

void FnxfXiiiLrDumpThreadStacks() {
  HANDLE process = GetCurrentProcess();
  static bool symbols = false;
  if (!symbols) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    symbols = SymInitialize(process, nullptr, TRUE) != FALSE;
  }
  const DWORD self = GetCurrentThreadId();
  const DWORD pid = GetCurrentProcessId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  REXLOG_WARN("[tw2de] stall: thread stacks follow (symbols {})", symbols ? "loaded" : "unavailable");
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
      continue;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                               entry.th32ThreadID);
    if (!thread)
      continue;
    if (SuspendThread(thread) == DWORD(-1)) {
      CloseHandle(thread);
      continue;
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    std::string line;
    bool guest = false;
    if (GetThreadContext(thread, &context)) {
      STACKFRAME64 frame{};
      frame.AddrPC.Offset = context.Rip;
      frame.AddrPC.Mode = AddrModeFlat;
      frame.AddrFrame.Offset = context.Rbp;
      frame.AddrFrame.Mode = AddrModeFlat;
      frame.AddrStack.Offset = context.Rsp;
      frame.AddrStack.Mode = AddrModeFlat;
      for (int depth = 0; depth < 40; ++depth) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0)
          break;
        const std::string name = Symbol(process, frame.AddrPC.Offset);
        if (name.rfind("sub_8", 0) == 0 || name.rfind("__imp__sub_8", 0) == 0)
          guest = true;
        line += "\n    " + name;
      }
    }
    ResumeThread(thread);
    CloseHandle(thread);
    // Host-only threads (drivers, the thread pool) only when they are short: the guest's are what matter.
    REXLOG_WARN("[tw2de] thread {}{}:{}", entry.th32ThreadID, guest ? " (guest code)" : "", line);
  }
  CloseHandle(snapshot);
}
#else
void FnxfXiiiLrDumpThreadStacks() {}
#endif
