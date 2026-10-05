// SPDX-License-Identifier: MIT
//
// Talking to something over HTTP.
//
// Everything in Crucible that leaves the machine comes through here: the
// update check, the Huggingface browser, web search, a prebuilt runtime
// download, and a model that lives at a provider instead of on a card. There
// were five copies of the same forty lines before this file existed, and one of
// them handed an API key to the process list.
//
// It drives the curl binary rather than linking a library. curl ships with
// Windows, with macOS and with every Linux anybody installs this on, so it
// costs no build dependency and no TLS stack of our own to keep patched; and a
// child process is a thing that can be killed, which is what makes a request
// cancelable at any moment rather than at the next timeout.
//
// Nothing secret is ever on a command line. The URL, the headers and the body
// are written to a private file and curl is told to read it -- argv is visible
// to every other process on the machine, and "Authorization: Bearer ..." is the
// one string in the program that must not be.
#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::util::http {

struct Header {
    std::string name;
    std::string value;
};

/// One request. Only `url` is required.
struct Request {
    std::string         method = "GET";
    std::string         url;
    std::vector<Header> headers;

    /// Sent as the request body when it is not empty.
    std::string body;

    /// A ceiling on the whole exchange, in seconds. Zero means "as long as it
    /// takes", which is what a model streaming an answer for five minutes
    /// needs and what nothing else should ask for.
    int timeout_seconds = 30;

    /// How long to wait for the connection alone. Separate from the ceiling
    /// so a stream with no overall limit still gives up on a host that is not
    /// there.
    int connect_seconds = 15;
};

/// What came back.
///
/// `error` is about the transport: curl missing, a host that did not resolve,
/// a connection that dropped. An HTTP 401 is not an error in that sense -- the
/// server answered, `status` says what with, and `body` is whatever it sent,
/// which for an API is usually the one sentence explaining what was wrong.
struct Response {
    int         status = 0;   ///< 0 when no response arrived at all
    std::string body;
    std::string error;

    bool ok() const { return error.empty() && status >= 200 && status < 300; }

    /// Why this is not ok, in words fit to show somebody: the transport error,
    /// or the status and the start of the body.
    std::string reason() const;
};

/// True when requests can be made at all. `why_not` says what is missing.
bool available(std::string& why_not);

/// Make a request and wait for all of it.
Response send(const Request& request);

/// Fetch `url` into the file at `to`, following redirects. For the downloads
/// that are too large to hold: a prebuilt runtime is hundreds of megabytes.
bool download(const std::string& url, const std::filesystem::path& to,
              int timeout_seconds, std::string& error);

/// Stop every request in flight, and refuse to start another.
///
/// For the way out. A request can be mid-flight on a thread of its own when
/// the window is closed, and the program should not sit for the length of a
/// network timeout before it is allowed to exit. There is no way back: this is
/// called once, when nothing more will be asked.
void shut_down();

/// A response read a line at a time, as it arrives.
///
/// What a streamed answer needs: the provider sends one event per line for as
/// long as the model is talking, and the point of streaming is to show each
/// one when it lands rather than all of them at the end.
class Stream {
public:
    Stream();
    ~Stream();
    Stream(const Stream&)            = delete;
    Stream& operator=(const Stream&) = delete;

    /// Start the request. False, with `error` set, when it could not start.
    bool open(const Request& request, std::string& error);

    /// The next line of the body, without its newline. False when there are
    /// no more -- because the response ended, or because it was aborted.
    bool read_line(std::string& line);

    /// Stop the request now. Safe to call from any thread, which is the whole
    /// reason it exists: the thread reading lines is blocked waiting for the
    /// next one, and the user pressing Stop is on a different thread.
    void abort();

    /// Reap the request and report how it went. `body` is empty -- the lines
    /// were handed out by read_line -- but `status` and `error` are filled in.
    Response finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --- exposed for the tests -------------------------------------------------

namespace detail {

/// The curl configuration file for `request`, as text.
///
/// `body_file` is where the body has been written, or empty when there is
/// none. Split out because the quoting is the part that can be wrong: a header
/// value with a quote or a backslash in it must come out the other side as the
/// same bytes, and that is a thing to assert rather than to hope for.
std::string curl_config(const Request& request, const std::filesystem::path& body_file,
                        bool streaming);

/// The status out of the line curl is told to end every response with, or -1
/// when `line` is not that line.
int status_from_marker(std::string_view line);

}  // namespace detail

}  // namespace crucible::util::http
