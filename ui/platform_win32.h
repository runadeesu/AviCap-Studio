#pragma once
// Windows desktop host: Win32 window, DXGI swapchain on the shared Direct3D 11
// device, Dear ImGui (docking + multi-viewport, per-monitor DPI), OS file
// drops and the frame loop (idle-aware, vsync-paced while animating).

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace avc::ui {

class App;
class MainWindow;
class IDialogs;

std::unique_ptr<IDialogs> createWin32Dialogs(void* hwnd);

struct StudioLaunch {
    std::vector<std::string> openFiles;  // project (.avicap) or media to open at start
    std::filesystem::path dataDir;       // empty = default (or AVICAP_DATA_DIR / portable)
    bool safeMode = false;               // WARP renderer, no hardware decode, default layout
    bool previousSessionCrashed = false;
    std::string workerExecutable;
};

// Lets automation (self test) drive the app frame by frame.
struct FrameControl {
    std::string screenshotPath;  // when set, the next presented frame is saved as PNG
    bool exit = false;
    int exitCode = 0;
};
using FrameDriver = std::function<void(App&, MainWindow&, FrameControl&)>;

int runStudio(const StudioLaunch& launch, const FrameDriver& driver = {});

}  // namespace avc::ui
