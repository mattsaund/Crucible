// SPDX-License-Identifier: MIT
//
// The backend table. See backend.hpp for why everything derives from it.
#include "crucible/runtime/backend.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include "crucible/util/subprocess.hpp"

namespace crucible {
namespace {

// Order matters: this is the order the settings screen lists runtimes in, and
// CPU comes first because every other one needs it.
constexpr std::array<BackendInfo, kBackendCount> kBackends{{
    {BackendKind::Cpu, "cpu", "CPU", /*reg=*/"CPU",
     "The processor. Slowest; the others need it.",
     "GGML_CPU", "ggml-cpu",
     /*required_tool=*/"",
     /*apt=*/"build-essential", /*dnf=*/"gcc-c++ make", /*pacman=*/"base-devel",
     /*brew=*/"", /*multi_device=*/false, /*required=*/true},

    {BackendKind::Cuda, "cuda", "CUDA", /*reg=*/"CUDA",
     "NVIDIA cards. Fastest there.",
     "GGML_CUDA", "ggml-cuda",
     /*required_tool=*/"nvcc",
     /*apt=*/"nvidia-cuda-toolkit",
     /*dnf=*/"cuda-toolkit",
     /*pacman=*/"cuda",
     // Nothing for brew: NVIDIA has shipped no macOS driver since 2018, so
     // this backend is not offered there at all. See backend_available_here.
     /*brew=*/"",
     /*multi_device=*/true, /*required=*/false},

    {BackendKind::Vulkan, "vulkan", "Vulkan", /*reg=*/"Vulkan",
     "Any GPU with a Vulkan driver.",
     "GGML_VULKAN", "ggml-vulkan",
     /*required_tool=*/"glslc",
     // spirv-headers is easy to miss and the build fails at configure time
     // without it, several minutes in, with an error that names a CMake
     // package rather than anything installable.
     /*apt=*/"glslc libvulkan-dev spirv-headers",
     /*dnf=*/"glslc vulkan-loader-devel spirv-headers-devel",
     /*pacman=*/"shaderc vulkan-headers vulkan-icd-loader spirv-headers",
     // MoltenVK puts Vulkan on top of Metal, so this can be made to work on a
     // Mac -- but Metal is the direct route there and is always present, so
     // the packages are named for anyone who wants it rather than recommended.
     /*brew=*/"molten-vk shaderc vulkan-headers",
     /*multi_device=*/true, /*required=*/false},

    {BackendKind::Metal, "metal", "Metal", /*reg=*/"MTL",
     "Apple GPUs.",
     "GGML_METAL", "ggml-metal",
     // Not the compiler but the tool that finds it. ggml compiles the Metal
     // shaders through `xcrun -sdk macosx metal`, and xcrun is what reports a
     // command-line-tools install that has not been completed.
     /*required_tool=*/"xcrun",
     /*apt=*/"", /*dnf=*/"", /*pacman=*/"",
     // Not a package: it comes with the Xcode command line tools, which brew
     // itself needs, so anyone able to run the suggestion already has it.
     /*brew=*/"",
     // One GPU per Mac. The split settings have nothing to divide, and unified
     // memory means there is nothing to divide it between.
     /*multi_device=*/false, /*required=*/false},
}};

/// Is this an Apple platform? The one thing the table cannot express, because
/// it is about the machine reading it rather than about the backend.
constexpr bool kOnApple =
#ifdef __APPLE__
    true;
#else
    false;
#endif

}  // namespace

const std::array<BackendInfo, kBackendCount>& all_backends() {
    return kBackends;
}

bool backend_available_here(BackendKind kind) {
    switch (kind) {
        case BackendKind::Metal:
            return kOnApple;
        case BackendKind::Cuda:
            // NVIDIA stopped shipping a macOS driver in 2018 and the toolkit
            // followed. Offering it there is offering a build that cannot work.
            return !kOnApple;
        case BackendKind::Cpu:
        case BackendKind::Vulkan:
            break;
    }
    return true;
}

std::string install_hint(const BackendInfo& info) {
    // In the order a machine is likely to have exactly one of them. brew is
    // checked first on Apple because a Mac with Homebrew has no apt, and last
    // elsewhere because a Linux box with brew still wants its system packages.
    struct Manager {
        std::string_view program;
        std::string_view command;
        std::string_view BackendInfo::*packages;
    };
    static constexpr std::array<Manager, 4> kManagers{{
        {"apt-get", "sudo apt install",   &BackendInfo::apt_packages},
        {"dnf",     "sudo dnf install",   &BackendInfo::dnf_packages},
        {"pacman",  "sudo pacman -S",     &BackendInfo::pacman_packages},
        {"brew",    "brew install",       &BackendInfo::brew_packages},
    }};

    for (std::size_t i = 0; i < kManagers.size(); ++i) {
        const Manager& manager = kManagers[kOnApple ? kManagers.size() - 1 - i : i];
        const std::string_view packages = info.*(manager.packages);
        if (packages.empty() || !util::on_path(std::string(manager.program))) {
            continue;
        }
        return std::string(manager.command) + " " + std::string(packages);
    }
    return {};
}

const BackendInfo& backend_info(BackendKind kind) {
    return kBackends[static_cast<std::size_t>(kind)];
}

std::optional<BackendKind> backend_from_id(std::string_view id) {
    for (const BackendInfo& info : kBackends) {
        if (info.id == id) {
            return info.kind;
        }
    }
    return std::nullopt;
}

std::string_view module_prefix() {
#ifdef _WIN32
    return "ggml-";
#else
    return "libggml-";
#endif
}

std::string_view module_suffix() {
#ifdef _WIN32
    return ".dll";
#else
    return ".so";
#endif
}

std::optional<BackendKind> backend_from_reg_name(std::string_view reg_name) {
    const auto lower = [](std::string_view text) {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };
    const std::string wanted = lower(reg_name);
    for (const BackendInfo& info : kBackends) {
        if (lower(info.reg_name) == wanted) {
            return info.kind;
        }
    }
    return std::nullopt;
}

}  // namespace crucible
