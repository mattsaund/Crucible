// SPDX-License-Identifier: MIT
//
// Finding a runtime that was already built.
//
// The name is the whole safety mechanism: it carries the backend, the platform
// and the llama.cpp tag this binary needs, so a module built against another
// llama.cpp cannot be downloaded rather than being downloaded and then crashing
// on the first tensor. Everything here is string work and JSON that GitHub
// sent; nothing touches the network.
#include "test_helpers.hpp"

#include "crucible/runtime/prebuilt.hpp"

using namespace crucible;

namespace {

/// A releases reply with one asset in it, named by the caller.
std::string releases_with(const std::string& asset) {
    return R"([
      { "tag_name": "v0.6.0", "assets": [
          { "name": "Crucible-macOS.dmg",
            "browser_download_url": "https://example.invalid/dmg" } ] },
      { "tag_name": "v0.5.5", "assets": [
          { "name": ")" + asset + R"(",
            "browser_download_url": "https://example.invalid/runtime.tar.gz" } ] }
    ])";
}

}  // namespace

TEST(this_platform_has_a_name_the_assets_can_use) {
    // Every platform CI publishes for answers this; one that does not answer it
    // never asks for a download and goes straight to compiling.
    const std::string platform = prebuilt::platform_tag();
    CHECK(!platform.empty());
    CHECK(platform.find('-') != std::string::npos);   // <os>-<arch>
}

TEST(an_asset_name_carries_the_backend_the_platform_and_the_tag) {
    const std::string name = prebuilt::asset_name(BackendKind::Cuda);
    CHECK(name.rfind("runtime-cuda-", 0) == 0);
    CHECK(name.find(prebuilt::platform_tag()) != std::string::npos);
    CHECK(name.size() > 8 && name.compare(name.size() - 7, 7, ".tar.gz") == 0);

    // The llama.cpp tag is in there, which is what stops a module built against
    // another one from ever being asked for.
    const std::string tag(RuntimeStatus::required_llama_tag());
    CHECK(name.find(tag) != std::string::npos);
}

TEST(each_backend_asks_for_its_own_asset) {
    CHECK(prebuilt::asset_name(BackendKind::Cpu)
          != prebuilt::asset_name(BackendKind::Cuda));
    CHECK(prebuilt::asset_name(BackendKind::Vulkan)
          != prebuilt::asset_name(BackendKind::Metal));
}

TEST(the_asset_is_found_on_whichever_release_carries_it) {
    // Not only the newest: a runtime is keyed to a llama.cpp tag, and the
    // release that first shipped that tag goes on carrying it after newer
    // Crucible releases have come out.
    const std::string name = prebuilt::asset_name(BackendKind::Cpu);
    const std::string url  = prebuilt::find_asset(releases_with(name), name);
    CHECK_EQ(url, "https://example.invalid/runtime.tar.gz");
}

TEST(an_asset_that_is_not_there_is_not_invented) {
    const std::string name = prebuilt::asset_name(BackendKind::Cpu);
    CHECK(prebuilt::find_asset(releases_with("runtime-cpu-somewhere-else-b1.tar.gz"),
                               name).empty());
}

TEST(a_reply_that_is_not_a_release_list_finds_nothing) {
    // What api.github.com sends when it has had enough of you, and what a proxy
    // sends when it has opinions. Both have to read as "compile it".
    CHECK(prebuilt::find_asset(R"({"message":"API rate limit exceeded"})", "x").empty());
    CHECK(prebuilt::find_asset("<html>504</html>", "x").empty());
    CHECK(prebuilt::find_asset("", "x").empty());
    CHECK(prebuilt::find_asset("[]", "x").empty());
}

TEST(a_release_with_no_assets_is_stepped_over) {
    const char* body = R"([{"tag_name":"v1"},{"tag_name":"v2","assets":[]}])";
    CHECK(prebuilt::find_asset(body, "runtime-cpu-linux-x64-b1.tar.gz").empty());
}

TEST(the_release_list_is_asked_of_the_repository_the_installers_come_from) {
    const std::string url = prebuilt::releases_url();
    CHECK(url.find("api.github.com") != std::string::npos);
    CHECK(url.find("mattsaund/Crucible") != std::string::npos);
}
