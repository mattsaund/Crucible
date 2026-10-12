// SPDX-License-Identifier: MIT
//
// What `crucible --uninstall` may take with it.
//
// Most of what it removes is under the prefix or the XDG folders, which a
// test can point somewhere else. A Mac application is not: it is in
// /Applications whichever install made it, and the suite's uninstall of a copy
// in a sandbox took the real one with it on every run.
#include "test_helpers.hpp"

#include <fstream>

#include "crucible/app/uninstall.hpp"

using namespace crucible;

TEST(only_this_installs_mac_application_is_removed) {
    TempDir dir;
    const std::filesystem::path prefix = dir.path() / "prefix";
    const std::filesystem::path binary = prefix / "bin" / "crucible";
    std::filesystem::create_directories(binary.parent_path());
    { std::ofstream(binary) << "program"; }

    // install.sh's launcher, which execs this binary.
    const std::filesystem::path launcher = dir.path() / "Applications" / "Crucible.app";
    std::filesystem::create_directories(launcher / "Contents" / "MacOS");
    { std::ofstream(launcher / "Contents" / "MacOS" / "Crucible") << "#!/bin/sh\nexec \"" << binary.string() << "\" \"$@\"\n"; }
    CHECK(app_belongs_to(launcher, binary));
#if !defined(_WIN32)
    // Reached through a link, it is still this install.
    const std::filesystem::path linked = dir.path() / "linked";
    std::filesystem::create_directory_symlink(prefix, linked);
    CHECK(app_belongs_to(launcher, linked / "bin" / "crucible"));
#endif

    // The same launcher, asked about another install's binary: the sandboxed
    // copy the test suite uninstalls is not the one /Applications points at.
    const std::filesystem::path other = dir.path() / "sandbox" / "bin" / "crucible";
    std::filesystem::create_directories(other.parent_path());
    { std::ofstream(other) << "program"; }
    CHECK(!app_belongs_to(launcher, other));

    // The disk image's application: the binary is inside it.
    const std::filesystem::path bundle = dir.path() / "Image" / "Crucible.app";
    const std::filesystem::path inside = bundle / "Contents" / "MacOS" / "Crucible";
    std::filesystem::create_directories(inside.parent_path());
    { std::ofstream(inside) << "\xCF\xFA\xED\xFE program"; }
    CHECK(app_belongs_to(bundle, inside));
    CHECK(!app_belongs_to(bundle, binary));

    // Nothing there, or nothing asked about.
    CHECK(!app_belongs_to(dir.path() / "Missing.app", binary));
    CHECK(!app_belongs_to(launcher, {}));
}
