// SPDX-License-Identifier: MIT
//
// Reading a page: a headless browser when there is one, curl when not.
// See fetch.hpp.
#include "crucible/tools/fetch.hpp"

#include "crucible/kit/kit.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

#include "crucible/tools/attachments.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/subprocess.hpp"
#include "crucible/util/text.hpp"

namespace crucible::tools {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/// Where a browser is, when it is not on PATH: the places the three install.
const std::vector<std::string>& browser_places() {
    static const std::vector<std::string> places{
#if defined(__APPLE__)
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Chromium.app/Contents/MacOS/Chromium",
        "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
        "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
#elif defined(_WIN32)
        "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
        "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
        "C:\\Program Files\\Chromium\\Application\\chrome.exe",
#else
        "/usr/bin/google-chrome", "/usr/bin/google-chrome-stable", "/usr/bin/chromium",
        "/usr/bin/chromium-browser", "/usr/bin/microsoft-edge", "/snap/bin/chromium",
        "/usr/bin/brave-browser",
#endif
    };
    return places;
}

/// Run the browser headless and keep the DOM it prints.
std::string browser_dom(const std::string& browser, std::string_view url, int timeout_seconds,
                        std::string& error) {
    util::Subprocess child;
    // A profile of its own, so a run never touches the person's browser and
    // never waits on a lock it holds.
    const std::filesystem::path profile = std::filesystem::temp_directory_path() / "crucible-browser";
    std::error_code ec;
    std::filesystem::create_directories(profile, ec);
    const std::vector<std::string> argv{
        browser, "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
        "--disable-extensions", "--hide-scrollbars", "--mute-audio",
        "--user-data-dir=" + profile.string(),
        "--virtual-time-budget=6000", "--timeout=" + std::to_string(timeout_seconds * 1000),
        "--dump-dom", std::string(url)};
    if (!child.start(argv, {}, {}, error)) {
        return {};
    }
    std::atomic<bool> finished{false};
    std::atomic<bool> timed_out{false};
    std::thread watchdog([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds + 5);
        while (!finished.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out.store(true, std::memory_order_relaxed);
                child.terminate();
                return;
            }
        }
    });
    // Read until the page has ended, then stop the browser. Chrome's own
    // headless mode prints the DOM and then does not exit -- measured with
    // Chrome 154 on a Mac, 2026-10-10: the page arrived in under a second
    // and the process was still there a minute later -- so waiting for the
    // end of its output waited for the watchdog, and threw the page away.
    std::string html;
    std::string line;
    bool        ended = false;
    while (child.read_line(line)) {
        html += line;
        html += '\n';
        if (line.find("</html>") != std::string::npos || line.find("</HTML>") != std::string::npos) {
            ended = true;
            break;
        }
    }
    if (ended) {
        child.terminate();
    }
    const int status = child.wait();
    finished.store(true, std::memory_order_relaxed);
    watchdog.join();
    if (timed_out.load(std::memory_order_relaxed) && !ended) {
        error = "the browser took longer than " + std::to_string(timeout_seconds) + " seconds";
        return {};
    }
    // The browser writes its own complaints to the same stream; a page that
    // came out is a page, whatever it grumbled on the way.
    if (html.find("<html") == std::string::npos && html.find("<HTML") == std::string::npos) {
        error = status == 0 ? "the browser printed no page" : "the browser exited " + std::to_string(status);
        return {};
    }
    return html;
}

}  // namespace

std::string render(const std::string& page, const std::filesystem::path& out, int width, int height,
                   int timeout_seconds) {
    const std::string browser = browser_here();
    if (browser.empty()) {
        return "there is no browser to draw it with yet -- Crucible fetches one as it starts";
    }
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    std::filesystem::remove(out, ec);
    const std::filesystem::path profile = std::filesystem::temp_directory_path() / "crucible-render";
    std::filesystem::create_directories(profile, ec);
    const std::string ext = out.extension().string();
    std::vector<std::string> argv{browser, "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
                                  "--disable-extensions", "--hide-scrollbars", "--mute-audio",
                                  "--user-data-dir=" + profile.string(), "--virtual-time-budget=4000"};
    if (ext == ".pdf") {
        argv.push_back("--no-pdf-header-footer");
        argv.push_back("--print-to-pdf=" + out.string());
    } else {
        argv.push_back("--window-size=" + std::to_string(width) + "," + std::to_string(height));
        argv.push_back("--screenshot=" + out.string());
    }
    argv.push_back(page);

    util::Subprocess child;
    std::string error;
    if (!child.start(argv, {}, {}, error)) {
        return "the browser would not start: " + error;
    }
    // Done when the file is there and has stopped growing -- not when the
    // browser exits, which Chrome's own headless mode does not do. See
    // browser_dom.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    std::uintmax_t last = 0;
    int            still = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const std::uintmax_t size = std::filesystem::exists(out, ec) ? std::filesystem::file_size(out, ec) : 0;
        if (size > 0 && size == last) {
            if (++still >= 2) {
                break;
            }
        } else {
            still = 0;
        }
        last = size;
        if (!child.running() && size > 0) {
            break;
        }
        if (!child.running() && size == 0) {
            still = -1;
            break;
        }
    }
    if (child.running()) {
        child.terminate();
    }
    child.wait();
    if (!std::filesystem::exists(out, ec) || std::filesystem::file_size(out, ec) == 0) {
        return "the browser drew nothing" + std::string(still < 0 ? "" : " in time");
    }
    return {};
}

namespace detail {

bool fetchable(std::string_view url) {
    const std::string head = lower(url.substr(0, 8));
    return head.rfind("http://", 0) == 0 || head.rfind("https://", 0) == 0;
}

std::string title_of(std::string_view html) {
    const std::string low = lower(html.substr(0, std::min<std::size_t>(html.size(), 200000)));
    const std::size_t open = low.find("<title");
    if (open == std::string::npos) {
        return {};
    }
    const std::size_t start = low.find('>', open);
    const std::size_t end   = low.find("</title>", start);
    if (start == std::string::npos || end == std::string::npos) {
        return {};
    }
    std::string title = attach::detail::markup_text(html.substr(start + 1, end - start - 1));
    // One line of it.
    std::string out;
    bool space = false;
    for (const char c : title) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            space = true;
            continue;
        }
        if (space && !out.empty()) {
            out += ' ';
        }
        space = false;
        out += c;
    }
    return out;
}

}  // namespace detail

std::string browser_here() {
    // The kit's headless shell first, when it has one: it is made for this,
    // and it is only there because the machine had no browser of its own.
    if (const std::filesystem::path shell = kit::browser(); !shell.empty()) {
        return shell.string();
    }
    return system_browser();
}

std::string system_browser() {
    for (const char* name : {"google-chrome", "google-chrome-stable", "chromium", "chromium-browser",
                             "chrome", "msedge", "microsoft-edge", "brave-browser"}) {
        if (util::on_path(name)) {
            return name;
        }
    }
    std::error_code ec;
    for (const std::string& place : browser_places()) {
        if (std::filesystem::exists(place, ec)) {
            return place;
        }
    }
    return {};
}

Page fetch(std::string_view url, std::size_t max_chars, int timeout_seconds) {
    Page page;
    page.url = std::string(url);
    if (!detail::fetchable(url)) {
        page.error = "only http and https addresses can be fetched";
        return page;
    }
    timeout_seconds = std::clamp(timeout_seconds, 5, 120);

    std::string html;
    std::string error;
    if (const std::string browser = browser_here(); !browser.empty()) {
        html = browser_dom(browser, url, timeout_seconds, error);
        page.how = "browser";
    }
    if (html.empty()) {
        util::http::Request request;
        request.url             = std::string(url);
        request.timeout_seconds = timeout_seconds;
        request.headers         = {{"Accept", "text/html,application/xhtml+xml,text/plain,application/json;q=0.9,*/*;q=0.5"},
                                   {"User-Agent", "Mozilla/5.0 (compatible; Crucible)"}};
        const util::http::Response response = util::http::send(request);
        if (!response.ok()) {
            page.error = response.reason();
            if (!error.empty()) {
                page.error += " (and the browser: " + error + ")";
            }
            return page;
        }
        html     = response.body;
        page.how = "curl";
    }

    page.ok    = true;
    page.title = detail::title_of(html);
    const std::string low = lower(html.substr(0, 2000));
    const bool markup = low.find("<html") != std::string::npos || low.find("<!doctype") != std::string::npos
                     || low.find("<body") != std::string::npos || low.find("<div") != std::string::npos
                     || low.find("<p") != std::string::npos;
    page.text = markup ? attach::detail::markup_text(html) : html;
    page.text = crucible::detail::scrub_utf8(page.text);
    if (page.text.size() > max_chars) {
        page.text.resize(max_chars);
        page.text += "\n... (the page goes on; this is the first " + std::to_string(max_chars) + " characters)";
    }
    return page;
}

}  // namespace crucible::tools
