// SPDX-License-Identifier: MIT
// A deliberately tiny test harness.
//
// Crucible already fetches three dependencies; a test framework would be a
// fourth, for something that fits in fifty lines. Tests self-register, report
// every failure in a case rather than stopping at the first, and the binary
// exits non-zero if anything failed, which is all CTest needs.
#pragma once

#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace harness {

struct TestCase {
    std::string           name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int& failure_count() {
    static int failures = 0;
    return failures;
}

/// The case running now, for a crash report to name. A crash takes the process
/// with it before the harness can say anything, so the name has to be
/// somewhere a handler can read it.
inline std::string& current_test() {
    static std::string name;
    return name;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back({name, std::move(body)});
    }
};

/// A failure as GitHub Actions' own error command, for a run there.
///
/// The log of a CI run can only be read signed in; the annotations a run
/// leaves on its commit can be read by anyone. Written this way a failure
/// is one of those, naming the case, the file and line, and what was wrong.
inline void annotate(const char* file, int line, const std::string& message) {
    const auto escape = [](const std::string& text, bool property) {
        std::string out;
        for (const char c : text) {
            if (c == '%') out += "%25";
            else if (c == '\r') out += "%0D";
            else if (c == '\n') out += "%0A";
            else if (property && c == ':') out += "%3A";
            else if (property && c == ',') out += "%2C";
            else out += c;
        }
        return out;
    };
    // From the checkout down, which is how GitHub names a file.
    std::string path = file;
    for (const char* tests : {"/tests/", "\\tests\\"}) {
        if (const std::size_t at = path.rfind(tests); at != std::string::npos) {
            path = "tests/" + path.substr(at + 7);
            break;
        }
    }
    std::cout << "::error file=" << escape(path, true) << ",line=" << line
              << ",title=" << escape(current_test(), true) << "::" << escape(message, false) << std::endl;
}

inline void report_failure(const char* file, int line, const std::string& message) {
    ++failure_count();
    // Flushed for the same reason the case names are: a failed check is often
    // the step before a crash, and a line still in the buffer dies with it.
    std::cout << "    FAIL " << file << ":" << line << "  " << message << std::endl;
    if (const char* actions = std::getenv("GITHUB_ACTIONS"); actions != nullptr && std::string(actions) == "true") {
        annotate(file, line, message);
    }
}

inline int run_all() {
    int failed_cases = 0;
    for (const TestCase& test : registry()) {
        const int before = failure_count();
        current_test() = test.name;
        // Flushed, not just ended. When a case crashes, whatever names are still
        // in the buffer die with the process, and the log then stops up to a
        // page of names before the one that did it.
        std::cout << "  " << test.name << std::endl;
        try {
            test.body();
        } catch (const std::exception& e) {
            report_failure("<exception>", 0, std::string("threw: ") + e.what());
        } catch (...) {
            report_failure("<exception>", 0, "threw a non-std exception");
        }
        if (failure_count() > before) {
            ++failed_cases;
        }
    }

    std::cout << "\n"
              << registry().size() << " cases, " << failed_cases << " failed, "
              << failure_count() << " assertions failed\n";
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace harness

#define TEST(name)                                                            \
    static void name();                                                       \
    static const harness::Registrar registrar_##name(#name, name);            \
    static void name()

/// Compare with operator==, printing both sides when they differ.
#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        const auto& a_ = (actual);                                            \
        const auto& e_ = (expected);                                          \
        if (!(a_ == e_)) {                                                    \
            std::ostringstream os_;                                           \
            os_ << #actual " == " #expected "\n"                              \
                << "         got: " << a_ << "\n"                             \
                << "    expected: " << e_;                                    \
            harness::report_failure(__FILE__, __LINE__, os_.str());           \
        }                                                                     \
    } while (false)

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            harness::report_failure(__FILE__, __LINE__, #condition);          \
        }                                                                     \
    } while (false)
