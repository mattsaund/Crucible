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

#include <iterator>

#include "crucible/runtime/cuda_libraries.hpp"
#include "crucible/runtime/prebuilt.hpp"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>   // SetThreadErrorMode
#endif

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
    // CUDA's says which kernels it has, so one published before them, under
    // the same llama.cpp, is never taken for it.
    CHECK(name.find(tag + "-k" + std::to_string(prebuilt::kCudaKernels) + ".tar.gz") != std::string::npos);
    CHECK(prebuilt::asset_name(BackendKind::Cpu).find("-k") == std::string::npos);
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

// --- NVIDIA's libraries, which a downloaded CUDA module needs ----------------

TEST(the_cuda_libraries_are_read_out_of_nvidias_manifest) {
    // Trimmed from redistrib_13.3.1.json. The sizes are strings, which is
    // what NVIDIA writes -- and reading one as a number once took the whole
    // program down from the thread installing the runtime.
    const std::string manifest = R"({
        "release_label": "13.3.1",
        "cuda_cudart": {
            "version": "13.3.29",
            "linux-x86_64": {"relative_path": "cuda_cudart/linux-x86_64/cuda_cudart-linux-x86_64-13.3.29-archive.tar.xz",
                             "size": "1573744"},
            "windows-x86_64": {"relative_path": "cuda_cudart/windows-x86_64/cuda_cudart-windows-x86_64-13.3.29-archive.zip",
                               "size": "2589792"}
        },
        "libcublas": {
            "version": "13.6.0.2",
            "linux-x86_64": {"relative_path": "libcublas/linux-x86_64/libcublas-linux-x86_64-13.6.0.2-archive.tar.xz",
                             "size": 817981368}
        }
    })";
    const auto linux_archives = crucible::cuda_libraries::detail::archives(manifest, "linux-x86_64");
    CHECK_EQ(linux_archives.size(), std::size_t{2});
    if (linux_archives.size() == 2) {
        CHECK(linux_archives[0].url.rfind("https://developer.download.nvidia.com/compute/cuda/redist/cuda_cudart/", 0) == 0);
        CHECK_EQ(linux_archives[0].size, std::uint64_t{1573744});
        CHECK_EQ(linux_archives[1].size, std::uint64_t{817981368});
        CHECK(!linux_archives[1].files.empty());
    }
    // A platform cuBLAS is not listed for is no set at all, not half of one.
    CHECK(crucible::cuda_libraries::detail::archives(manifest, "windows-x86_64").empty());
    CHECK(crucible::cuda_libraries::detail::archives("not json", "linux-x86_64").empty());
}

namespace {

namespace cuda = crucible::cuda_libraries;

/// What is left to read of a stream, or what a file holds, whole.
std::string all_of(std::istream& in) {
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
std::string contents_of(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return all_of(in);
}

/// A set in place the way an earlier fetch left it: `what` in each library,
/// and `release` written beside them, or nothing beside them when it is empty.
void place_set(const std::string& what, const std::string& release) {
    std::filesystem::create_directories(cuda::dir());
    for (const std::string& name : cuda::wanted()) {
        std::ofstream(cuda::dir() / name, std::ios::binary) << what;
    }
    std::filesystem::remove(cuda::dir() / "release");
    if (!release.empty()) {
        std::ofstream(cuda::dir() / "release") << release << '\n';
    }
}

/// NVIDIA's archives as a fetch unpacks them, each library saying `what`.
std::filesystem::path unpacked_set(const std::filesystem::path& into, const std::string& what) {
    for (const std::string& name : cuda::wanted()) {
        const std::filesystem::path folder = into / ("archive-" + name) / "lib";
        std::filesystem::create_directories(folder);
        std::ofstream(folder / name, std::ios::binary) << what;
    }
    return into;
}

/// preload() loads a complete set, and the sets here are not libraries. On
/// Windows one that will not load can put up a dialog box, which on a machine
/// nobody is watching waits for ever; on this thread, while this lives, it
/// does not.
class NoDialogs {
public:
#if defined(_WIN32)
    NoDialogs() { ::SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previous_); }
    ~NoDialogs() { ::SetThreadErrorMode(previous_, nullptr); }
#else
    NoDialogs() {}
    ~NoDialogs() {}
#endif
    NoDialogs(const NoDialogs&)            = delete;
    NoDialogs& operator=(const NoDialogs&) = delete;

private:
#if defined(_WIN32)
    DWORD previous_ = 0;
#endif
};

}  // namespace

TEST(nvidias_libraries_from_another_release_are_fetched_again) {
    // An update brings a CUDA module built with a newer toolkit. Libraries
    // that were only looked for stayed: an older cudart and cuBLAS, under a
    // module that needs the newer ones.
    if (cuda::wanted().empty()) {
        return;   // no CUDA runtime is published for this platform
    }
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    place_set("old", "");   // from a Crucible that did not write it down
    CHECK(!cuda::complete());
    CHECK(cuda::usable());  // still loaded, so an update does not turn CUDA off
    place_set("old", "12.9.1");
    CHECK(!cuda::complete());
    CHECK(!cuda::usable());
    place_set("new", std::string(cuda::kRelease));
    CHECK(cuda::complete());
    CHECK(cuda::usable());
    std::filesystem::remove(cuda::dir() / cuda::wanted().back());
    CHECK(!cuda::complete());
    CHECK(!cuda::usable());
}

TEST(a_fetched_set_is_moved_over_the_one_in_use_and_never_written_into_it) {
    // A running Crucible has the old set loaded, and preload never closes it.
    // Copied over in place, the new set was written into the very file that
    // process has mapped, which crashes it at its next call into CUDA.
    if (cuda::wanted().empty()) {
        return;
    }
    TempDir temp;
    const ScopedDataHome scoped(temp.path());
    const NoDialogs quiet;
    place_set("old", "12.9.1");

    std::string error;
    CHECK(cuda::detail::stage(unpacked_set(temp.path() / "unpacked", "new"), true, error));
    // Staged beside the old set, which is as it was until the new one goes in.
    CHECK(cuda::waiting());
    CHECK(!cuda::complete());
    const std::filesystem::path first = cuda::dir() / cuda::wanted().front();
    CHECK_EQ(contents_of(first), std::string("old"));

    // Open, the way a loaded library is, while the next start moves the new
    // set in before it loads anything.
    std::ifstream in_use(first, std::ios::binary);
    cuda::preload();
    // Whoever has the old file still reads the old file.
    CHECK_EQ(all_of(in_use), std::string("old"));
#if defined(_WIN32)
    // Windows will not replace a file that is open, so there the set waits:
    // for the next start, and here for the file to be closed.
    in_use.close();
    cuda::preload();
#endif
    CHECK(!cuda::waiting());
    CHECK(cuda::complete());
    for (const std::string& name : cuda::wanted()) {
        CHECK_EQ(contents_of(cuda::dir() / name), std::string("new"));
        CHECK(!std::filesystem::exists(cuda::dir() / (name + ".new")));
    }
}

TEST(a_set_staged_halfway_or_for_another_release_never_goes_in) {
    if (cuda::wanted().empty()) {
        return;
    }
    TempDir temp;
    const ScopedDataHome scoped(temp.path());
    const NoDialogs quiet;
    place_set("old", std::string(cuda::kRelease));
    const std::filesystem::path unpacked = unpacked_set(temp.path() / "unpacked", "new");
    const std::filesystem::path first    = cuda::dir() / cuda::wanted().front();
    std::string error;

    // A fetch that stopped before it wrote the release staged no set.
    CHECK(cuda::detail::stage(unpacked, true, error));
    std::filesystem::remove(cuda::dir() / "release.new");
    CHECK(!cuda::waiting());
    cuda::preload();
    CHECK_EQ(contents_of(first), std::string("old"));
    CHECK(cuda::complete());

    // Nor did one a Crucible staged for another release, which goes.
    CHECK(cuda::detail::stage(unpacked, true, error));
    std::ofstream(cuda::dir() / "release.new") << "12.9.1\n";
    CHECK(!cuda::waiting());
    cuda::preload();
    CHECK_EQ(contents_of(first), std::string("old"));
    CHECK(!std::filesystem::exists(cuda::dir() / "release.new"));
    for (const std::string& name : cuda::wanted()) {
        CHECK(!std::filesystem::exists(cuda::dir() / (name + ".new")));
    }
}

TEST(an_archive_short_of_one_library_stages_none_of_them) {
    if (cuda::wanted().empty()) {
        return;
    }
    TempDir temp;
    const ScopedDataHome scoped(temp.path());
    const std::filesystem::path unpacked = unpacked_set(temp.path() / "unpacked", "new");
    std::filesystem::remove_all(unpacked / ("archive-" + cuda::wanted().back()));

    std::string error;
    CHECK(!cuda::detail::stage(unpacked, true, error));
    CHECK(error.find(cuda::wanted().back()) != std::string::npos);
    CHECK(!cuda::waiting());
    for (const std::string& name : cuda::wanted()) {
        CHECK(!std::filesystem::exists(cuda::dir() / (name + ".new")));
    }
}
