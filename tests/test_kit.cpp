// SPDX-License-Identifier: MIT
//
// The kit: which download each platform gets, read from each program's own
// published listing. The listings here are cut down from the real ones, as
// they were on 2026-10-10; the fetching itself is the network's, and is what
// `crucible --install-kit` is for.
#include "test_helpers.hpp"

#include "crucible/app/self_source.hpp"
#include "crucible/app/starter.hpp"
#include "crucible/kit/kit.hpp"

#include <fstream>

namespace {

const char* const kGhRelease = R"({"tag_name": "v2.102.0", "assets": [
  {"name": "gh_2.102.0_linux_amd64.tar.gz", "size": 15319960,
   "browser_download_url": "https://github.com/cli/cli/releases/download/v2.102.0/gh_2.102.0_linux_amd64.tar.gz"},
  {"name": "gh_2.102.0_linux_arm64.tar.gz", "size": 13917794,
   "browser_download_url": "https://github.com/cli/cli/releases/download/v2.102.0/gh_2.102.0_linux_arm64.tar.gz"},
  {"name": "gh_2.102.0_macOS_arm64.zip", "size": 14359983,
   "browser_download_url": "https://github.com/cli/cli/releases/download/v2.102.0/gh_2.102.0_macOS_arm64.zip"},
  {"name": "gh_2.102.0_macOS_universal.pkg", "size": 29429945,
   "browser_download_url": "https://github.com/cli/cli/releases/download/v2.102.0/gh_2.102.0_macOS_universal.pkg"},
  {"name": "gh_2.102.0_windows_amd64.zip", "size": 15512013,
   "browser_download_url": "https://github.com/cli/cli/releases/download/v2.102.0/gh_2.102.0_windows_amd64.zip"}
]})";

const char* const kNodeIndex = R"([
  {"version": "v25.1.0", "lts": false, "files": ["osx-arm64-tar", "linux-x64", "win-x64-zip"]},
  {"version": "v24.21.0", "lts": "Krypton", "files": ["osx-arm64-tar", "linux-x64", "win-x64-zip"]},
  {"version": "v22.20.0", "lts": "Jod", "files": ["osx-arm64-tar"]}
])";

const char* const kBrowsers = R"({"channels": {"Stable": {"version": "155.0.8059.39", "downloads": {
  "chrome": [{"platform": "mac-arm64", "url": "https://x/chrome-mac-arm64.zip"}],
  "chrome-headless-shell": [
    {"platform": "linux64", "url": "https://x/155/linux64/chrome-headless-shell-linux64.zip"},
    {"platform": "mac-arm64", "url": "https://x/155/mac-arm64/chrome-headless-shell-mac-arm64.zip"},
    {"platform": "win64", "url": "https://x/155/win64/chrome-headless-shell-win64.zip"}]}}}})";

const char* const kMinGit = R"({"tag_name": "v2.56.0.windows.2", "assets": [
  {"name": "MinGit-2.56.0.2-32-bit.zip", "size": 39426788, "browser_download_url": "https://x/MinGit-32.zip"},
  {"name": "MinGit-2.56.0.2-64-bit.zip", "size": 39806486, "browser_download_url": "https://x/MinGit-64.zip"},
  {"name": "MinGit-2.56.0.2-busybox-64-bit.zip", "size": 1, "browser_download_url": "https://x/busybox.zip"},
  {"name": "MinGit-2.56.0.2-arm64.zip", "size": 37936336, "browser_download_url": "https://x/MinGit-arm64.zip"}
]})";

}  // namespace

TEST(each_platform_gets_its_own_github_cli) {
    std::uint64_t bytes = 0;
    CHECK(kit::detail::gh_asset(kGhRelease, {"macos", "arm64"}, bytes).find("macOS_arm64.zip") != std::string::npos);
    CHECK_EQ(bytes, std::uint64_t{14359983});
    CHECK(kit::detail::gh_asset(kGhRelease, {"linux", "x64"}, bytes).find("linux_amd64.tar.gz") != std::string::npos);
    CHECK(kit::detail::gh_asset(kGhRelease, {"windows", "x64"}, bytes).find("windows_amd64.zip") != std::string::npos);
    // None for a machine the release has nothing for, and none from a listing
    // that is not one.
    CHECK(kit::detail::gh_asset(kGhRelease, {"windows", "arm64"}, bytes).empty());
    CHECK(kit::detail::gh_asset("{\"message\": \"rate limited\"}", {"macos", "arm64"}, bytes).empty());
}

TEST(node_is_the_newest_long_term_release_for_the_platform) {
    std::string version;
    const std::string mac = kit::detail::node_url(kNodeIndex, {"macos", "arm64"}, version);
    CHECK_EQ(version, std::string("v24.21.0"));
    CHECK_EQ(mac, std::string("https://nodejs.org/dist/v24.21.0/node-v24.21.0-darwin-arm64.tar.gz"));
    CHECK_EQ(kit::detail::node_url(kNodeIndex, {"linux", "x64"}, version),
             std::string("https://nodejs.org/dist/v24.21.0/node-v24.21.0-linux-x64.tar.xz"));
    CHECK_EQ(kit::detail::node_url(kNodeIndex, {"windows", "x64"}, version),
             std::string("https://nodejs.org/dist/v24.21.0/node-v24.21.0-win-x64.zip"));
    CHECK(kit::detail::node_url("[]", {"macos", "arm64"}, version).empty());
}

TEST(the_headless_browser_is_chromes_shell_for_the_platform) {
    CHECK(kit::detail::browser_url(kBrowsers, {"macos", "arm64"}).find("chrome-headless-shell-mac-arm64.zip")
          != std::string::npos);
    CHECK(kit::detail::browser_url(kBrowsers, {"linux", "x64"}).find("linux64") != std::string::npos);
    CHECK(kit::detail::browser_url(kBrowsers, {"windows", "x64"}).find("win64") != std::string::npos);
    CHECK(kit::detail::browser_url(kBrowsers, {"macos", "x64"}).empty());
}

TEST(git_for_windows_is_mingit_without_busybox) {
    std::uint64_t bytes = 0;
    CHECK_EQ(kit::detail::mingit_asset(kMinGit, {"windows", "x64"}, bytes), std::string("https://x/MinGit-64.zip"));
    CHECK_EQ(bytes, std::uint64_t{39806486});
    CHECK_EQ(kit::detail::mingit_asset(kMinGit, {"windows", "arm64"}, bytes), std::string("https://x/MinGit-arm64.zip"));
}

TEST(the_kit_puts_what_it_has_on_path_once) {
    TempDir dir;
    ScopedDataHome home(dir.path());
    // A piece is the folder and the marker an install leaves when it finished.
    const std::filesystem::path bin = kit::dir() / "gh" / "bin";
    std::filesystem::create_directories(bin);
    std::ofstream(kit::dir() / "gh" / ".crucible-kit") << "x";
    CHECK(kit::installed("gh"));
    CHECK(!kit::installed("node"));
    const char* had = std::getenv("PATH");
    const std::string before = had != nullptr ? had : "";
    kit::put_on_path();
    kit::put_on_path();
    const std::string after = std::getenv("PATH");
    CHECK_EQ(after.rfind(bin.string(), 0), std::size_t{0});
    // Once, however often it is called.
    CHECK_EQ(after.find(bin.string(), 1), std::string::npos);
    set_env("PATH", before);
}

// ---------------------------------------------------------------------------
// A model to build with, for a machine with none
// ---------------------------------------------------------------------------

TEST(the_starter_model_is_the_largest_that_leaves_half_the_machine) {
    constexpr std::uint64_t kGiB = 1ULL << 30;
    CHECK_EQ(starter::for_memory(64 * kGiB).label, std::string("Qwen3 Coder 30B"));
    CHECK_EQ(starter::for_memory(24 * kGiB).label, std::string("Qwen2.5 Coder 14B"));
    CHECK_EQ(starter::for_memory(16 * kGiB).label, std::string("Qwen2.5 Coder 7B"));
    CHECK_EQ(starter::for_memory(8 * kGiB).label, std::string("Qwen3 4B"));
    CHECK_EQ(starter::for_memory(2 * kGiB).label, std::string("Qwen2.5 Coder 1.5B"));
    // Even a machine that cannot say is given the smallest rather than none.
    CHECK_EQ(starter::for_memory(0).label, std::string("Qwen2.5 Coder 1.5B"));
    CHECK_EQ(starter::for_memory(16 * kGiB).url(),
             std::string("https://huggingface.co/bartowski/Qwen2.5-Coder-7B-Instruct-GGUF/resolve/main/"
                         "Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf"));
}

TEST(a_starter_is_fetched_only_for_a_machine_with_nothing_to_build_with) {
    TempDir dir;
    Config config;
    config.models_dir = dir.path().string();
    CHECK(starter::needed(config));

    // A model in the folder is somebody who has chosen.
    std::ofstream(dir.path() / "mine.gguf") << "GGUF";
    CHECK(!starter::needed(config));
    std::filesystem::remove(dir.path() / "mine.gguf");

    // So is a provider.
    Provider claude;
    claude.id   = "anthropic";
    claude.kind = "anthropic";
    config.providers.push_back(claude);
    CHECK(!starter::needed(config));
}

TEST(a_starter_is_seated_as_programming_and_everything_falls_back_to_it) {
    TempDir dir;
    Config config;
    config.models_dir = dir.path().string();
    std::ofstream(dir.path() / "coder.gguf") << "GGUF";
    starter::seat(config, "coder.gguf");
    CHECK_EQ(config.roster.size(), std::size_t{1});
    const ExpertId id = config.roster.experts().front().id;
    CHECK_EQ(config.roster.experts().front().name, std::string("Programming"));
    CHECK(config.has_expert(id));
    CHECK_EQ(config.build.worker_model, std::string("coder.gguf"));
    CHECK_EQ(config.routing.default_expert, id);

    // Seats that were there keep their models; only the empty ones are filled.
    Config mixed;
    mixed.models_dir = dir.path().string();
    std::string error;
    Expert writing;
    writing.name  = "Writing";
    writing.blurb = "prose";
    mixed.roster.add(writing, error);
    mixed.experts[mixed.roster.experts().front().id].model = "other.gguf";
    starter::seat(mixed, "coder.gguf");
    CHECK_EQ(mixed.experts[mixed.roster.experts().front().id].model, std::string("other.gguf"));
}

// ---------------------------------------------------------------------------
// Crucible's own source
// ---------------------------------------------------------------------------

TEST(crucibles_own_source_is_found_from_the_program_built_from_it) {
    // The tests are built from the source, so walking up from them finds it.
    const self::Source here = self::find();
    CHECK(here.found);
    CHECK(self::is_source(here.root));
    CHECK(std::filesystem::exists(here.build / "CMakeCache.txt"));

    // A folder that is not the source is said to be not it.
    TempDir dir;
    const self::Source wrong = self::find(dir.path());
    CHECK(!wrong.found);
    CHECK(wrong.why.find("is not Crucible's source") != std::string::npos);
}
