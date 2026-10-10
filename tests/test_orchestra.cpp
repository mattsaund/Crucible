// SPDX-License-Identifier: MIT
//
// Crucible's own Python and the orchestrator that runs on it: the digest a
// download is checked against, the pipe, and the process at the other end.
//
// The process tests need a Python. CI has one on PATH, and so does any machine
// building this; the orchestrator itself needs nothing but the standard
// library, so any 3.10 or newer will do.
#include "test_helpers.hpp"

#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include "crucible/lab/python.hpp"
#include "crucible/orchestra/link.hpp"
#include "crucible/util/sha256.hpp"
#include "crucible/util/subprocess.hpp"

namespace {

using json = nlohmann::json;

/// A Python on PATH for the tests to run the orchestrator on, or empty.
std::string test_python() {
    if (const char* chosen = std::getenv("CRUCIBLE_PYTHON"); chosen != nullptr && *chosen != '\0') {
        return chosen;
    }
    for (const char* name : {"python3", "python"}) {
        if (util::on_path(name)) {
            return name;
        }
    }
    return {};
}

std::string sha(std::string_view text) {
    util::Sha256 hash;
    hash.update(text);
    return hash.hex();
}

/// A roster of two, as the engine sends it.
json roster_request(const std::string& prompt, const std::string& pinned = {}) {
    return json{
        {"prompt", prompt},
        {"pinned", pinned},
        {"roster", json::array({
            {{"id", "physics"}, {"name", "Physics"}, {"tag", "PHYS"}, {"blurb", "mechanics"},
             {"keywords", {"orbit", "torque"}}, {"examples", json::array()}},
            {{"id", "chemistry"}, {"name", "Chemistry"}, {"tag", "CHEM"}, {"blurb", "reactions"},
             {"keywords", {"reaction"}}, {"examples", json::array()}},
        })},
        {"seats", {{"physics", {{"model", true}}}, {"chemistry", {{"model", true}}}}},
        {"routing", {{"min_confidence", 0.0}, {"default_expert", ""}}},
    };
}

}  // namespace

TEST(sha256_matches_the_published_test_vectors) {
    // FIPS 180-4's own examples, and the empty string.
    CHECK_EQ(sha(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(sha("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // A million a's, fed in pieces that do not line up with the blocks.
    util::Sha256 hash;
    const std::string piece(997, 'a');
    std::size_t fed = 0;
    while (fed + piece.size() <= 1000000) {
        hash.update(piece);
        fed += piece.size();
    }
    hash.update(std::string(1000000 - fed, 'a'));
    CHECK_EQ(hash.hex(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(a_file_is_hashed_as_its_bytes) {
    TempDir dir;
    const auto file = dir.path() / "blob";
    { std::ofstream(file, std::ios::binary) << "abc"; }
    CHECK_EQ(util::sha256_file(file), sha("abc"));
    CHECK(util::sha256_file(dir.path() / "missing").empty());
}

TEST(every_platform_crucible_ships_on_has_a_pinned_python) {
#if (defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))) || defined(__APPLE__) \
    || (defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)))
    const std::optional<lab::python::Build> build = lab::python::build_here();
    CHECK(build.has_value());
    if (build) {
        CHECK_EQ(build->version, "3.12.15");
        CHECK_EQ(build->sha256.size(), 64u);
        CHECK(build->url.rfind("https://github.com/astral-sh/python-build-standalone/", 0) == 0);
        CHECK(build->bytes > 10'000'000u);
    }
#endif
}

TEST(a_child_can_be_written_to_and_its_errors_kept_apart) {
    const std::string python = test_python();
    if (python.empty()) {
        std::printf("      (no Python on PATH; skipped)\n");
        return;
    }
    TempDir dir;
    util::Subprocess child;
    util::Subprocess::Streams streams;
    streams.input  = true;
    streams.errors = dir.path() / "errors.log";
    std::string error;
    CHECK(child.start({python, "-c",
                       "import sys\n"
                       "line = sys.stdin.readline().strip()\n"
                       "print('noise', file=sys.stderr)\n"
                       "print(line[::-1])\n"},
                      {}, {}, error, streams));
    CHECK(child.write_line("olleh"));
    child.close_input();
    std::string line;
    CHECK(child.read_line(line));
    CHECK_EQ(line, "hello");
    CHECK(!child.read_line(line));   // nothing else: the noise went to the file
    CHECK_EQ(child.wait(), 0);
    std::ifstream in(streams.errors);
    std::string said;
    std::getline(in, said);
    CHECK_EQ(said, "noise");
}

TEST(the_orchestrator_answers_over_the_pipe) {
    const std::string python = test_python();
    if (python.empty()) {
        std::printf("      (no Python on PATH; skipped)\n");
        return;
    }
    TempDir dir;
    ScopedDataHome home(dir.path());
    const char* had = std::getenv("CRUCIBLE_PYTHON");
    const std::string previous = had != nullptr ? had : "";
    set_env("CRUCIBLE_PYTHON", python);

    orchestra::Link link;
    const json pong = link.call("ping", {{"say", "hi"}});
    CHECK_EQ(pong.value("pong", ""), "hi");

    // A pinned route asks the core nothing.
    const json pinned = link.call("route", roster_request("anything", "chemistry"));
    CHECK_EQ(pinned.value("expert", ""), "chemistry");
    CHECK_EQ(pinned.value("source", ""), "pinned");

    // With no delegator it routes on keywords, having asked whether there is one.
    std::vector<std::string> asked;
    std::mutex               asked_mutex;
    link.set_handler([&](const std::string& method, const json&) {
        const std::lock_guard<std::mutex> lock(asked_mutex);
        asked.push_back(method);
        return json{{"available", false}};
    });
    const json routed = link.call("route", roster_request("what holds a satellite in orbit"));
    CHECK_EQ(routed.value("expert", ""), "physics");
    CHECK_EQ(routed.value("source", ""), "keywords");
    CHECK(asked == std::vector<std::string>{"delegator.ready"});

    // A core that cannot answer makes the call fail, with the reason.
    link.set_handler([](const std::string&, const json&) -> json {
        throw std::runtime_error("no delegator here");
    });
    bool threw = false;
    try {
        link.call("route", roster_request("a question"));
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("no delegator here") != std::string::npos;
    }
    CHECK(threw);

    // Several calls at once, from several threads: each gets its own answer.
    std::vector<std::thread> callers;
    std::vector<std::string> said(4);
    for (std::size_t i = 0; i < said.size(); ++i) {
        callers.emplace_back([&link, &said, i] {
            said[i] = link.call("ping", {{"say", "caller " + std::to_string(i)}}).value("pong", "");
        });
    }
    for (std::thread& caller : callers) {
        caller.join();
    }
    for (std::size_t i = 0; i < said.size(); ++i) {
        CHECK_EQ(said[i], "caller " + std::to_string(i));
    }

    // Gone, and back on the next call.
    link.stop();
    CHECK(!link.running());
    CHECK_EQ(link.call("ping", {{"say", "again"}}).value("pong", ""), "again");
    link.stop();
    if (previous.empty()) {
        unset_env("CRUCIBLE_PYTHON");
    } else {
        set_env("CRUCIBLE_PYTHON", previous);
    }
}

TEST(every_file_of_the_orchestrator_is_compiled_in) {
    // The package is listed twice, in CMakeLists.txt and src/orchestra/package.cpp,
    // and a file added to the folder but to neither would be missing from every
    // build without anything failing until it was imported.
    TempDir dir;
    std::string error;
    CHECK(orchestra::write_package(dir.path(), error));
    std::set<std::string> written;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path() / "crucible_orchestrator")) {
        written.insert(entry.path().filename().string());
    }
    const std::filesystem::path source =
        std::filesystem::path(CRUCIBLE_SOURCE_DIR) / "scripts" / "orchestrator" / "crucible_orchestrator";
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
        if (entry.path().extension() == ".py") {
            CHECK(written.count(entry.path().filename().string()) == 1);
            if (written.count(entry.path().filename().string()) == 0) {
                std::printf("      %s is not compiled in\n", entry.path().filename().string().c_str());
            }
        }
    }
}
