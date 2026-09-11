// SPDX-License-Identifier: MIT
// A deliberately tiny test harness.
//
// Crucible already fetches three dependencies; a test framework would be a
// fourth, for something that fits in fifty lines. Tests self-register, report
// every failure in a case rather than stopping at the first, and the binary
// exits non-zero if anything failed, which is all CTest needs.
#pragma once

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

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back({name, std::move(body)});
    }
};

inline void report_failure(const char* file, int line, const std::string& message) {
    ++failure_count();
    std::cout << "    FAIL " << file << ":" << line << "  " << message << "\n";
}

inline int run_all() {
    int failed_cases = 0;
    for (const TestCase& test : registry()) {
        const int before = failure_count();
        std::cout << "  " << test.name << "\n";
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
