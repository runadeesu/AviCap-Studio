#include "diagnostics/crash.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>

#include "core/file_io.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/strings.h"

#if defined(_WIN32)
#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>
#endif

namespace avc::diag {

namespace fs = std::filesystem;

namespace {

fs::path g_dumpDir;
bool g_writeDumps = true;
std::once_flag g_once;

#if defined(_WIN32)
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

fs::path writeDump(EXCEPTION_POINTERS* ep, const char* reason) {
    if (!g_writeDumps || g_dumpDir.empty()) return {};
    static HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp) return {};
    auto fn = reinterpret_cast<MiniDumpWriteDumpFn>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    if (!fn) return {};
    std::error_code ec;
    fs::create_directories(g_dumpDir, ec);
    SYSTEMTIME st;
    GetSystemTime(&st);
    wchar_t name[96];
    swprintf(name, 96, L"AviCapStudio_%04u%02u%02u_%02u%02u%02u_%lu.dmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, GetCurrentProcessId());
    const fs::path file = g_dumpDir / name;
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                                 MiniDumpWithUnloadedModules | MiniDumpWithDataSegs);
    const BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), h, type, ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(file.c_str());
        return {};
    }
    (void)reason;
    return file;
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(code));
    log::write(LogLevel::Fatal, "crash", std::string("Unhandled exception ") + buf);
    const fs::path dump = writeDump(ep, "exception");
    if (!dump.empty()) log::write(LogLevel::Fatal, "crash", "Crash dump written: " + pathToUtf8(dump));
    log::flush();
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void onTerminate() {
    std::string what = "std::terminate";
    if (auto ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            what += std::string(": ") + e.what();
        } catch (...) {
            what += ": unknown exception";
        }
    }
    log::write(LogLevel::Fatal, "crash", what);
    const fs::path dump = writeDumpNow(what);
    if (!dump.empty()) log::write(LogLevel::Fatal, "crash", "Crash dump written: " + pathToUtf8(dump));
    log::flush();
    std::_Exit(3);
}

void onSignal(int sig) {
    log::write(LogLevel::Fatal, "crash", "Fatal signal " + std::to_string(sig));
    writeDumpNow("signal");
    log::flush();
    std::_Exit(3);
}

}  // namespace

void installCrashHandler(const fs::path& dumpDir, bool writeDumps) {
    g_dumpDir = dumpDir;
    g_writeDumps = writeDumps;
    std::call_once(g_once, [] {
#if defined(_WIN32)
        SetUnhandledExceptionFilter(unhandledFilter);
        // Keep the Windows error dialog from blocking automated runs.
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
        std::set_terminate(onTerminate);
        std::signal(SIGABRT, onSignal);
    });
}

fs::path writeDumpNow(const std::string& reason) {
#if defined(_WIN32)
    // Capture the current context so the dump has this thread's stack.
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xE0A1CA70;  // application-defined
#if defined(_M_X64) || defined(__x86_64__)
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
#endif
    EXCEPTION_POINTERS ep{&rec, &ctx};
    return writeDump(&ep, reason.c_str());
#else
    (void)reason;
    return {};
#endif
}

SessionState beginSession(const fs::path& dataDir) {
    SessionState s;
    const fs::path marker = dataDir / "session.running";
    if (auto old = readFileBytes(marker)) {
        s.previousSessionCrashed = true;
        s.previousStartUtc = *old;
    }
    std::error_code ec;
    fs::create_directories(dataDir, ec);
    writeFileAtomic(marker, utcNowIso8601(), AtomicWriteOptions{false, false});
    return s;
}

void endSession(const fs::path& dataDir) {
    std::error_code ec;
    fs::remove(dataDir / "session.running", ec);
}

std::vector<fs::path> listCrashDumps(const fs::path& dumpDir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dumpDir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".dmp") out.push_back(it->path());
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace avc::diag
