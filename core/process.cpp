#include "core/process.h"

#include <algorithm>
#include <thread>

#include "core/strings.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace avc {

#if defined(_WIN32)
namespace {
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    int backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') out.append(static_cast<size_t>(backslashes * 2 + 1), L'\\');
        else out.append(static_cast<size_t>(backslashes), L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(static_cast<size_t>(backslashes * 2), L'\\');
    out.push_back(L'"');
    return out;
}
}  // namespace

Result<ProcessResult> runProcess(const std::string& exe, const std::vector<std::string>& args,
                                 const std::function<void(const std::string&)>& onLine, const CancelToken& cancel) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return Result<ProcessResult>::error("CreatePipe failed");
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = quoteArg(utf8ToWide(exe));
    for (const auto& a : args) cmd += L" " + quoteArg(utf8ToWide(a));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                   nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return Result<ProcessResult>::error("CreateProcess failed for " + exe);
    }
    ProcessResult res;
    std::string pending;
    char buf[4096];
    for (;;) {
        if (cancel.cancelled()) {
            TerminateProcess(pi.hProcess, 1);
            res.cancelled = true;
            break;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr)) break;  // closed
        if (avail == 0) {
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
                if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
            }
            continue;
        }
        DWORD n = 0;
        if (!ReadFile(readPipe, buf, std::min<DWORD>(avail, sizeof(buf)), &n, nullptr) || n == 0) break;
        pending.append(buf, n);
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (onLine) onLine(line);
            pending.erase(0, pos + 1);
        }
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exitCode = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readPipe);
    if (!pending.empty() && onLine) onLine(pending);
    return res;
}
#else
Result<ProcessResult> runProcess(const std::string& exe, const std::vector<std::string>& args,
                                 const std::function<void(const std::string&)>& onLine, const CancelToken& cancel) {
    int fds[2];
    if (pipe(fds) != 0) return Result<ProcessResult>::error("pipe failed");
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return Result<ProcessResult>::error("spawn failed for " + exe);
    }
    ProcessResult res;
    std::string pending;
    char buf[4096];
    for (;;) {
        if (cancel.cancelled()) {
            kill(pid, SIGKILL);
            res.cancelled = true;
            break;
        }
        pollfd p{fds[0], POLLIN, 0};
        const int pr = poll(&p, 1, 50);
        if (pr <= 0) continue;
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0) break;
        pending.append(buf, static_cast<size_t>(n));
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            if (onLine) onLine(pending.substr(0, pos));
            pending.erase(0, pos + 1);
        }
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    res.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (!pending.empty() && onLine) onLine(pending);
    return res;
}
#endif

}  // namespace avc
