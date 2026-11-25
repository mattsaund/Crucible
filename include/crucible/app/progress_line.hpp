// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include "crucible/util/platform.hpp"

namespace crucible {

/// Whether progress can be redrawn in place: somebody at a terminal, on both
/// ends. Piped, which is how the installer reads it, it is plain lines.
inline bool progress_is_interactive(bool quiet) {
    return !quiet && util::stdin_is_a_terminal() && util::stdout_is_a_terminal();
}

/// A line with its figures taken out, a size with its unit as one figure:
/// "downloading 0 B of 72.7 MB" and "downloading 4.9 MB of 72.7 MB" are the
/// same line moving on, not two things to say.
inline std::string progress_shape(const std::string& text) {
    const auto digit = [&text](std::size_t at) { return at < text.size() && text[at] >= '0' && text[at] <= '9'; };
    std::string shape;
    for (std::size_t i = 0; i < text.size();) {
        if (!digit(i)) {
            shape += text[i++];
            continue;
        }
        std::size_t end = i;
        while (digit(end) || (end < text.size() && text[end] == '.')) {
            ++end;
        }
        std::size_t unit = end;
        if (unit < text.size() && text[unit] == ' ') {
            ++unit;
        }
        if (unit < text.size() && (text[unit] == 'K' || text[unit] == 'M' || text[unit] == 'G' || text[unit] == 'T')) {
            ++unit;
        }
        if (unit < text.size() && text[unit] == 'B') {
            end = unit + 1;
        }
        shape += '#';
        i = end;
    }
    return shape;
}

/// One line of progress for the --install-* commands.
///
/// At a terminal it is one line, redrawn in place. Anywhere else, each new
/// thing is a line of its own, and a line that only moves a number
/// ("downloading 41 MB of 412 MB") is printed every couple of seconds rather
/// than every quarter: often enough for the installer's bar to follow, seldom
/// enough that a log stays a log.
class ProgressLine {
public:
    explicit ProgressLine(bool interactive) : interactive_(interactive) {}

    void show(const std::string& text) {
        if (text == last_) {
            return;
        }
        last_ = text;
        if (interactive_) {
            std::printf("\r\033[2K    %s", text.c_str());
        } else {
            std::string shape = progress_shape(text);
            const auto  now   = std::chrono::steady_clock::now();
            if (shape == shape_ && now - printed_ < std::chrono::seconds(2)) {
                return;
            }
            shape_   = std::move(shape);
            printed_ = now;
            std::printf("    %s\n", text.c_str());
        }
        // Flushed either way. Redirected to a file, stdout is block buffered,
        // and four kilobytes of progress lines is most of a download's worth
        // of silence.
        std::fflush(stdout);
    }

    void done() const {
        if (interactive_) {
            std::printf("\r\033[2K");
            std::fflush(stdout);
        }
    }

private:
    bool                                  interactive_;
    std::string                           last_;
    std::string                           shape_;
    std::chrono::steady_clock::time_point printed_{};
};

/// What a worker thread said, kept for the thread that prints it. A step can
/// be over in less than the quarter second between looks, and every download
/// is to be named, so each one is queued rather than only the latest read.
class SaidLines {
public:
    void push(std::string line) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (lines_.empty() || lines_.back() != line) {
            lines_.push_back(std::move(line));
        }
    }

    std::deque<std::string> take() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(lines_, {});
    }

private:
    std::mutex              mutex_;
    std::deque<std::string> lines_;
};

}  // namespace crucible
