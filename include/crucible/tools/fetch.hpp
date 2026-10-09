// SPDX-License-Identifier: MIT
//
// Reading a page off the web.
//
// SEARCH finds pages; this reads one. Through a browser when the machine has
// one -- Chrome, Chromium or Edge run headless render the page the way a
// person sees it, scripts and all, which is what a documentation site or a
// web app needs -- and through curl otherwise, which reads what the server
// sends. Either way the markup is reduced to its words, the way an attached
// HTML file is, so what the expert gets is the page's text and not its tags.
//
// It leaves the machine, so it is behind the same switch as search.
#pragma once

#include <string>
#include <string_view>

namespace crucible::tools {

/// What a fetch came to: the page's text, or why there is none.
struct Page {
    bool        ok = false;
    std::string url;
    std::string title;
    std::string text;
    std::string how;     ///< "chrome" or "curl": which read it
    std::string error;
};

/// The headless browser this machine has, as the path to run, or empty.
std::string browser_here();

/// Read `url`. `max_chars` caps the text; `timeout_seconds` the whole thing.
Page fetch(std::string_view url, std::size_t max_chars, int timeout_seconds);

// --- exposed for the tests -------------------------------------------------

namespace detail {

/// Whether `url` is one that may be fetched: http or https, nothing else. A
/// file: URL would read the disk through a tool meant for the web.
bool fetchable(std::string_view url);

/// The <title> of a page, or empty.
std::string title_of(std::string_view html);

}  // namespace detail

}  // namespace crucible::tools
