// SPDX-License-Identifier: MIT
//
// The two NVIDIA libraries a downloaded CUDA runtime needs and a machine with
// only a driver does not have.
//
// The CUDA module is built in CI against the CUDA toolkit, and it links the
// toolkit's runtime (cudart) and its BLAS (cuBLAS) as shared libraries. The
// driver is not the toolkit: a machine with an NVIDIA card and a driver has
// libcuda, and does not have either of these. So the module was downloaded,
// put in place -- and then failed to load without a word, and the machine ran
// on Vulkan as if CUDA had never been installed.
//
// These are fetched with the runtime, from NVIDIA's own redistributable
// archives -- the same files the toolkit installs, published by NVIDIA for
// exactly this -- into a folder beside the modules. Crucible carries none of
// them; they come from NVIDIA, onto this machine, the way pip would fetch
// them for a Python program. And they are loaded by full path before the CUDA
// module is, so the module finds them already there whatever its own search
// path says.
//
// Linux on x86-64 and Windows on x64: the two platforms a CUDA runtime is
// published for. Everywhere else every function here is a harmless no.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::cuda_libraries {

/// The CUDA release the published module was built with, and so the release
/// its libraries are fetched from. Must agree with the `cuda:` version in
/// .github/workflows/runtimes.yml -- tests/test_install.sh checks that it does.
inline constexpr std::string_view kRelease = "13.3.1";

/// Where they are kept: a folder of its own beside the modules.
std::filesystem::path dir();

/// The files a CUDA module needs, in the order they have to be loaded: the
/// runtime, then cuBLAS's implementation library, then cuBLAS itself.
/// Empty where no CUDA runtime is published.
std::vector<std::string> wanted();

/// True when every one of them is in dir().
bool complete();

/// True when this machine can already load them without any help -- because
/// the toolkit is installed, which is also what a runtime compiled here needs.
bool on_system();

/// Fetch them from NVIDIA. `say` is told what is happening, for a progress
/// line. False, with `error` saying why, when they could not be had.
bool fetch(std::string& error, const std::function<void(std::string)>& say);

/// Load them, so that a CUDA module loaded after this finds them. Safe to call
/// any number of times and when there is nothing to load.
void preload();

/// What they take on disk.
std::uintmax_t bytes();

/// Take them away again, with the runtime that needed them.
void remove();

// --- exposed for the tests ---------------------------------------------------

namespace detail {

/// One archive to fetch: where it is, how big NVIDIA says it is, and which of
/// `wanted()` it holds.
struct Archive {
    std::string              url;
    std::uint64_t            size = 0;
    std::vector<std::string> files;
};

/// The archives to fetch, read out of NVIDIA's redistributable manifest
/// (redistrib_<release>.json) for `platform` -- "linux-x86_64" or
/// "windows-x86_64". Empty when the manifest does not list one of them.
std::vector<Archive> archives(std::string_view manifest, std::string_view platform);

}  // namespace detail

}  // namespace crucible::cuda_libraries
