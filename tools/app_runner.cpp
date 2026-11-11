// SPDX-License-Identifier: MIT
//
// crucible-app -- a program Crucible made, running as a program of its own.
//
// Crucible makes the programs people ask it for as pages -- HTML, CSS and
// JavaScript, the one kind of program every machine runs with nothing
// installed -- and shows them in its preview while it builds them. Packaged,
// a page needs a window of its own and somewhere to keep its data, and this is
// both. A copy of this binary is the program: its page and its name sit beside
// it, in app/index.html and app/app.json -- inside Contents/Resources on a Mac
// -- and src/tools/apps.cpp is where Crucible puts the copy and tells the
// operating system about it.
//
// The page is given one thing: window.crucible.load() and
// window.crucible.save(data), the two calls Crucible's preview answers too,
// kept here as a JSON file in the person's own data folder. Not beside the
// program, which an update replaces: the budget somebody typed in last month
// is theirs, and outlives the version of the program that holds it. And
// localStorage, which a page handed over as a string has none of, stood in
// for as the preview stands in for it, and kept in a file beside that one.
//
// The page is not handed over as a string but served, from an address of
// the program's own, so that location.reload() has somewhere to go back to:
// a scheme of its own on Linux and on a Mac, and on Windows an https address
// no real site can have. See serve(). A window that will not have that is
// handed the page as a string, where a reload empties it.
//
// Nothing else of Crucible is in it -- no models, no engine -- so it is small,
// starts at once, and runs on a machine Crucible was never installed on.
//
// Run with --self-test it checks that reload on the desktop it is on, and
// exits with what it found: see SelfTest. CI runs it on all three systems,
// since no unit test can open a window.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "crucible/tools/app_self_test.hpp"
#include "crucible/tools/page_storage.hpp"
#include "webkit_env.hpp"

#include <nlohmann/json.hpp>
#include <webview/webview.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#elif defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <mach-o/dyld.h>
#include <objc/NSObjCRuntime.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <climits>
#else
#include <climits>
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

/// Where this binary is.
fs::path executable() {
#if defined(_WIN32)
    wchar_t buffer[32768];
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return length == 0 ? fs::path() : fs::path(std::wstring(buffer, length));
#elif defined(__APPLE__)
    char buffer[PATH_MAX];
    std::uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) {
        return {};
    }
    std::error_code ec;
    const fs::path found = fs::weakly_canonical(fs::path(buffer), ec);
    return ec ? fs::path(buffer) : found;
#else
    char buffer[PATH_MAX];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    return length <= 0 ? fs::path() : fs::path(std::string(buffer, static_cast<std::size_t>(length)));
#endif
}

/// The app's folder: app/ beside the binary, or Resources/app in a Mac bundle.
fs::path app_folder() {
    const fs::path here = executable().parent_path();
    std::error_code ec;
    for (const fs::path& candidate : {here / "app", here.parent_path() / "Resources" / "app"}) {
        if (fs::is_regular_file(candidate / "index.html", ec)) {
            return candidate;
        }
    }
    return here / "app";
}

std::string read_all(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return in ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>())
              : std::string();
}

/// A name a folder can have: the app's name with anything a file system
/// objects to taken out.
std::string folder_name(const std::string& name) {
    std::string out;
    for (const char c : name) {
        if (std::string_view("<>:\"/\\|?*").find(c) == std::string_view::npos
            && static_cast<unsigned char>(c) >= 32) {
            out += c;
        }
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    return out.empty() ? std::string("Crucible app") : out;
}

/// Where the person's data for this app is kept: their own data folder, the
/// one each platform has for exactly this.
fs::path data_file(const std::string& name) {
    fs::path base;
#if defined(_WIN32)
    if (const char* roaming = std::getenv("APPDATA"); roaming != nullptr && *roaming != '\0') {
        base = fs::path(roaming);
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        base = fs::path(home) / "Library" / "Application Support";
    }
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        base = fs::path(xdg);
    } else if (const char* home = std::getenv("HOME"); home != nullptr) {
        base = fs::path(home) / ".local" / "share";
    }
#endif
    if (base.empty()) {
        base = app_folder();
    }
    return base / folder_name(name) / "data.json";
}

/// The page's window.crucible: the same two calls Crucible's preview gives a
/// page, answered here from the data file.
constexpr const char* kBridge = R"JS(
window.crucible = {
  native: true,
  load: function () { return window.__crucibleLoad(); },
  save: function (data) { return window.__crucibleSave(data === undefined ? null : data); }
};
)JS";

/// `value` written to `file` whole. Written beside and renamed over, so a
/// program closed mid-save leaves the last whole copy rather than half of
/// the new one.
bool write_whole(const fs::path& file, const json& value) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path part = file;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << value.dump(2, ' ', false, json::error_handler_t::replace);
    }
    fs::rename(part, file, ec);
    return !ec;
}

/// What a page that is missing is shown instead.
std::string missing_page(const std::string& name) {
    return "<!doctype html><title>" + name + "</title><body style='font:16px sans-serif;"
           "padding:2rem'>This program's page is missing. Make it an app again from Crucible.";
}

/// The page as it is served: the app's own, with the stand-ins for its
/// storage first, holding `local` as storage.json has it and `session` as the
/// program has held it since it started, and handing what they hold to `keep`.
std::string document_of(const std::string& page, const json& local, const json& session, const char* keep) {
    const json kept{{"local", local.is_object() ? local : json::object()},
                    {"session", session.is_object() ? session : json::object()}};
    return crucible::tools::with_script_first(
        page, crucible::tools::page_storage_script(kept.dump(-1, ' ', false, json::error_handler_t::replace), keep));
}

/// Whatever the address is asked for, made into what it answers with: the
/// page for the address itself, nothing for any other.
struct Served {
    std::function<std::string()> document;
};

/// --self-test: the reload a made app depends on, checked on the desktop it
/// runs on, which no unit test can open a window on. CI runs it on Linux,
/// macOS and Windows.
///
/// The program starts as it always does, with its own page or the page it
/// shows when that is missing, and kSelfTestProbe reports each load. The
/// first one is asked to reload. The second has to be the program's page,
/// served again from the program's own address by the program itself, and
/// has to find what the first kept in all three places: see
/// crucible/tools/app_self_test.hpp. That is the reload the address exists
/// for. A page handed over as a string reloads to an empty window, and one
/// served before what it kept arrived comes back without it.
///
/// Exits 0 when all of that held, 1 when any of it did not, and 2 when the
/// second load never came. What each load found, and why it failed, is
/// printed and written to the report file named after --self-test. The data
/// it keeps is in a folder made for the run and removed after it, never in
/// the person's own.
class SelfTest {
public:
    SelfTest(fs::path report, std::string address) : report_(std::move(report)), address_(std::move(address)) {}

    /// The program answered a request for its page.
    void served() {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++served_;
    }

    /// What a load of the page found. True when it is to reload.
    bool heard(const json& said) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (over_) {
            return false;
        }
        loads_.push_back(said);
        if (said.contains("error")) {
            fail("the page's check threw: " + said["error"].dump());
            over_ = true;
            return false;
        }
        if (loads_.size() == 1) {
            return true;
        }
        for (const std::string& failure : crucible::tools::self_test_failures(loads_, served_, address_)) {
            fail(failure);
        }
        code_ = failures_.empty() ? 0 : 1;
        over_ = true;
        return false;
    }

    /// The second load never came, or the window stopped answering.
    void give_up(const std::string& why) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (over_) {
            return;
        }
        fail(why);
        code_ = 2;
        over_ = true;
    }

    bool over() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return over_;
    }

    int code() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return code_;
    }

    /// What happened, printed and written to the report.
    void write() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream out;
        out << "crucible-app self-test at " << address_ << "\n";
        for (std::size_t i = 0; i < loads_.size(); ++i) {
            out << "load " << (i + 1) << ": " << loads_[i].dump(-1, ' ', false, json::error_handler_t::replace)
                << "\n";
        }
        out << "the program served its page " << served_ << (served_ == 1 ? " time" : " times") << "\n";
        if (failures_.empty() && code_ == 0) {
            out << "passed: served from its own address both times, and the reload found what was kept\n";
        }
        for (const std::string& failure : failures_) {
            out << "failed: " << failure << "\n";
        }
        const std::string text = out.str();
        std::fputs(text.c_str(), stdout);
        std::fflush(stdout);
        if (!report_.empty()) {
            std::ofstream file(report_, std::ios::binary | std::ios::trunc);
            file << text;
        }
    }

private:
    void fail(const std::string& why) {
        failures_.push_back(why);
        code_ = 1;
    }

    mutable std::mutex       mutex_;
    fs::path                 report_;
    std::string              address_;
    std::vector<json>        loads_;
    std::vector<std::string> failures_;
    int                      served_ = 0;
    int                      code_   = 1;
    bool                     over_   = false;
};

/// A folder for one self-test's data, in the system's temporary folder, so
/// that a run never reads or writes what somebody kept with the app.
fs::path scratch_folder() {
    std::random_device random;
    std::error_code    ec;
    const fs::path     base = fs::temp_directory_path(ec);
    return (ec ? fs::path(".") : base) / ("crucible-app-self-test-" + std::to_string(random()));
}

/// The command line's arguments after the program's name, as UTF-8.
std::vector<std::string> arguments(int argc, char** argv) {
#if defined(_WIN32)
    // WinMain has no argv, and the narrow command line is in the code page
    // the manifest sets, so it is read wide and converted.
    (void)argc;
    (void)argv;
    std::vector<std::string> out;
    int       count = 0;
    LPWSTR*   wide  = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    for (int i = 1; wide != nullptr && i < count; ++i) {
        const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string one(length > 0 ? static_cast<std::size_t>(length) : 1U, '\0');
        if (length > 0) {
            ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, one.data(), length, nullptr, nullptr);
        }
        one.resize(one.size() - 1);   // its terminator
        out.push_back(std::move(one));
    }
    if (wide != nullptr) {
        ::LocalFree(wide);
    }
    return out;
#else
    return std::vector<std::string>(argv + (argc > 0 ? 1 : 0), argv + (argc > 0 ? argc : 0));
#endif
}

#if defined(WEBVIEW_GTK)
/// Where WebKitGTK is given the page, from a scheme of the program's own.
///
/// A page handed to it as a string is a document at about:blank, and a
/// location.reload() goes back to about:blank: an empty window. Asked for at
/// an address, the page is served again on a reload, holding what its storage
/// holds by then rather than what it held when the program started.
constexpr const char* kScheme  = "crucible-app";
constexpr const char* kAddress = "crucible-app://app/";

void answer(WebKitURISchemeRequest* request, const Served& served) {
    const std::string path = webkit_uri_scheme_request_get_path(request) != nullptr
                                 ? webkit_uri_scheme_request_get_path(request) : "";
    if (!crucible::tools::is_app_page(path)) {
        GError* error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "not part of this program");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    const std::string document = served.document();
    GBytes*       bytes  = g_bytes_new(document.data(), document.size());
    GInputStream* stream = g_memory_input_stream_new_from_bytes(bytes);
    // The encoding said, as a page handed over as a string was read in it.
    webkit_uri_scheme_request_finish(request, stream, static_cast<gint64>(document.size()),
                                     "text/html; charset=utf-8");
    g_object_unref(stream);
    g_bytes_unref(bytes);
}

/// The scheme's handler. It answers once whatever the page asked of the
/// program before it went is done: a binding runs in an idle callback of the
/// webview's (its dispatch), and a page that saves and reloads in one click
/// must be served what it saved. Queued behind those, at their priority.
void serve(WebKitURISchemeRequest* request, gpointer data) {
    struct Waiting {
        WebKitURISchemeRequest* request;
        const Served*           served;
    };
    g_object_ref(request);
    g_idle_add_full(
        G_PRIORITY_HIGH_IDLE,
        [](gpointer waiting) -> gboolean {
            const auto* one = static_cast<Waiting*>(waiting);
            answer(one->request, *one->served);
            return G_SOURCE_REMOVE;
        },
        new Waiting{request, static_cast<const Served*>(data)},
        [](gpointer waiting) {
            const auto* one = static_cast<Waiting*>(waiting);
            g_object_unref(one->request);
            delete one;
        });
}
#elif defined(WEBVIEW_EDGE)
/// Where WebView2 is given the page, for the same reason as WebKitGTK: an
/// https address of the program's own, whose every request WebView2 hands
/// this program to answer (WebResourceRequested), so that nothing is ever
/// fetched from it. WebView2 takes a scheme of the program's own only in the
/// options its environment is made with, which the webview makes without
/// any, and only from versions newer than the 1.0.1150.38 it is built
/// against. The name is under .example, which RFC 2606 keeps from ever being
/// a real site's.
constexpr const char*    kAddress   = "https://crucible-app.example/";
constexpr const wchar_t* kAddresses = L"https://crucible-app.example/*";

/// UTF-8 of what WebView2 says in UTF-16.
std::string narrow(const wchar_t* wide) {
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), length, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(length) - 1);   // its terminator
    return out;
}

/// What WebView2 asks whenever the program's address is requested.
class Answers final : public ICoreWebView2WebResourceRequestedEventHandler {
public:
    Answers(webview::webview& view, const Served& served, ICoreWebView2Environment* environment)
        : view_(view), served_(served), environment_(environment) {
        environment_->AddRef();
    }
    Answers(const Answers&)            = delete;
    Answers& operator=(const Answers&) = delete;

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0) {
            delete this;
        }
        return left;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID asked, void** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(asked, __uuidof(IUnknown))
            || IsEqualIID(asked, IID_ICoreWebView2WebResourceRequestedEventHandler)) {
            *out = static_cast<ICoreWebView2WebResourceRequestedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    /// Answered once whatever the page asked of the program before it went
    /// is done, as on Linux: a binding runs from the window's queue (the
    /// webview's dispatch), and a page that saves and reloads in one click
    /// must be served what it saved. Put off, and queued behind those.
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/,
                                     ICoreWebView2WebResourceRequestedEventArgs* args) override {
        if (args == nullptr) {
            return S_OK;
        }
        std::string path;
        ICoreWebView2WebResourceRequest* request = nullptr;
        if (SUCCEEDED(args->get_Request(&request)) && request != nullptr) {
            LPWSTR uri = nullptr;
            if (SUCCEEDED(request->get_Uri(&uri)) && uri != nullptr) {
                const std::string address = narrow(uri);
                path = std::string(crucible::tools::address_path(address));
                ::CoTaskMemFree(uri);
            }
            request->Release();
        }
        ICoreWebView2Deferral* deferral = nullptr;
        if (FAILED(args->GetDeferral(&deferral)) || deferral == nullptr) {
            answer(args, path);
            return S_OK;
        }
        AddRef();
        args->AddRef();
        view_.dispatch([this, args, deferral, path] {
            answer(args, path);
            deferral->Complete();
            deferral->Release();
            args->Release();
            Release();
        });
        return S_OK;
    }

private:
    ~Answers() { environment_->Release(); }

    /// The page for the address itself, nothing for any other.
    void answer(ICoreWebView2WebResourceRequestedEventArgs* args, const std::string& path) const {
        ICoreWebView2WebResourceResponse* response = nullptr;
        if (crucible::tools::is_app_page(path)) {
            const std::string document = served_.document();
            IStream* content = ::SHCreateMemStream(reinterpret_cast<const BYTE*>(document.data()),
                                                   static_cast<UINT>(document.size()));
            // The encoding said, as a page handed over as a string was read
            // in it, and kept nowhere, so that a reload asks for it again.
            if (content != nullptr) {
                environment_->CreateWebResourceResponse(
                    content, 200, L"OK", L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store",
                    &response);
                content->Release();
            }
        } else {
            environment_->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
        }
        if (response != nullptr) {
            args->put_Response(response);
            response->Release();
        }
    }

    webview::webview&         view_;
    const Served&             served_;
    ICoreWebView2Environment* environment_;
    std::atomic<ULONG>        references_{1};
};

/// The page served at kAddress from here on. False when WebView2 will not
/// have it, which leaves nothing changed that the page could reach: the
/// page is then handed over as a string.
bool serve(webview::webview& view, const Served& served) {
    const auto controller = view.browser_controller();
    if (!controller.ok() || controller.value() == nullptr) {
        return false;
    }
    ICoreWebView2* core = nullptr;
    if (FAILED(static_cast<ICoreWebView2Controller*>(controller.value())->get_CoreWebView2(&core))
        || core == nullptr) {
        return false;
    }
    // The answers are made by the environment, which ICoreWebView2_2 gives:
    // every WebView2 runtime since the end of 2020 has it.
    bool                      serving     = false;
    ICoreWebView2_2*          newer       = nullptr;
    ICoreWebView2Environment* environment = nullptr;
    if (SUCCEEDED(core->QueryInterface(IID_ICoreWebView2_2, reinterpret_cast<void**>(&newer))) && newer != nullptr
        && SUCCEEDED(newer->get_Environment(&environment)) && environment != nullptr
        && SUCCEEDED(core->AddWebResourceRequestedFilter(kAddresses, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL))) {
        auto*                  answers = new Answers(view, served, environment);
        EventRegistrationToken token{};
        serving = SUCCEEDED(core->add_WebResourceRequested(answers, &token));
        answers->Release();   // WebView2's from here, when it took it
        if (!serving) {
            core->RemoveWebResourceRequestedFilter(kAddresses, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        }
    }
    if (environment != nullptr) {
        environment->Release();
    }
    if (newer != nullptr) {
        newer->Release();
    }
    core->Release();
    return serving;
}
#elif defined(WEBVIEW_COCOA)
/// Where WKWebView is given the page, for the same reason as WebKitGTK: a
/// scheme of the program's own (WKURLSchemeHandler), spoken to through the
/// Objective-C runtime, as the webview speaks to it and as Crucible's own
/// window does (src/gui/closing_mac.cpp).
///
/// WebKit takes a scheme's handler only in the configuration a WKWebView is
/// made with, and the webview makes the two inside its constructor, where
/// nothing of this program's reaches: set_up_web_view, in webview 0.12.0. So
/// for as long as that constructor runs, and no longer, WKWebView's own
/// initWithFrame:configuration: is one that puts the handler into the
/// configuration it is handed and then does what it always does. See
/// Handing. Whether the window's web view has the handler is asked after,
/// and the page is handed over as a string when it does not.
constexpr const char* kScheme  = "crucible-app";
constexpr const char* kAddress = "crucible-app://app/";

/// objc_msgSend, called through a pointer of the type the method has.
template <typename Result, typename... Arguments>
Result send(id receiver, const char* selector, Arguments... arguments) {
    using Function = Result (*)(id, SEL, Arguments...);
    return reinterpret_cast<Function>(objc_msgSend)(receiver, sel_registerName(selector), arguments...);
}

id class_named(const char* name) {
    return reinterpret_cast<id>(objc_lookUpClass(name));
}

id ns_string(const char* text) {
    return send<id, const char*>(class_named("NSString"), "stringWithUTF8String:", text);
}

/// An autorelease pool for as long as this lives, for what an answer makes
/// outside the webview's own.
class Pool {
public:
    Pool() : pool_(send<id>(class_named("NSAutoreleasePool"), "new")) {}
    ~Pool() {
        if (pool_ != nullptr) {
            send<void>(pool_, "drain");
        }
    }
    Pool(const Pool&)            = delete;
    Pool& operator=(const Pool&) = delete;

private:
    id pool_;
};

/// NSURLErrorFileDoesNotExist, spelled as a number because the header that
/// names it is an Objective-C one.
constexpr NSInteger kNoSuchFile = -1100;

// One window per program, so one of each.
const Served*                              g_served = nullptr;
std::function<void(std::function<void()>)> g_later;              ///< the webview's queue, once there is one
std::set<id>                               g_asked;              ///< tasks started and not stopped
id                                         g_handler = nullptr;
IMP                                        g_made    = nullptr;  ///< WKWebView's own initWithFrame:configuration:
bool                                       g_handed  = false;    ///< a configuration was given the handler

/// What `task` asked for, answered: the page for the address itself,
/// nothing for any other. Read from the address as it was asked for, as on
/// Windows, rather than from NSURL's path, which is decoded and trimmed.
void answer(id task) {
    const Pool  pool;
    const id    request = send<id>(task, "request");
    const id    url     = request != nullptr ? send<id>(request, "URL") : nullptr;
    const id    address = url != nullptr ? send<id>(url, "absoluteString") : nullptr;
    const char* text    = address != nullptr ? send<const char*>(address, "UTF8String") : nullptr;
    if (url == nullptr || g_served == nullptr
        || !crucible::tools::is_app_page(crucible::tools::address_path(text != nullptr ? text : ""))) {
        send<void, id>(task, "didFailWithError:",
                       send<id, id, NSInteger, id>(class_named("NSError"), "errorWithDomain:code:userInfo:",
                                                   ns_string("NSURLErrorDomain"), kNoSuchFile, nullptr));
        return;
    }
    const std::string document = g_served->document();
    // The encoding said, as a page handed over as a string was read in it.
    const id response = send<id, id, id, NSInteger, id>(
        send<id>(class_named("NSURLResponse"), "alloc"), "initWithURL:MIMEType:expectedContentLength:textEncodingName:",
        url, ns_string("text/html"), static_cast<NSInteger>(document.size()), ns_string("utf-8"));
    send<void, id>(task, "didReceiveResponse:", response);
    send<void, id>(task, "didReceiveData:",
                   send<id, const void*, NSUInteger>(class_named("NSData"), "dataWithBytes:length:",
                                                     document.data(), static_cast<NSUInteger>(document.size())));
    send<void>(task, "didFinish");
    send<void>(response, "release");
}

/// The scheme's handler, webView:startURLSchemeTask:. It answers once
/// whatever the page asked of the program before it went is done, as on
/// Linux: a binding runs from the main queue (the webview's dispatch), and a
/// page that saves and reloads in one click must be served what it saved.
/// Queued behind those, and the task held until then.
void serve(id /*self*/, SEL /*selector*/, id /*web_view*/, id task) {
    send<id>(task, "retain");
    g_asked.insert(task);
    const std::function<void()> answering = [task] {
        // Not once WebKit has stopped it, which it holds a mistake to answer.
        if (g_asked.erase(task) != 0) {
            answer(task);
        }
        send<void>(task, "release");
    };
    if (g_later) {
        g_later(answering);
    } else {
        answering();
    }
}

/// webView:stopURLSchemeTask:, when the page has gone before its answer.
void stop(id /*self*/, SEL /*selector*/, id /*web_view*/, id task) {
    g_asked.erase(task);
}

/// The handler WebKit is given: an object of a class made here, which
/// answers the two calls WKURLSchemeHandler has.
id make_handler() {
    constexpr const char* kName = "CrucibleAppSchemeHandler";
    Class made = objc_lookUpClass(kName);
    if (made == nullptr) {
        made = objc_allocateClassPair(objc_lookUpClass("NSObject"), kName, 0);
        if (made == nullptr) {
            return nullptr;
        }
        if (Protocol* handler = objc_getProtocol("WKURLSchemeHandler"); handler != nullptr) {
            class_addProtocol(made, handler);
        }
        class_addMethod(made, sel_registerName("webView:startURLSchemeTask:"), reinterpret_cast<IMP>(&serve),
                        "v@:@@");
        class_addMethod(made, sel_registerName("webView:stopURLSchemeTask:"), reinterpret_cast<IMP>(&stop),
                        "v@:@@");
        objc_registerClassPair(made);
    }
    return send<id>(reinterpret_cast<id>(made), "new");
}

/// WKWebView's initWithFrame:configuration: while the window's web view is
/// made: the handler put into the configuration first, when the scheme is
/// not one WebKit answers itself and the configuration has no handler for
/// it, which are when WebKit would refuse one.
id made_with_handler(id self, SEL selector, CGRect frame, id configuration) {
    if (configuration != nullptr && g_handler != nullptr) {
        const id scheme = ns_string(kScheme);
        if (send<BOOL, id>(class_named("WKWebView"), "handlesURLScheme:", scheme) == NO
            && send<id, id>(configuration, "urlSchemeHandlerForURLScheme:", scheme) == nullptr) {
            send<void, id, id>(configuration, "setURLSchemeHandler:forURLScheme:", g_handler, scheme);
            g_handed = true;
        }
    }
    using Made = id (*)(id, SEL, CGRect, id);
    return reinterpret_cast<Made>(g_made)(self, selector, frame, configuration);
}

/// For as long as this lives, a WKWebView is made with the handler: see
/// above. Made just before the webview, and gone as soon as it is made.
class Handing {
public:
    Handing() {
        if (const Class web_view = objc_lookUpClass("WKWebView"); web_view != nullptr) {
            method_ = class_getInstanceMethod(web_view, sel_registerName("initWithFrame:configuration:"));
        }
        g_handler = method_ != nullptr ? make_handler() : nullptr;
        if (g_handler != nullptr) {
            g_made = method_setImplementation(method_, reinterpret_cast<IMP>(&made_with_handler));
        }
    }
    ~Handing() {
        if (g_made != nullptr) {
            method_setImplementation(method_, g_made);
        }
    }
    Handing(const Handing&)            = delete;
    Handing& operator=(const Handing&) = delete;

private:
    Method method_ = nullptr;
};

/// Whether the window's web view was made with the handler, asked of the
/// copy of its configuration it gives.
bool handed(webview::webview& view) {
    const auto web_view = view.browser_controller();
    if (!g_handed || !web_view.ok() || web_view.value() == nullptr) {
        return false;
    }
    const Pool pool;
    const id   configuration = send<id>(static_cast<id>(web_view.value()), "configuration");
    return configuration != nullptr
        && send<id, id>(configuration, "urlSchemeHandlerForURLScheme:", ns_string(kScheme)) != nullptr;
}
#endif

}  // namespace

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    const std::vector<std::string> args = arguments(0, nullptr);
#else
int main(int argc, char** argv) {
    const std::vector<std::string> args = arguments(argc, argv);
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
    crucible::gui::avoid_nvidia_dmabuf_renderer();
#endif
    // --self-test [report] [seconds]. See SelfTest.
    const bool     testing = !args.empty() && args[0] == "--self-test";
    const fs::path report  = testing && args.size() > 1 ? fs::absolute(args[1]) : fs::path();
    const double   seconds = testing && args.size() > 2 ? std::atof(args[2].c_str()) : 60.0;
    const fs::path scratch = testing ? scratch_folder() : fs::path();
#if defined(WEBVIEW_GTK) || defined(WEBVIEW_EDGE) || defined(WEBVIEW_COCOA)
    SelfTest test(report, kAddress);
#else
    SelfTest test(report, "");
#endif

    const fs::path folder = app_folder();
    const json about = json::parse(read_all(folder / "app.json"), nullptr, false);
    const std::string name = about.is_object() ? about.value("name", std::string("App")) : "App";
    const int width  = about.is_object() ? about.value("width", 1100) : 1100;
    const int height = about.is_object() ? about.value("height", 760) : 760;
    const fs::path data = testing ? scratch / "data.json" : data_file(name);

    // localStorage, stood in for (crucible/tools/page_storage.hpp) and kept in
    // a file of its own beside the data, so neither can write over the other;
    // sessionStorage held here for as long as the program is open, as a
    // browser holds it, so that a page served again on a reload finds it.
    // Made before the window, which runs what is still queued for it as it
    // closes, and so outliving it.
    const fs::path storage = data.parent_path() / "storage.json";
    json           session = json::object();
    const auto     keep    = "function (kind, items) { window.__crucibleKeep(kind, items); }";
    // Served, with the stand-ins inside the page, holding what the storage
    // holds each time it is asked for.
    Served served{[&] {
        if (testing) {
            test.served();
        }
        const std::string page = read_all(folder / "index.html");
        return page.empty() ? missing_page(name)
                            : document_of(page, json::parse(read_all(storage), nullptr, false), session, keep);
    }};

#if defined(WEBVIEW_COCOA)
    // The handler goes into the web view as the webview makes it. See Handing.
    std::optional<Handing> handing(std::in_place);
#endif
    webview::webview view(/*debug=*/false, nullptr);
#if defined(WEBVIEW_COCOA)
    handing.reset();
#endif
    view.set_title(name);
    view.set_size(width, height, WEBVIEW_HINT_NONE);

    view.bind("__crucibleLoad", [data](const std::string&) -> std::string {
        const json saved = json::parse(read_all(data), nullptr, false);
        return saved.is_discarded() ? std::string("null")
                                    : saved.dump(-1, ' ', false, json::error_handler_t::replace);
    });
    view.bind("__crucibleSave", [data](const std::string& arguments) -> std::string {
        const json given = json::parse(arguments, nullptr, false);
        return write_whole(data, given.is_array() && !given.empty() ? given[0] : json(nullptr)) ? "true"
                                                                                               : "false";
    });
    view.init(kBridge);

    view.bind("__crucibleKeep", [storage, &session](const std::string& arguments) -> std::string {
        const json given = json::parse(arguments, nullptr, false);
        if (!given.is_array() || given.size() < 2 || !given[0].is_string() || !given[1].is_object()) {
            return "false";
        }
        if (given[0] == "session") {
            session = given[1];
            return "true";
        }
        return given[0] == "local" && write_whole(storage, given[1]) ? "true" : "false";
    });

    // What each load of the page found, for --self-test, which ends once the
    // second has said or when it is not coming.
    if (testing) {
        view.bind("__crucibleSelfTest", [&test, &view](const std::string& arguments) -> std::string {
            const json given  = json::parse(arguments, nullptr, false);
            const bool reload = test.heard(given.is_array() && !given.empty() ? given[0] : json::object());
            if (test.over()) {
                view.dispatch([&view] { view.terminate(); });
            }
            return reload ? "\"reload\"" : "null";
        });
        view.init(crucible::tools::kSelfTestProbe);
    }

    // Handed over as a string, with the stand-ins put in before it as it
    // starts, holding what the storage held then. Not inside the page itself:
    // WebView2 takes a page of two megabytes at most as a string, and a page's
    // storage alone can be more.
    const auto as_string = [&] {
        const json held = json::parse(read_all(storage), nullptr, false);
        const json kept{{"local", held.is_object() ? held : json::object()}, {"session", json::object()}};
        view.init(crucible::tools::page_storage_script(kept.dump(-1, ' ', false, json::error_handler_t::replace),
                                                       keep));
        const std::string page = read_all(folder / "index.html");
        view.set_html(page.empty() ? missing_page(name) : page);
    };

#if defined(WEBVIEW_GTK)
    const auto widget = view.widget();
    if (widget.ok()) {
        webkit_web_context_register_uri_scheme(webkit_web_view_get_context(WEBKIT_WEB_VIEW(widget.value())),
                                               kScheme, serve, &served, nullptr);
        view.navigate(kAddress);
    } else {
        as_string();
    }
#elif defined(WEBVIEW_EDGE)
    if (serve(view, served)) {
        view.navigate(kAddress);
    } else {
        as_string();
    }
#elif defined(WEBVIEW_COCOA)
    if (handed(view)) {
        g_served = &served;
        g_later  = [&view](std::function<void()> work) { view.dispatch(std::move(work)); };
        view.navigate(kAddress);
    } else {
        as_string();
    }
#else
    as_string();
#endif

    // A self-test that has not ended by its deadline has failed, and ends
    // there. Asked of the window's own thread, where ending its loop has to
    // happen, and from here when that thread no longer answers.
    std::atomic<bool> ended{false};
    std::thread       deadline;
    if (testing) {
        deadline = std::thread([&] {
            const auto until = [&ended](std::chrono::steady_clock::time_point when) {
                while (!ended.load() && std::chrono::steady_clock::now() < when) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                return ended.load();
            };
            const double limit = seconds > 0.0 ? seconds : 60.0;
            if (until(std::chrono::steady_clock::now()
                      + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                          std::chrono::duration<double>(limit)))) {
                return;
            }
            test.give_up("the page did not load twice in " + std::to_string(static_cast<int>(limit)) + " seconds");
            view.dispatch([&view] { view.terminate(); });
            if (until(std::chrono::steady_clock::now() + std::chrono::seconds(10))) {
                return;
            }
            test.write();
            std::_Exit(2);
        });
    }
    view.run();
    ended = true;
    if (deadline.joinable()) {
        deadline.join();
    }
    if (testing) {
        test.give_up("the window closed before the page had loaded twice");
        test.write();
        std::error_code ec;
        fs::remove_all(scratch, ec);
        return test.code();
    }
    return 0;
}
