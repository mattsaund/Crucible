// SPDX-License-Identifier: MIT
//
// A runtime that was already built, somewhere else, by the same tag.
//
// Building a backend on the machine that will run it is the right default and
// it is what Crucible has always done: a backend compiled against another
// llama.cpp loads and then crashes on the first tensor, so the safe thing is to
// compile it here, against the source this binary was built from.
//
// It is also, for CUDA, four and a half minutes on a twenty-eight core machine
// and a multi-gigabyte toolkit that has to be installed first. That toolkit is
// the real cost: a person with an NVIDIA card and no nvcc cannot have a GPU
// runtime at all today, however long they are willing to wait.
//
// So: the same modules, compiled by the release workflow on the same tag, and
// downloaded. The name carries everything that has to match -- the backend, the
// platform, and the llama.cpp tag this binary needs -- so a mismatch cannot be
// downloaded rather than being downloaded and then crashing. Anything that
// fails here falls back to compiling, which is why none of it reports an error
// to the user.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/runtime/backend.hpp"

namespace crucible::prebuilt {

/// The oldest card a published CUDA runtime has kernels for. CUDA 13 starts
/// at Turing, compute capability 7.5; a card before it is not slow with one,
/// it fails at the first kernel.
inline constexpr int kCudaFloor = 75;

/// Which kernels a published CUDA runtime carries: in its asset's name
/// ("-k2"), and in the manifest once it is downloaded. 2 is llama.cpp's own
/// list, Turing to Blackwell. Before it the name said nothing, and the list
/// was the RTX 30, 40 and 50 cards' alone: on a machine with any other
/// card, a CUDA runtime downloaded then is fetched again. See runtimes.yml.
inline constexpr int kCudaKernels = 2;

/// Whether a CUDA runtime published before kCudaKernels runs every card
/// here. Its kernels were for compute capability 8.6, 8.9 and 12.0, and a
/// kernel runs only on its own major version at its minor or above: an RTX
/// 30, 40 or 50 runs it, a Turing card, an A100 or a Hopper does not. Asked
/// of nvidia-smi once.
bool old_cuda_kernels_run_here();

/// Whether every NVIDIA card here can run a published CUDA runtime. True when
/// nvidia-smi says nothing: there is then nothing to protect.
bool cuda_cards_supported();

/// This machine, as the release assets name it: "linux-x64", "macos-arm64",
/// "windows-x64". Empty where no build is published, which is every platform
/// the release workflow does not have a runner for.
std::string platform_tag();

/// What the asset for `kind` is called on this platform, empty when there is
/// none to ask for. Every part that has to match is in the name: the backend,
/// the platform, and the llama.cpp tag.
std::string asset_name(BackendKind kind);

/// Where the release list is read from.
std::string releases_url();

/// The download URL for `name` in a GitHub releases-API reply, or empty.
///
/// Walks every release rather than only the newest: the asset is keyed to a
/// llama.cpp tag, and the release that first shipped that tag keeps carrying it
/// after newer Crucible releases have moved on.
std::string find_asset(std::string_view releases_json, std::string_view name);

/// Ask GitHub for the release list. Blocking.
bool fetch_releases(std::string& body, std::string& error);

/// Download `url` and unpack its modules into `into`.
///
/// `say` is given a line of progress at each step. `written` receives the
/// module files that landed, so the caller can verify it got what it asked for
/// before writing a manifest entry that claims it did.
bool install(BackendKind kind, const std::string& url, const std::filesystem::path& into,
             std::vector<std::filesystem::path>& written, std::string& error,
             const std::function<void(std::string)>& say);

/// The whole thing: is there one, and can it be put in place? False means
/// "compile it", not "something went wrong" -- unless `error` says what did:
/// a module that landed without the NVIDIA libraries it needs, which a
/// compile would not mend.
bool try_install(BackendKind kind, const std::filesystem::path& into,
                 std::vector<std::filesystem::path>& written,
                 const std::function<void(std::string)>& say, std::string& error);

}  // namespace crucible::prebuilt
