// SPDX-License-Identifier: MIT
#include "crucible/util/http.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <fstream>
#include <mutex>
#include <random>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#  include <stdlib.h>   // mkdtemp
#endif

#include "crucible/util/subprocess.hpp"

namespace crucible::util::http {
namespace {

/// What curl is told to print after every response, with the status in it.
///
/// The status has to come back somehow and the exit code cannot carry it:
/// `--fail` turns a 401 into "exit 22" and throws away the body, and the body
/// of a 401 from an API is the sentence that says which key was wrong. So the
/// response is allowed through whatever its status, and the status follows it
/// on a line nothing else would ever print.
constexpr std::string_view kMarker = "[[crucible-http-status ";

/// A directory only this user can read, for the files curl is pointed at.
std::filesystem::path private_directory(std::string& error) {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) {
        error = "there is no temporary directory to write a request to";
        return {};
    }
#if defined(_WIN32)
    // %TEMP% is already per-user on Windows, so what is made inside it is too.
    std::random_device seed;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::filesystem::path dir = base / ("crucible-http-" + std::to_string(seed()));
        if (std::filesystem::create_directory(dir, ec) && !ec) {
            return dir;
        }
    }
    error = "a temporary directory could not be created";
    return {};
#else
    // mkdtemp, not create_directory: it makes the directory 0700 in the same
    // step as choosing its name, so there is no moment at which it exists and
    // somebody else could have opened it.
    std::string pattern = (base / "crucible-http-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
        error = "a temporary directory could not be created";
        return {};
    }
    return pattern;
#endif
}

/// `value` as a quoted string in curl's configuration syntax.
std::string config_string(std::string_view value) {
    std::string out = "\"";
    for (const char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\t': out += "\\t";  break;
            // A line break inside a URL or a header is never something a
            // caller meant, and in a header it is how one request becomes two.
            // Dropped rather than escaped.
            case '\n':
            case '\r': break;
            default:   out += ch;     break;
        }
    }
    return out + '"';
}

}  // namespace

namespace detail {

std::string curl_config(const Request& request, const std::filesystem::path& body_file,
                        bool streaming) {
    std::string config;
    const auto line = [&config](std::string_view text) {
        config += text;
        config += '\n';
    };

    line("silent");
    line("show-error");
    line("location");
    if (streaming) {
        // Without this curl holds output back until it has a buffer's worth,
        // and a model's answer arrives in bursts of a paragraph.
        line("no-buffer");
    }
    line("request = " + config_string(request.method.empty() ? "GET" : request.method));
    line("url = " + config_string(request.url));
    line("user-agent = " + config_string(std::string("Crucible/") + CRUCIBLE_VERSION));
    line("connect-timeout = " + std::to_string(std::max(request.connect_seconds, 1)));
    if (request.timeout_seconds > 0) {
        line("max-time = " + std::to_string(request.timeout_seconds));
    }
    for (const Header& header : request.headers) {
        line("header = " + config_string(header.name + ": " + header.value));
    }
    if (!body_file.empty()) {
        // data-binary, not data: the plain form strips newlines out of what it
        // sends, which is a silent edit to a JSON document.
        line("data-binary = " + config_string("@" + body_file.string()));
    }
    line("write-out = \"\\n" + std::string(kMarker) + "%{http_code}]]\\n\"");
    return config;
}

int status_from_marker(std::string_view line) {
    if (line.substr(0, kMarker.size()) != kMarker) {
        return -1;
    }
    line.remove_prefix(kMarker.size());
    int status = -1;
    const auto parsed = std::from_chars(line.data(), line.data() + line.size(), status);
    if (parsed.ec != std::errc{} || line.substr(
            static_cast<std::size_t>(parsed.ptr - line.data())) != "]]") {
        return -1;
    }
    return status;
}

}  // namespace detail

std::string Response::reason() const {
    if (!error.empty()) {
        return error;
    }
    std::string out = "HTTP " + std::to_string(status);
    // The first line of what the server said, which for an API is the reason.
    const std::size_t end = body.find('\n');
    const std::string first = body.substr(0, std::min<std::size_t>(end, 300));
    if (!first.empty()) {
        out += ": " + first;
    }
    return out;
}

bool available(std::string& why_not) {
    if (on_path("curl")) {
        return true;
    }
    why_not = "curl is needed to reach the network and is not installed";
    return false;
}

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------

struct Stream::Impl {
    Subprocess            child;
    std::filesystem::path directory;   ///< holds the config and the body; removed at the end

    /// Guards the process handle between abort() on one thread and finish()
    /// on another. Not held while reading: the reader blocks for as long as
    /// the server takes, and abort() has to get in during exactly that.
    std::mutex mutex;
    bool       started = false;
    bool       reaped  = false;
    bool       aborted = false;

    int         status = 0;
    std::string transport;   ///< curl's own complaint, when it made one

    ~Impl() {
        std::error_code ec;
        if (!directory.empty()) {
            std::filesystem::remove_all(directory, ec);
        }
    }
};

// --- every request in flight ------------------------------------------------
//
// Kept so that shut_down can reach them. A list rather than anything cleverer:
// there are never more than a handful, and each is on it for seconds.

namespace {

std::mutex           g_flying_mutex;
std::vector<Stream*> g_flying;
std::atomic<bool>    g_closed{false};

void took_off(Stream* stream) {
    const std::lock_guard<std::mutex> lock(g_flying_mutex);
    g_flying.push_back(stream);
}

void landed(Stream* stream) {
    const std::lock_guard<std::mutex> lock(g_flying_mutex);
    g_flying.erase(std::remove(g_flying.begin(), g_flying.end(), stream), g_flying.end());
}

}  // namespace

void shut_down() {
    g_closed.store(true);
    const std::lock_guard<std::mutex> lock(g_flying_mutex);
    for (Stream* stream : g_flying) {
        stream->abort();
    }
}

Stream::Stream() : impl_(std::make_unique<Impl>()) {}

Stream::~Stream() {
    if (impl_->started && !impl_->reaped) {
        abort();
        finish();
    }
    landed(this);
}

bool Stream::open(const Request& request, std::string& error) {
    if (g_closed.load()) {
        error = "Crucible is closing";
        return false;
    }
    if (!available(error)) {
        return false;
    }
    impl_->directory = private_directory(error);
    if (impl_->directory.empty()) {
        return false;
    }

    std::filesystem::path body_file;
    if (!request.body.empty()) {
        body_file = impl_->directory / "body";
        std::ofstream out(body_file, std::ios::binary);
        out << request.body;
        if (!out.good()) {
            error = "the request could not be written to " + body_file.string();
            return false;
        }
    }
    const std::filesystem::path config_file = impl_->directory / "request";
    {
        std::ofstream out(config_file, std::ios::binary);
        out << detail::curl_config(request, body_file, /*streaming=*/true);
        if (!out.good()) {
            error = "the request could not be written to " + config_file.string();
            return false;
        }
    }

    if (!impl_->child.start({"curl", "--config", config_file.string()}, {}, {}, error)) {
        return false;
    }
    impl_->started = true;
    took_off(this);
    if (g_closed.load()) {
        // Closed between the check at the top and here. Nobody is going to
        // read the answer, so it is stopped rather than left to run out.
        abort();
    }
    return true;
}

bool Stream::read_line(std::string& line) {
    if (!impl_->started) {
        return false;
    }
    while (impl_->child.read_line(line)) {
        if (const int status = detail::status_from_marker(line); status >= 0) {
            impl_->status = status;
            continue;
        }
        // stdout and stderr arrive as one stream, so curl's own error is a
        // line of it. Kept aside rather than handed out as part of the body.
        if (line.rfind("curl: (", 0) == 0) {
            impl_->transport = line;
            continue;
        }
        return true;
    }
    return false;
}

void Stream::abort() {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->aborted = true;
    if (impl_->started && !impl_->reaped) {
        impl_->child.interrupt();
    }
}

Response Stream::finish() {
    Response response;
    if (!impl_->started) {
        response.error = "the request was never started";
        return response;
    }
    int exit_code = 0;
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        exit_code     = impl_->child.wait();
        impl_->reaped = true;
    }
    response.status = impl_->status;
    if (impl_->aborted) {
        response.error = "stopped";
    } else if (exit_code != 0) {
        response.error = impl_->transport.empty()
                             ? "the request failed (curl exited " + std::to_string(exit_code) + ")"
                             : impl_->transport;
    }
    return response;
}

// ---------------------------------------------------------------------------
// The two that wait
// ---------------------------------------------------------------------------

Response send(const Request& request) {
    Stream      stream;
    std::string error;
    if (!stream.open(request, error)) {
        Response failed;
        failed.error = error;
        return failed;
    }
    std::string body;
    std::string line;
    while (stream.read_line(line)) {
        body += line;
        body += '\n';
    }
    Response response = stream.finish();
    response.body     = std::move(body);
    return response;
}

bool download(const std::string& url, const std::filesystem::path& to,
              int timeout_seconds, std::string& error) {
    if (!available(error)) {
        return false;
    }
    // On the command line, unlike everything above: a download is a public
    // URL and a file name, and there is nothing in either to hide.
    Subprocess child;
    if (!child.start({"curl", "--location", "--fail", "--silent", "--show-error",
                      "--retry", "2", "--retry-delay", "2",
                      "--max-time", std::to_string(std::max(timeout_seconds, 1)),
                      "--output", to.string(), url},
                     {}, {}, error)) {
        return false;
    }
    std::string last;
    std::string line;
    while (child.read_line(line)) {
        if (!line.empty()) {
            last = line;
        }
    }
    if (const int status = child.wait(); status != 0) {
        error = last.empty() ? "the download failed (curl exited " + std::to_string(status) + ")"
                             : last;
        return false;
    }
    return true;
}

}  // namespace crucible::util::http
