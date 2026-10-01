#include "ui/platform_win32.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <objbase.h>
#include <shellapi.h>

#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "core/i18n.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/settings.h"
#include "core/strings.h"
#include "ui/app.h"
#include "ui/main_window.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace avc::ui {

namespace {

constexpr UINT WM_APP_WAKE = WM_APP + 1;

template <typename T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct Host {
    HWND hwnd = nullptr;
    gpu::Device* device = nullptr;
    ID3D11Device* d3d = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    UINT pendingW = 0, pendingH = 0;
    bool occluded = false;
    App* app = nullptr;
    MainWindow* window = nullptr;
    bool viewports = false;
    bool inputThisFrame = false;
};

Host* g_host = nullptr;

void createRenderTarget(Host& h) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(h.swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        h.d3d->CreateRenderTargetView(back, nullptr, &h.rtv);
        back->Release();
    }
}

bool createSwapChain(Host& h) {
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    if (FAILED(h.d3d->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;
    dxgiDevice->GetAdapter(&adapter);
    if (adapter) adapter->GetParent(IID_PPV_ARGS(&factory));
    bool ok = false;
    if (factory) {
        IDXGIFactory2* f2 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f2))) && f2) {
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            IDXGISwapChain1* sc1 = nullptr;
            if (SUCCEEDED(f2->CreateSwapChainForHwnd(h.d3d, h.hwnd, &sd, nullptr, nullptr, &sc1)) && sc1) {
                h.swap = sc1;
                ok = true;
            }
            f2->Release();
        }
        if (!ok) {
            // Older systems / translation layers: bitblt model.
            DXGI_SWAP_CHAIN_DESC sd{};
            sd.BufferCount = 2;
            sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = h.hwnd;
            sd.SampleDesc.Count = 1;
            sd.Windowed = TRUE;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            ok = SUCCEEDED(factory->CreateSwapChain(h.d3d, &sd, &h.swap)) && h.swap;
        }
        factory->MakeWindowAssociation(h.hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
    safeRelease(factory);
    safeRelease(adapter);
    safeRelease(dxgiDevice);
    if (ok) createRenderTarget(h);
    return ok;
}

ImVec2 dropPoint(HWND hwnd, HDROP drop, bool viewports) {
    POINT pt{};
    DragQueryPoint(drop, &pt);
    if (viewports) ClientToScreen(hwnd, &pt);  // multi-viewport ImGui works in screen coordinates
    return ImVec2(static_cast<float>(pt.x), static_cast<float>(pt.y));
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        if (g_host) g_host->inputThisFrame = true;
        return 1;
    }
    Host* h = g_host;
    switch (msg) {
    case WM_SIZE:
        if (h && wp != SIZE_MINIMIZED) {
            h->pendingW = LOWORD(lp);
            h->pendingH = HIWORD(lp);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = 960;
        mmi->ptMinTrackSize.y = 600;
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // Alt opens the ImGui menu, not the system one
        break;
    case WM_CLOSE:
        if (h && h->app) {
            h->app->requestExit();
            h->inputThisFrame = true;
            return 0;
        }
        break;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        std::vector<std::string> paths;
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring w(len + 1, L'\0');
            DragQueryFileW(drop, i, w.data(), len + 1);
            w.resize(len);
            paths.push_back(pathToUtf8(std::filesystem::path(w)));
        }
        const ImVec2 pos = dropPoint(hwnd, drop, h && h->viewports);
        DragFinish(drop);
        if (h && h->window) h->window->onFilesDropped(paths, pos);
        if (h) h->inputThisFrame = true;
        return 0;
    }
    case WM_APP_WAKE:
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool saveBackbufferPng(Host& h, const std::string& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(h.swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = false;
    if (SUCCEEDED(h.d3d->CreateTexture2D(&d, nullptr, &staging)) && staging) {
        h.ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(h.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            std::vector<uint8_t> rgba(static_cast<size_t>(d.Width) * d.Height * 4);
            for (UINT y = 0; y < d.Height; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                uint8_t* dst = rgba.data() + static_cast<size_t>(y) * d.Width * 4;
                for (UINT x = 0; x < d.Width; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 255;
                }
            }
            h.ctx->Unmap(staging, 0);
            std::string png;
            stbi_write_png_to_func([](void* c, void* data, int size) { static_cast<std::string*>(c)->append(static_cast<char*>(data), size); },
                                   &png, static_cast<int>(d.Width), static_cast<int>(d.Height), 4, rgba.data(), static_cast<int>(d.Width) * 4);
            if (FILE* f = openFileUtf8(pathFromUtf8(path), "wb")) {
                ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
                std::fclose(f);
            }
        }
        staging->Release();
    }
    back->Release();
    return ok;
}

std::unique_ptr<gpu::Device> createRenderDevice(const AppSettings& s, bool safeMode, const std::filesystem::path& cacheRoot) {
    gpu::D3D11DeviceOptions o;
    o.warp = safeMode || s.gpu.api == "warp";
    o.adapterName = s.gpu.adapter;
    o.shaderCacheDir = pathToUtf8(cacheRoot / "shaders");
    std::string err;
    auto dev = gpu::createD3D11Device(o, &err);
    if (!dev && !o.warp) {
        AVC_WARN("app", "Direct3D 11 hardware device failed ({}); falling back to WARP", err);
        o.warp = true;
        dev = gpu::createD3D11Device(o, &err);
    }
    if (!dev) AVC_FATAL("app", "no Direct3D 11 device: {}", err);
    return dev;
}

}  // namespace

int runStudio(const StudioLaunch& launch, const FrameDriver& driver) {
    ImGui_ImplWin32_EnableDpiAwareness();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    const std::filesystem::path dataDir = launch.dataDir.empty() ? appDataDir() : launch.dataDir;
    AppSettings early = AppSettings::load(dataDir / "settings.json");
    if (launch.safeMode) {
        early.performance.hardwareDecode = false;
        early.gpu.api = "warp";
    }
    const std::filesystem::path cacheRoot = early.cache.location.empty() ? dataDir / "Cache" : pathFromUtf8(early.cache.location);
    std::unique_ptr<gpu::Device> device = createRenderDevice(early, launch.safeMode, cacheRoot);
    if (!device) {
        MessageBoxW(nullptr, L"AviCap Studio could not initialise Direct3D 11 (not even the WARP software renderer).\n"
                             L"Please update your graphics driver. Details are in the log folder.",
                    L"AviCap Studio", MB_ICONERROR | MB_OK);
        if (SUCCEEDED(com)) CoUninitialize();
        return 2;
    }

    // ---- window
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AviCapStudioWindow";
    RegisterClassExW(&wc);
    const auto& ui = early.ui;
    const int w = ui.windowW > 0 ? ui.windowW : 1600, hgt = ui.windowH > 0 ? ui.windowH : 900;
    const int x = ui.windowX >= 0 ? ui.windowX : CW_USEDEFAULT, y = ui.windowY >= 0 ? ui.windowY : CW_USEDEFAULT;
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"AviCap Studio", WS_OVERLAPPEDWINDOW, x, y, w, hgt, nullptr,
                                nullptr, inst, nullptr);
    if (!hwnd) {
        AVC_FATAL("app", "CreateWindow failed ({})", static_cast<unsigned long>(GetLastError()));
        return 2;
    }

    Host host;
    g_host = &host;
    host.hwnd = hwnd;
    host.device = device.get();
    host.d3d = static_cast<ID3D11Device*>(device->nativeDevice());
    host.d3d->GetImmediateContext(&host.ctx);
    if (!createSwapChain(host)) {
        AVC_FATAL("app", "swap chain creation failed");
        MessageBoxW(hwnd, L"Could not create the display swap chain.", L"AviCap Studio", MB_ICONERROR | MB_OK);
        DestroyWindow(hwnd);
        return 2;
    }
    ShowWindow(hwnd, ui.windowMaximized && !driver ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    DragAcceptFiles(hwnd, TRUE);

    // ---- application
    AppOptions opt;
    opt.dataDir = dataDir;
    opt.device = std::move(device);
    opt.dialogs = createWin32Dialogs(hwnd);
    opt.workerExecutable = launch.workerExecutable;
    auto app = std::make_unique<App>(std::move(opt));
    host.app = app.get();
    if (launch.safeMode) {
        app->settings().performance.hardwareDecode = false;
        app->ui().resetLayout = true;
    }
    app->setWakeCallback([hwnd] { PostMessageW(hwnd, WM_APP_WAKE, 0, 0); });

    // ---- Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static std::string iniPath;
    iniPath = pathToUtf8(dataDir / "layout.ini");
    io.IniFilename = iniPath.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    host.viewports = app->settings().ui.multiViewports && !driver && !getEnv("AVICAP_NO_VIEWPORTS");
    if (host.viewports) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(host.d3d, host.ctx);
    loadUiFonts();
    float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    auto uiScale = [&] { return (app->settings().ui.uiScale > 0 ? app->settings().ui.uiScale : 1.0f) * dpi; };
    float appliedScale = uiScale();
    bool appliedContrast = app->settings().ui.highContrast;
    applyTheme(appliedScale, appliedContrast);
    if (host.viewports) {
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = 0.0f;
        st.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    MainWindow window(*app);
    host.window = &window;
    if (launch.previousSessionCrashed)
        app->notify(LogLevel::Warning, tr("AviCap Studio did not close normally last time. Unsaved work can be recovered from autosave."),
                    tr("Open Crash Folder"), [] { openWithShell(pathToUtf8(logsDir() / "CrashDumps")); });
    for (const auto& f : launch.openFiles) {
        if (pathFromUtf8(f).extension() == ".avicap") app->openProject(f);
        else app->importFiles({f});
    }
    if (launch.openFiles.empty() && app->settings().general.reopenLastProject && !app->settings().general.recentProjects.empty() &&
        app->recoveryCandidates().empty())
        app->openProject(app->settings().general.recentProjects.front());

    // ---- frame loop
    FrameControl control;
    int idleFrames = 0;
    std::string lastTitle;
    bool running = true;
    while (running) {
        const bool busy = app->animating() || driver;
        if (!busy && idleFrames > 6) {
            const DWORD timeout = io.WantTextInput ? 33 : 250;
            MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        host.inputThisFrame = false;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
            if (msg.message >= WM_KEYFIRST && msg.message <= WM_MOUSELAST) host.inputThisFrame = true;
        }
        if (!running) break;
        idleFrames = host.inputThisFrame ? 0 : idleFrames + 1;

        if (host.pendingW > 0 && host.pendingH > 0) {
            host.device->lock();
            safeRelease(host.rtv);
            host.swap->ResizeBuffers(0, host.pendingW, host.pendingH, DXGI_FORMAT_UNKNOWN, 0);
            createRenderTarget(host);
            host.device->unlock();
            host.pendingW = host.pendingH = 0;
        }
        if (host.occluded && host.swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            app->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            continue;
        }
        host.occluded = false;
        if (IsIconic(hwnd)) {
            app->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // Theme follows settings / DPI changes.
        dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
        if (std::abs(uiScale() - appliedScale) > 0.01f || appliedContrast != app->settings().ui.highContrast) {
            appliedScale = uiScale();
            appliedContrast = app->settings().ui.highContrast;
            applyTheme(appliedScale, appliedContrast);
        }

        app->tick();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        window.frame();
        if (driver) driver(*app, window, control);
        ImGui::Render();

        host.device->lock();
        const float clear[4] = {0.08f, 0.08f, 0.09f, 1.0f};
        host.ctx->OMSetRenderTargets(1, &host.rtv, nullptr);
        host.ctx->ClearRenderTargetView(host.rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!control.screenshotPath.empty()) {
            const bool ok = saveBackbufferPng(host, control.screenshotPath);
            AVC_INFO("app", "screenshot {} -> {}", ok ? "saved" : "FAILED", control.screenshotPath);
            control.screenshotPath.clear();
        }
        host.device->unlock();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            host.device->lock();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            host.device->unlock();
        }
        const HRESULT pr = host.swap->Present(1, 0);
        if (pr == DXGI_STATUS_OCCLUDED) host.occluded = true;
        if (pr == DXGI_ERROR_DEVICE_REMOVED || pr == DXGI_ERROR_DEVICE_RESET) {
            AVC_FATAL("app", "GPU device lost (0x{:08X}, reason 0x{:08X})", static_cast<unsigned>(pr),
                      static_cast<unsigned>(host.d3d->GetDeviceRemovedReason()));
            app->autosave().flush();
            MessageBoxW(hwnd, L"The graphics device was lost (driver reset). Your edits are autosaved and will be offered for recovery "
                              L"when AviCap Studio starts again.",
                        L"AviCap Studio", MB_ICONERROR | MB_OK);
            control.exit = true;
            control.exitCode = 3;
        }

        const std::string title = app->windowTitle();
        if (title != lastTitle) {
            SetWindowTextW(hwnd, utf8ToWide(title).c_str());
            lastTitle = title;
        }
        if (app->ui().exitConfirmed || control.exit) running = false;
    }

    // ---- shutdown
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (GetWindowPlacement(hwnd, &wp)) {
        auto& s = app->settings().ui;
        s.windowMaximized = wp.showCmd == SW_SHOWMAXIMIZED;
        s.windowX = wp.rcNormalPosition.left;
        s.windowY = wp.rcNormalPosition.top;
        s.windowW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        s.windowH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    }
    app->saveSettings();
    app->prepareExit();
    ImGui::SaveIniSettingsToDisk(io.IniFilename);
    host.window = nullptr;
    host.device->lock();
    ImGui_ImplDX11_Shutdown();
    host.device->unlock();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    safeRelease(host.rtv);
    safeRelease(host.swap);
    safeRelease(host.ctx);
    host.app = nullptr;
    app.reset();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    g_host = nullptr;
    if (SUCCEEDED(com)) CoUninitialize();
    return control.exitCode;
}

}  // namespace avc::ui
