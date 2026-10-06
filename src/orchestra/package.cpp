// SPDX-License-Identifier: MIT
//
// The orchestrator package, compiled in from scripts/orchestrator and written
// out beside Crucible's data. See link.hpp. The list here and the one in
// CMakeLists.txt name the same files; tests/test_orchestra.cpp checks that
// nothing in the package directory was left out of either.
#include "crucible/orchestra/link.hpp"

#include <fstream>
#include <system_error>

namespace crucible::orchestra {

namespace embedded {
extern const unsigned char kOrchestratorInit[];
extern const unsigned int  kOrchestratorInit_size;
extern const unsigned char kOrchestratorMain[];
extern const unsigned int  kOrchestratorMain_size;
extern const unsigned char kOrchestratorRun[];
extern const unsigned int  kOrchestratorRun_size;
extern const unsigned char kOrchestratorRpc[];
extern const unsigned int  kOrchestratorRpc_size;
extern const unsigned char kOrchestratorRouting[];
extern const unsigned int  kOrchestratorRouting_size;
extern const unsigned char kOrchestratorCook[];
extern const unsigned int  kOrchestratorCook_size;
extern const unsigned char kOrchestratorNaming[];
extern const unsigned int  kOrchestratorNaming_size;
}  // namespace embedded

namespace {

struct File {
    const char*          name;
    const unsigned char* data;
    const unsigned int*  size;
};

const File kFiles[] = {
    {"__init__.py", embedded::kOrchestratorInit, &embedded::kOrchestratorInit_size},
    {"__main__.py", embedded::kOrchestratorRun, &embedded::kOrchestratorRun_size},
    {"main.py", embedded::kOrchestratorMain, &embedded::kOrchestratorMain_size},
    {"rpc.py", embedded::kOrchestratorRpc, &embedded::kOrchestratorRpc_size},
    {"routing.py", embedded::kOrchestratorRouting, &embedded::kOrchestratorRouting_size},
    {"cook.py", embedded::kOrchestratorCook, &embedded::kOrchestratorCook_size},
    {"naming.py", embedded::kOrchestratorNaming, &embedded::kOrchestratorNaming_size},
};

}  // namespace

bool write_package(const std::filesystem::path& dir, std::string& error) {
    const std::filesystem::path package = dir / "crucible_orchestrator";
    std::error_code ec;
    std::filesystem::remove_all(package, ec);
    std::filesystem::create_directories(package, ec);
    if (ec) {
        error = "could not make " + package.string() + ": " + ec.message();
        return false;
    }
    for (const File& file : kFiles) {
        std::ofstream out(package / file.name, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data), static_cast<std::streamsize>(*file.size));
        if (!out) {
            error = "could not write " + (package / file.name).string();
            return false;
        }
    }
    return true;
}

}  // namespace crucible::orchestra
