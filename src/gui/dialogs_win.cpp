// SPDX-License-Identifier: MIT
//
// The file dialog on Windows: IFileOpenDialog, the one Explorer uses.
//
// COM is already initialized on this thread -- WebView2 needs it and the
// webview sets it up -- so this only has to ask for the dialog.
#include "dialogs.hpp"

#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>

namespace crucible::gui::dialogs {
namespace {

std::wstring wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            out.data(), size);
    }
    return out;
}

std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    // `size` counts the terminator, which a std::string keeps for itself.
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

}  // namespace

void pick(void* window, const Request& request, std::function<void(Answer)> done) {
    Answer answer;

    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))
        || dialog == nullptr) {
        answer.supported = false;
        done(std::move(answer));
        return;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    // Real paths only: a library or a search result is a thing Explorer can
    // show and nothing here can open.
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    options |= request.folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    dialog->SetOptions(options);

    if (!request.title.empty()) {
        dialog->SetTitle(wide(request.title).c_str());
    }

    // Kept alive until Show returns: the filter spec holds pointers into them.
    std::wstring filter_name;
    std::wstring patterns;
    if (!request.folder && !request.extensions.empty()) {
        filter_name = wide(request.filter_name.empty() ? "Matching files"
                                                       : request.filter_name);
        for (const std::string& extension : request.extensions) {
            patterns += (patterns.empty() ? L"*" : L";*") + wide(extension);
        }
        const COMDLG_FILTERSPEC filters[] = {
            {filter_name.c_str(), patterns.c_str()},
            {L"All files", L"*.*"},
        };
        dialog->SetFileTypes(2, filters);
    }

    if (!request.start.empty()) {
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(wide(request.start).c_str(), nullptr,
                                                  IID_PPV_ARGS(&start)))
            && start != nullptr) {
            dialog->SetFolder(start);
            start->Release();
        }
    }

    // Modal, and it runs its own message loop while it is up, so the window
    // behind it keeps painting.
    if (SUCCEEDED(dialog->Show(static_cast<HWND>(window)))) {
        IShellItem* chosen = nullptr;
        if (SUCCEEDED(dialog->GetResult(&chosen)) && chosen != nullptr) {
            PWSTR path = nullptr;
            if (SUCCEEDED(chosen->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                answer.path = narrow(path);
                CoTaskMemFree(path);
            }
            chosen->Release();
        }
    }
    dialog->Release();
    done(std::move(answer));
}

}  // namespace crucible::gui::dialogs
