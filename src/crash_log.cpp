// harmonyrecomp - crash report for fatal native exceptions.
//
// The SDK has no crash handler, so a crash just closes the window and leaves
// nothing in the log. This writes logs/crash_<time>.txt next to the exe with
// the exception, the faulting module + offset, a stack trace (module +
// offset per frame, resolved later against harmonyrecomp.map) and the guest
// registers if the faulting thread was running recompiled code.

#include "crash_log.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <string>

namespace harmony {
namespace {

std::atomic<int> g_reports{0};
wchar_t g_dir[MAX_PATH] = {};

void Describe(DWORD64 address, char* out, size_t out_size) {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &module) &&
      module) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = path;
    for (const char* p = path; *p; ++p) {
      if (*p == '\\' || *p == '/') name = p + 1;
    }
    std::snprintf(out, out_size, "%s+0x%llX", name,
                  static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
  } else {
    std::snprintf(out, out_size, "0x%llX", static_cast<unsigned long long>(address));
  }
}

bool IsFatal(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN (also abort()/__fastfail)
    case 0xC0000374:  // heap corruption
      return true;
    default:
      return false;
  }
}

void WriteReport(EXCEPTION_POINTERS* info) {
  // A few reports at most: an access violation that some code handles itself
  // could come first; the real crash is then in a later file.
  const int index = ++g_reports;
  if (index > 5) return;
  wchar_t file[MAX_PATH];
  std::time_t now = std::time(nullptr);
  std::tm tm_now{};
  localtime_s(&tm_now, &now);
  swprintf_s(file, L"%ls\\crash_%04d%02d%02d_%02d%02d%02d_%d.txt", g_dir, tm_now.tm_year + 1900,
             tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, index);
  FILE* f = nullptr;
  if (_wfopen_s(&f, file, L"w") != 0 || !f) return;

  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  CONTEXT ctx = *info->ContextRecord;
  char where[MAX_PATH + 32];
  Describe(reinterpret_cast<DWORD64>(rec->ExceptionAddress), where, sizeof(where));
  std::fprintf(f, "exception 0x%08lX at %s (thread %lu)\n", rec->ExceptionCode, where,
               GetCurrentThreadId());
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    std::fprintf(f, "  %s address 0x%llX\n",
                 rec->ExceptionInformation[0] == 0   ? "read"
                 : rec->ExceptionInformation[0] == 1 ? "write"
                                                     : "execute",
                 static_cast<unsigned long long>(rec->ExceptionInformation[1]));
  }
  std::fprintf(f,
               "rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX\n"
               "rsi=%016llX rdi=%016llX rbp=%016llX rsp=%016llX\n"
               "r8 =%016llX r9 =%016llX r10=%016llX r11=%016llX\n"
               "r12=%016llX r13=%016llX r14=%016llX r15=%016llX\n",
               ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.Rbp, ctx.Rsp, ctx.R8,
               ctx.R9, ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15);

  std::fprintf(f, "stack:\n");
  for (int frame = 0; frame < 64 && ctx.Rip; ++frame) {
    Describe(ctx.Rip, where, sizeof(where));
    std::fprintf(f, "  #%02d %s\n", frame, where);
    DWORD64 image_base = 0;
    auto* entry = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (!entry) {
      // Leaf function: return address is at [rsp].
      if (!ctx.Rsp) break;
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, entry, &ctx, &handler_data,
                     &establisher, nullptr);
  }
  std::fclose(f);
}

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* info) {
  // Registered last: the SDK's handler (first) resolves guest memory accesses
  // and continues execution, so those never reach this one.
  if (IsFatal(info->ExceptionRecord->ExceptionCode)) WriteReport(info);
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info) {
  WriteReport(info);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashLog() {
  GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
  if (wchar_t* slash = wcsrchr(g_dir, L'\\')) *slash = 0;
  wcscat_s(g_dir, L"\\logs");
  CreateDirectoryW(g_dir, nullptr);
  SetUnhandledExceptionFilter(UnhandledFilter);
  AddVectoredExceptionHandler(0, VectoredHandler);
}

}  // namespace harmony

#else

namespace harmony {
void InstallCrashLog() {}
}  // namespace harmony

#endif
