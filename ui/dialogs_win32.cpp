// Native Windows file dialogs (Common Item Dialog). Paths are returned as
// UTF-8 so Japanese and other non-ASCII names work end to end.

#include <windows.h>
#include <shobjidl.h>

#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/strings.h"
#include "ui/app.h"

namespace avc::ui {

namespace {

template <typename T>
struct ComRelease {
    void operator()(T* p) const {
        if (p) p->Release();
    }
};
template <typename T>
using ComPtr = std::unique_ptr<T, ComRelease<T>>;

std::string itemPath(IShellItem* item) {
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return {};
    std::string out = pathToUtf8(std::filesystem::path(p));
    CoTaskMemFree(p);
    return out;
}

struct FilterSpec {
    std::vector<std::wstring> names, specs;
    std::vector<COMDLG_FILTERSPEC> items;
    explicit FilterSpec(const std::vector<FileFilter>& filters) {
        for (const auto& f : filters) {
            names.push_back(utf8ToWide(f.name));
            specs.push_back(utf8ToWide(f.patterns));
        }
        for (size_t i = 0; i < names.size(); ++i) items.push_back({names[i].c_str(), specs[i].c_str()});
    }
};

class Win32Dialogs final : public IDialogs {
public:
    explicit Win32Dialogs(HWND owner) : owner_(owner) {}

    std::vector<std::string> openFiles(const std::string& title, const std::vector<FileFilter>& filters, bool multiple) override {
        std::vector<std::string> out;
        IFileOpenDialog* raw = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw)))) {
            AVC_ERROR("ui", "file dialog unavailable");
            return out;
        }
        ComPtr<IFileOpenDialog> dlg(raw);
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | (multiple ? FOS_ALLOWMULTISELECT : 0));
        const std::wstring wtitle = utf8ToWide(title);
        dlg->SetTitle(wtitle.c_str());
        FilterSpec spec(filters);
        if (!spec.items.empty()) dlg->SetFileTypes(static_cast<UINT>(spec.items.size()), spec.items.data());
        if (FAILED(dlg->Show(owner_))) return out;  // cancelled
        IShellItemArray* itemsRaw = nullptr;
        if (FAILED(dlg->GetResults(&itemsRaw)) || !itemsRaw) return out;
        ComPtr<IShellItemArray> items(itemsRaw);
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, &it)) && it) {
                ComPtr<IShellItem> item(it);
                if (auto p = itemPath(item.get()); !p.empty()) out.push_back(std::move(p));
            }
        }
        return out;
    }

    std::optional<std::string> saveFile(const std::string& title, const std::vector<FileFilter>& filters, const std::string& defaultName,
                                        const std::string& defaultExt) override {
        IFileSaveDialog* raw = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw)))) return std::nullopt;
        ComPtr<IFileSaveDialog> dlg(raw);
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
        const std::wstring wtitle = utf8ToWide(title), wname = utf8ToWide(defaultName), wext = utf8ToWide(defaultExt);
        dlg->SetTitle(wtitle.c_str());
        FilterSpec spec(filters);
        if (!spec.items.empty()) dlg->SetFileTypes(static_cast<UINT>(spec.items.size()), spec.items.data());
        if (!wname.empty()) dlg->SetFileName(wname.c_str());
        if (!wext.empty()) dlg->SetDefaultExtension(wext.c_str());
        if (FAILED(dlg->Show(owner_))) return std::nullopt;
        IShellItem* it = nullptr;
        if (FAILED(dlg->GetResult(&it)) || !it) return std::nullopt;
        ComPtr<IShellItem> item(it);
        std::string p = itemPath(item.get());
        if (p.empty()) return std::nullopt;
        return p;
    }

    std::optional<std::string> pickFolder(const std::string& title) override {
        IFileOpenDialog* raw = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw)))) return std::nullopt;
        ComPtr<IFileOpenDialog> dlg(raw);
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        const std::wstring wtitle = utf8ToWide(title);
        dlg->SetTitle(wtitle.c_str());
        if (FAILED(dlg->Show(owner_))) return std::nullopt;
        IShellItem* it = nullptr;
        if (FAILED(dlg->GetResult(&it)) || !it) return std::nullopt;
        ComPtr<IShellItem> item(it);
        std::string p = itemPath(item.get());
        if (p.empty()) return std::nullopt;
        return p;
    }

private:
    HWND owner_;
};

}  // namespace

std::unique_ptr<IDialogs> createWin32Dialogs(void* hwnd) { return std::make_unique<Win32Dialogs>(static_cast<HWND>(hwnd)); }

}  // namespace avc::ui
