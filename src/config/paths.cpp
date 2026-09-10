// SPDX-License-Identifier: MIT
//
// Where Crucible keeps its files.
//
// The XDG base directory spec on Linux and macOS, and %APPDATA% /
// %LOCALAPPDATA% on Windows -- with the XDG variables still honored first
// there, because someone running Crucible under a POSIX-flavoured shell on
// Windows has usually set them and expects them to mean what they mean
// everywhere else.
//
// The spec's awkward rule is honored here: an XDG variable set to a *relative*
// path must be ignored rather than resolved, because resolving it against the
// working directory would scatter config wherever the user happened to be.
#include "crucible/config/paths.hpp"

#include <cstdlib>


namespace crucible::paths {
namespace {

std::filesystem::path home_dir() {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home);
    }
#if defined(_WIN32)
    // Windows has no HOME unless something set one. USERPROFILE is the one
    // every shell there agrees on.
    if (const char* profile = std::getenv("USERPROFILE");
        profile != nullptr && *profile != '\0') {
        return std::filesystem::path(profile);
    }
#endif
    return std::filesystem::current_path();
}

/// Honor an XDG variable if it is set to an absolute path, per the spec:
/// a relative value must be ignored rather than resolved.
///
/// `fallback` is the directory under $HOME to use when it is not set;
/// `windows_var` is the environment variable that plays the same part on
/// Windows, where there is no such convention.
std::filesystem::path base_dir(const char* env_var, const char* fallback,
                               const char* windows_var) {
    if (const char* value = std::getenv(env_var); value != nullptr && *value != '\0') {
        if (std::filesystem::path candidate(value); candidate.is_absolute()) {
            return candidate / "crucible";
        }
    }
#if defined(_WIN32)
    if (const char* value = std::getenv(windows_var); value != nullptr && *value != '\0') {
        return std::filesystem::path(value) / "crucible";
    }
    return home_dir() / "crucible";
#else
    (void)windows_var;
    return home_dir() / fallback / "crucible";
#endif
}

}  // namespace

std::filesystem::path config_dir() {
    return base_dir("XDG_CONFIG_HOME", ".config", "APPDATA");
}
std::filesystem::path data_dir() {
    return base_dir("XDG_DATA_HOME", ".local/share", "LOCALAPPDATA");
}

std::filesystem::path config_file() { return config_dir() / "config.json"; }
std::filesystem::path trust_file()  { return config_dir() / "trust.json"; }
std::filesystem::path models_dir()  { return data_dir() / "models"; }
std::filesystem::path log_file()    { return data_dir() / "crucible.log"; }

std::filesystem::path runtimes_dir()      { return data_dir() / "runtimes"; }
std::filesystem::path runtime_src_dir()   { return data_dir() / "runtime-src"; }
std::filesystem::path runtime_build_dir() { return data_dir() / "runtime-build"; }
std::filesystem::path projects_dir()      { return data_dir() / "projects"; }

std::filesystem::path expand_user(std::string_view raw) {
    if (raw.empty()) {
        return {};
    }

    std::filesystem::path path;
    if (raw == "~") {
        path = home_dir();
    } else if (raw.size() >= 2 && raw[0] == '~' && (raw[1] == '/')) {
        path = home_dir() / std::filesystem::path(std::string(raw.substr(2)));
    } else {
        path = std::filesystem::path(std::string(raw));
    }

    // weakly_canonical tolerates a path whose tail does not exist yet, which
    // matters for model files the user has not downloaded.
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : resolved;
}

}  // namespace crucible::paths
