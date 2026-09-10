// SPDX-License-Identifier: MIT
//
// What every test file needs: the includes, a temporary directory that cleans
// itself up, an XDG guard so a test cannot touch the real Crucible directory,
// and the few builders that would otherwise be copied into each file.
//
// Anything used by only one area stays in that area's file, in its own
// anonymous namespace. This header is for what genuinely crosses them.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <random>
#include <map>
#include <set>

#include <ggml.h>
#include <gguf.h>
#include <nlohmann/json.hpp>

#include "crucible/config/config.hpp"
#include "crucible/config/gpu_policy.hpp"
#include "crucible/routing/benchmark.hpp"
#include "crucible/routing/completion.hpp"
#include "crucible/engine/route_policy.hpp"
#include "crucible/llm/model_catalog.hpp"
#include "crucible/llm/model_shape.hpp"
#include "crucible/llm/response_filter.hpp"
#include "crucible/tools/web_search.hpp"
#include "crucible/cook/journal.hpp"
#include "crucible/util/diff.hpp"
#include "crucible/tools/workshop.hpp"
#include "crucible/util/markdown.hpp"
#include "crucible/util/resources.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/routing/router.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/routing/expert.hpp"
#include "crucible/runtime/backend.hpp"
#include "crucible/runtime/builder.hpp"
#include <llama.h>

#include "crucible/runtime/devices.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/session/store.hpp"
#include "crucible/session/usage.hpp"
#include "crucible/util/subprocess.hpp"
#include "crucible/util/text.hpp"
#include "crucible/config/trust.hpp"
#include "harness.hpp"
#include "roster_fixture.hpp"

using namespace crucible;

using namespace crucible;


/// A directory that cleans itself up, so tests never leave files behind.
/// Point the XDG data directory at a temporary place, so a test that reads or
/// writes a real Crucible directory cannot touch the one belonging to whoever is
/// running the suite.
class ScopedDataHome {
public:
    explicit ScopedDataHome(const std::filesystem::path& dir) {
        if (const char* existing = std::getenv("XDG_DATA_HOME"); existing != nullptr) {
            previous_ = existing;
            had_      = true;
        }
        ::setenv("XDG_DATA_HOME", dir.c_str(), 1);
    }
    ~ScopedDataHome() {
        if (had_) {
            ::setenv("XDG_DATA_HOME", previous_.c_str(), 1);
        } else {
            ::unsetenv("XDG_DATA_HOME");
        }
    }
    ScopedDataHome(const ScopedDataHome&)            = delete;
    ScopedDataHome& operator=(const ScopedDataHome&) = delete;

private:
    std::string previous_;
    bool        had_ = false;
};

class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path()
              / ("crucible-test-" + std::to_string(::getpid()) + "-"
                 + std::to_string(counter()++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    static int& counter() { static int n = 0; return n; }
    std::filesystem::path path_;
};

/// The shipped roster, shared by every test that needs one. Built once: it is
/// immutable, and the routers take it by shared_ptr anyway.
inline const std::shared_ptr<const Roster>& shipped() {
    static const std::shared_ptr<const Roster> roster =
        std::make_shared<const Roster>(testing::sample_roster());
    return roster;
}

inline ExpertId route_of(const std::string& prompt) {
    KeywordRouter router(shipped());
    return router.route(prompt, {}).expert;
}

/// A journal step, built by name. CookStep has grown a field in the middle
/// more than once, and a positional literal has to be edited every time.
inline CookStep step_of(int iteration, const char* expert, const char* kind, const char* summary,
                 bool ok = true, long ms = 0, std::vector<std::string> changed = {}) {
    CookStep step;
    step.iteration = iteration;
    step.expert    = expert;
    step.kind      = kind;
    step.summary   = summary;
    step.ok        = ok;
    step.ms        = ms;
    step.changed   = std::move(changed);
    return step;
}

/// One seat's state out of a snapshot, found by id rather than by index.
///
/// The seats vector is parallel to the roster the snapshot carries, and the
/// roster is no longer a fixed order known at compile time, so a test that
/// indexed by a hard-coded number would silently start reading its neighbor
/// the first time a seat moved.
inline SeatState seat_of(const Snapshot& snapshot, const ExpertId& id) {
    if (snapshot.roster) {
        if (const std::optional<std::size_t> row = snapshot.roster->find(id)) {
            if (*row < snapshot.seats.size()) {
                return snapshot.seats[*row];
            }
        }
    }
    return SeatState{};
}

/// Write a GGUF carrying just the keys read_model_shape looks at.
///
/// Built with ggml's own writer rather than hand-rolled bytes, so the test
/// exercises the same encoding a real model file uses. `kv_heads` is written
/// as a single value when it has one entry and as a per-layer array otherwise,
/// which is exactly how the two families of model in the wild spell it.
inline void write_test_gguf(const std::filesystem::path& file, std::uint32_t layers,
                     std::uint32_t embd, std::uint32_t heads,
                     const std::vector<std::int32_t>& kv_heads, std::uint32_t key_len,
                     std::uint32_t vocab) {
    gguf_context* gguf = gguf_init_empty();
    gguf_set_val_str(gguf, "general.architecture", "testarch");
    gguf_set_val_u32(gguf, "testarch.block_count", layers);
    gguf_set_val_u32(gguf, "testarch.embedding_length", embd);
    gguf_set_val_u32(gguf, "testarch.attention.head_count", heads);
    gguf_set_val_u32(gguf, "testarch.attention.key_length", key_len);
    gguf_set_val_u32(gguf, "testarch.attention.value_length", key_len);
    gguf_set_val_u32(gguf, "testarch.vocab_size", vocab);

    if (kv_heads.size() == 1) {
        gguf_set_val_u32(gguf, "testarch.attention.head_count_kv",
                         static_cast<std::uint32_t>(kv_heads.front()));
    } else {
        gguf_set_arr_data(gguf, "testarch.attention.head_count_kv", GGUF_TYPE_INT32,
                          kv_heads.data(), kv_heads.size());
    }

    gguf_write_to_file(gguf, file.string().c_str(), /*only_meta=*/true);
    gguf_free(gguf);
}

/// Write a GGUF with a real tensor table: a small hybrid model, one block in
/// `attention_every` carrying a K/V projection and the rest a recurrent state.
///
/// The metadata-only fixture above exercises the fallback path, where there is
/// nothing to read but hparams. This one exercises the path that matters for a
/// real model: what each block weighs, and which of them actually cache.
inline void write_test_gguf_with_tensors(const std::filesystem::path& file, std::uint32_t layers,
                                  std::uint32_t attention_every, std::uint32_t embd,
                                  std::uint32_t kv_width, std::uint32_t vocab) {
    ggml_init_params init{};
    init.mem_size   = static_cast<std::size_t>(layers + 8) * 4 * ggml_tensor_overhead();
    init.mem_buffer = nullptr;
    init.no_alloc   = true;  // shapes only; no tensor data is written
    ggml_context* ctx = ggml_init(init);

    gguf_context* gguf = gguf_init_empty();
    gguf_set_val_str(gguf, "general.architecture", "testarch");
    gguf_set_val_u32(gguf, "testarch.block_count", layers);
    gguf_set_val_u32(gguf, "testarch.embedding_length", embd);
    gguf_set_val_u32(gguf, "testarch.ssm.inner_size", 8);
    gguf_set_val_u32(gguf, "testarch.ssm.state_size", 4);

    const auto add = [&](const std::string& name, std::int64_t rows, std::int64_t columns) {
        ggml_tensor* tensor = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, rows, columns);
        ggml_set_name(tensor, name.c_str());
        gguf_add_tensor(gguf, tensor);
    };

    add("token_embd.weight", embd, vocab);
    add("output.weight", embd, vocab);
    add("output_norm.weight", embd, 1);
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        const std::string prefix = "blk." + std::to_string(layer) + ".";
        add(prefix + "attn_norm.weight", embd, 1);
        if (attention_every > 0 && (layer + 1) % attention_every == 0) {
            add(prefix + "attn_q.weight", embd, embd);
            add(prefix + "attn_k.weight", embd, kv_width);
            add(prefix + "attn_v.weight", embd, kv_width);
        } else {
            add(prefix + "ssm_conv1d.weight", 4, 8);
            add(prefix + "ssm_out.weight", embd, embd);
        }
    }

    gguf_write_to_file(gguf, file.string().c_str(), /*only_meta=*/true);
    gguf_free(gguf);
    ggml_free(ctx);
}
