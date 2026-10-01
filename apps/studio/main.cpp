// AviCapStudio.exe entry point.
//
//   AviCapStudio.exe [project.avicap | media files...]
//   AviCapStudio.exe --safe-mode            software renderer, no hardware decode, default layout
//   AviCapStudio.exe --data-dir <dir>       use another settings/cache/autosave folder
//   AviCapStudio.exe --self-test <dir> [--media <file>]...   automated end-to-end check

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "apps/studio/self_test.h"
#include "avicap_build_info.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/settings.h"
#include "core/strings.h"
#include "diagnostics/crash.h"
#include "ui/platform_win32.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace avc;
    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(pathToUtf8(std::filesystem::path(argvW[i])));
    LocalFree(argvW);

    ui::StudioLaunch launch;
    std::string selfTestDir;
    std::vector<std::string> selfTestMedia;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--safe-mode") launch.safeMode = true;
        else if (a == "--data-dir" && i + 1 < args.size()) launch.dataDir = pathFromUtf8(args[++i]);
        else if (a == "--self-test" && i + 1 < args.size()) selfTestDir = args[++i];
        else if (a == "--media" && i + 1 < args.size()) selfTestMedia.push_back(args[++i]);
        else if (!a.empty() && a[0] != '-') launch.openFiles.push_back(a);
    }
    if (!selfTestDir.empty() && launch.dataDir.empty()) launch.dataDir = pathFromUtf8(selfTestDir) / "data";
    // Every path helper (logs, cache, autosave) follows the chosen data folder.
    if (!launch.dataDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(launch.dataDir, ec);
        SetEnvironmentVariableW(L"AVICAP_DATA_DIR", launch.dataDir.wstring().c_str());
        _wputenv_s(L"AVICAP_DATA_DIR", launch.dataDir.wstring().c_str());
    }
    const std::filesystem::path dataDir = launch.dataDir.empty() ? appDataDir() : launch.dataDir;

    LogConfig lc;
    lc.directory = logsDir();
    lc.minLevel = LogLevel::Info;
    log::init(lc);
    AVC_INFO("app", "AviCap Studio {} ({}, {}) on {}", AVICAP_VERSION_STRING, AVICAP_BUILD_DATE, AVICAP_COMPILER, osDescription());
    const AppSettings settings = AppSettings::load(dataDir / "settings.json");
    diag::installCrashHandler(logsDir() / "CrashDumps", settings.privacy.crashDumps);
    const auto session = diag::beginSession(dataDir);
    launch.previousSessionCrashed = session.previousSessionCrashed && selfTestDir.empty();
    if (session.previousSessionCrashed) AVC_WARN("app", "previous session (started {}) did not exit cleanly", session.previousStartUtc);

    int rc = 0;
    if (!selfTestDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(pathFromUtf8(selfTestDir), ec);
        rc = ui::runStudio(launch, studio::makeSelfTest(selfTestDir, selfTestMedia));
    } else {
        rc = ui::runStudio(launch);
    }
    Jobs::shutdown();
    diag::endSession(dataDir);
    AVC_INFO("app", "exit code {}", rc);
    log::shutdown();
    return rc;
}
