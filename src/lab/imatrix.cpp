// SPDX-License-Identifier: MIT
//
// Measuring an importance matrix. See imatrix.hpp.
//
// The collection is llama.cpp's own, from its llama-imatrix tool (MIT, the
// ggml authors): the scheduler is asked to show us every matrix product
// against one of the model's weights, and the squares of the values that went
// into it are added up per column, per expert where the weight is a mixture
// of them. What the quantizer is then given is each sum over the rows that
// made it.
#include "crucible/lab/imatrix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>

#include <ggml-backend.h>
#include <ggml.h>
#include <gguf.h>
#include <llama.h>
#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/platform.hpp"

namespace crucible::lab {
namespace {

using json = nlohmann::json;

/// One weight's running sums: `columns` per expert (one for a plain weight),
/// and how many rows each expert has seen.
struct Sums {
    std::vector<float>        values;
    std::vector<std::int64_t> counts;
};

struct Collector {
    std::mutex                  mutex;
    std::map<std::string, Sums> sums;
    std::vector<char>           off_card;   // a product's input, copied off a card
    std::vector<char>           ids;        // which experts each row went to
    bool                        failed = false;
};

/// A weight's own name from the name of a view of it: llama.cpp names some
/// of the tensors it computes with "...#blk.0.ffn_up.weight#...".
std::string weight_name(const char* name) {
    const char* start = std::strchr(name, '#');
    if (start == nullptr) {
        return name;
    }
    ++start;
    const char* end = std::strchr(start, '#');
    return end == nullptr ? std::string(start) : std::string(start, end);
}

bool finite(const float* values, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(values[i])) {
            return false;
        }
    }
    return true;
}

/// The scheduler's callback. Asked first whether a node is wanted; then,
/// for the ones that are, shown it once computed.
bool collect(ggml_tensor* node, bool ask, void* user) {
    auto& collector = *static_cast<Collector*>(user);
    const ggml_tensor* weight = node->src[0];
    const ggml_tensor* input  = node->src[1];
    if (ask) {
        if (node->op == GGML_OP_MUL_MAT_ID) {
            return true;
        }
        // The embeddings and the output are left out, as llama-imatrix leaves
        // them: the quantizer never asks for them. Small batches tell little.
        return node->op == GGML_OP_MUL_MAT && input->ne[1] >= 16 && input->type == GGML_TYPE_F32
            && weight_name(weight->name).rfind("blk.", 0) == 0;
    }

    const std::lock_guard<std::mutex> lock(collector.mutex);
    const std::string name = weight_name(weight->name);
    const char* data = static_cast<const char*>(input->data);
    if (!ggml_backend_buffer_is_host(input->buffer)) {
        collector.off_card.resize(ggml_nbytes(input));
        ggml_backend_tensor_get(input, collector.off_card.data(), 0, collector.off_card.size());
        data = collector.off_card.data();
    }
    const std::int64_t columns = input->ne[0];
    Sums& sums = collector.sums[name];

    if (node->op == GGML_OP_MUL_MAT_ID) {
        // input: [columns, experts used, tokens]; ids: [experts used, tokens].
        const ggml_tensor* ids = node->src[2];
        const std::int64_t experts = weight->ne[2];
        const std::int64_t used    = ids->ne[0];
        const std::int64_t tokens  = input->ne[2];
        if (sums.values.empty()) {
            sums.values.assign(static_cast<std::size_t>(columns * experts), 0.0F);
            sums.counts.assign(static_cast<std::size_t>(experts), 0);
        }
        if (sums.values.size() != static_cast<std::size_t>(columns * experts)) {
            collector.failed = true;
            return true;
        }
        collector.ids.resize(ggml_nbytes(ids));
        ggml_backend_tensor_get(ids, collector.ids.data(), 0, collector.ids.size());
        for (std::int64_t slot = 0; slot < used; ++slot) {
            for (std::int64_t token = 0; token < tokens; ++token) {
                const std::int32_t expert = *reinterpret_cast<const std::int32_t*>(
                    collector.ids.data() + token * ids->nb[1] + slot * ids->nb[0]);
                if (expert < 0 || expert >= experts) {
                    continue;
                }
                const float* x = reinterpret_cast<const float*>(
                    data + (slot % input->ne[1]) * input->nb[1] + token * input->nb[2]);
                float* into = sums.values.data() + expert * columns;
                for (std::int64_t j = 0; j < columns; ++j) {
                    into[j] += x[j] * x[j];
                }
                ++sums.counts[static_cast<std::size_t>(expert)];
            }
        }
    } else {
        // A plain weight, or a stack of them along its third and fourth
        // dimensions, each with a run of columns of its own.
        const std::int64_t stacked = weight->ne[2] * weight->ne[3];
        if (sums.values.empty()) {
            sums.values.assign(static_cast<std::size_t>(columns * stacked), 0.0F);
            sums.counts.assign(1, 0);
        }
        if (sums.values.size() != static_cast<std::size_t>(columns * stacked)) {
            collector.failed = true;
            return true;
        }
        for (std::int64_t i3 = 0; i3 < input->ne[3]; ++i3) {
            for (std::int64_t i2 = 0; i2 < input->ne[2]; ++i2) {
                const std::int64_t which = (i3 % weight->ne[3]) * weight->ne[2] + (i2 % weight->ne[2]);
                float* into = sums.values.data() + which * columns;
                for (std::int64_t row = 0; row < input->ne[1]; ++row) {
                    const float* x = reinterpret_cast<const float*>(
                        data + row * input->nb[1] + i2 * input->nb[2] + i3 * input->nb[3]);
                    for (std::int64_t j = 0; j < columns; ++j) {
                        into[j] += x[j] * x[j];
                    }
                }
            }
        }
        sums.counts[0] += ggml_nrows(input) / stacked;
    }
    if (!finite(sums.values.data(), sums.values.size())) {
        collector.failed = true;
    }
    return true;
}

/// The layers a GGUF has, from its header, or 0.
std::int64_t layers_in(const std::filesystem::path& model) {
    gguf_init_params params{};
    params.no_alloc = true;
    params.ctx      = nullptr;
    gguf_context* gguf = gguf_init_from_file(model.string().c_str(), params);
    if (gguf == nullptr) {
        return 0;
    }
    std::int64_t layers = 0;
    const std::int64_t arch = gguf_find_key(gguf, "general.architecture");
    if (arch >= 0 && gguf_get_kv_type(gguf, arch) == GGUF_TYPE_STRING) {
        const std::int64_t key = gguf_find_key(gguf, (std::string(gguf_get_val_str(gguf, arch)) + ".block_count").c_str());
        if (key >= 0 && gguf_get_kv_type(gguf, key) == GGUF_TYPE_UINT32) {
            layers = gguf_get_val_u32(gguf, key);
        }
    }
    gguf_free(gguf);
    return layers;
}

/// How many of the model's layers to put on the cards: all of them when
/// their free memory holds the whole model with room to compute, and a share
/// in proportion when it does not. A card two backends both list (CUDA and
/// Vulkan) is counted once, the way llama.cpp uses it once.
int layers_for_cards(const std::filesystem::path& model) {
    std::uint64_t free_memory = 0;
    int           cards       = 0;
    std::set<std::string> seen;
    for (ggml_backend_dev_t device : RuntimeRegistry::devices()) {
        if (ggml_backend_dev_type(device) != GGML_BACKEND_DEVICE_TYPE_GPU) {
            continue;
        }
        ggml_backend_dev_props props{};
        ggml_backend_dev_get_props(device, &props);
        if (props.device_id != nullptr && !seen.insert(props.device_id).second) {
            continue;
        }
        std::size_t free = 0;
        std::size_t total = 0;
        ggml_backend_dev_memory(device, &free, &total);
        free_memory += free;
        ++cards;
    }
    if (cards == 0) {
        return 0;
    }
    std::error_code ec;
    const std::uint64_t bytes = std::filesystem::file_size(model, ec);
    const std::uint64_t room  = std::uint64_t{768} << 20;   // compute buffers, per card
    const std::uint64_t spare = free_memory > room * cards ? free_memory - room * cards : 0;
    if (ec || bytes == 0 || spare >= bytes) {
        return -1;
    }
    const std::int64_t layers = layers_in(model);
    return layers <= 0 ? 0 : static_cast<int>(layers * static_cast<double>(spare) / static_cast<double>(bytes));
}

}  // namespace

std::vector<float> mean_squares(const std::vector<float>& sums, const std::vector<std::int64_t>& counts) {
    std::vector<float> out(sums.size(), 1.0F);
    if (counts.empty() || sums.size() % counts.size() != 0) {
        return out;
    }
    const std::size_t columns = sums.size() / counts.size();
    for (std::size_t expert = 0; expert < counts.size(); ++expert) {
        if (counts[expert] <= 0) {
            continue;   // never chosen: ones, which ask nothing of the quantizer
        }
        for (std::size_t j = 0; j < columns; ++j) {
            out[expert * columns + j] = sums[expert * columns + j] / static_cast<float>(counts[expert]);
        }
    }
    return out;
}

bool sample_text(std::string& text, std::string& error) {
    const std::filesystem::path kept = paths::data_dir() / "condense" / "wikitext-2.txt";
    std::error_code ec;
    if (std::filesystem::is_regular_file(kept, ec)) {
        std::ifstream in(kept, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (text.size() > 10000) {
            return true;
        }
    }
    // Eight pages of a hundred lines: about 280,000 characters, or 70,000
    // tokens, more than a hundred chunks of 512 take.
    text.clear();
    for (int offset = 0; offset < 800; offset += 100) {
        util::http::Request request;
        request.url = "https://datasets-server.huggingface.co/rows?dataset=Salesforce%2Fwikitext"
                      "&config=wikitext-2-raw-v1&split=train&offset="
                    + std::to_string(offset) + "&length=100";
        request.timeout_seconds = 60;
        const util::http::Response response = util::http::send(request);
        const json page = response.ok() ? json::parse(response.body, nullptr, false) : json();
        if (!page.is_object() || !page.contains("rows") || !page["rows"].is_array()) {
            error = "could not fetch the sample text: " + (response.ok() ? std::string("not rows") : response.reason());
            return false;
        }
        for (const json& row : page["rows"]) {
            if (row.contains("row") && row["row"].contains("text") && row["row"]["text"].is_string()) {
                text += row["row"]["text"].get<std::string>();
            }
        }
    }
    std::filesystem::create_directories(kept.parent_path(), ec);
    std::string ignored;
    util::write_file_atomically(kept, text, ignored);
    return true;
}

bool measure_importance(const std::filesystem::path& model, const std::string& text, ImportanceMatrix& out,
                        std::string& error, const std::function<void(int, int)>& progress,
                        const std::function<bool()>& cancel, int chunk_tokens, int max_chunks) {
    // The runtimes this process has not loaded yet: a child started for
    // condensing has quantized with nothing but the CPU until now.
    static std::once_flag loaded;
    std::call_once(loaded, [] {
        llama_backend_init();
        RuntimeRegistry::load_all();
    });

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = layers_for_cards(model);
    llama_model* loaded_model = llama_model_load_from_file(model.string().c_str(), model_params);
    if (loaded_model == nullptr) {
        error = "could not load " + model.filename().string() + " to measure it";
        return false;
    }

    Collector collector;
    llama_context_params context_params = llama_context_default_params();
    context_params.n_ctx             = static_cast<std::uint32_t>(chunk_tokens);
    context_params.n_batch           = static_cast<std::uint32_t>(chunk_tokens);
    context_params.n_ubatch          = static_cast<std::uint32_t>(chunk_tokens);
    context_params.cb_eval           = collect;
    context_params.cb_eval_user_data = &collector;
    context_params.no_perf           = true;
    llama_context* context = llama_init_from_model(loaded_model, context_params);
    if (context == nullptr) {
        llama_model_free(loaded_model);
        error = "could not make room to measure " + model.filename().string();
        return false;
    }

    const llama_vocab* vocab = llama_model_get_vocab(loaded_model);
    const int wanted = -llama_tokenize(vocab, text.data(), static_cast<std::int32_t>(text.size()), nullptr, 0, false, false);
    std::vector<llama_token> tokens(static_cast<std::size_t>(std::max(wanted, 0)));
    if (wanted <= 0
        || llama_tokenize(vocab, text.data(), static_cast<std::int32_t>(text.size()), tokens.data(),
                          static_cast<std::int32_t>(tokens.size()), false, false) < 0) {
        llama_free(context);
        llama_model_free(loaded_model);
        error = "could not read the sample text";
        return false;
    }

    // Chunks that start where a document would, with the model's own first
    // token when it has one, as llama-imatrix does. Every token asks for its
    // output: llama.cpp otherwise takes only the last one through the last
    // layer, and that layer's weights would go unmeasured.
    const bool bos   = llama_vocab_get_add_bos(vocab);
    const int  body  = chunk_tokens - (bos ? 1 : 0);
    const int  total = std::min(max_chunks, static_cast<int>(tokens.size()) / body);
    llama_batch batch = llama_batch_init(chunk_tokens, 0, 1);
    int done = 0;
    for (; done < total; ++done) {
        if (cancel && cancel()) {
            break;
        }
        batch.n_tokens = chunk_tokens;
        for (int i = 0; i < chunk_tokens; ++i) {
            batch.token[i]     = bos && i == 0 ? llama_vocab_bos(vocab)
                                               : tokens[static_cast<std::size_t>(done) * body + i - (bos ? 1 : 0)];
            batch.pos[i]       = i;
            batch.n_seq_id[i]  = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i]    = 1;
        }
        llama_memory_clear(llama_get_memory(context), true);
        if (llama_decode(context, batch) != 0) {
            error = "llama.cpp could not run " + model.filename().string() + " over the sample text";
            break;
        }
        if (collector.failed) {
            error = "the measurement went out of range in " + model.filename().string();
            break;
        }
        if (progress) {
            progress(done + 1, total);
        }
    }
    llama_batch_free(batch);
    llama_free(context);
    llama_model_free(loaded_model);
    if (!error.empty()) {
        return false;
    }
    if (done < total) {
        error = "stopped";
        return false;
    }
    if (done == 0) {
        error = "the sample text is too short to measure with";
        return false;
    }
    out.values.clear();
    for (const auto& [name, sums] : collector.sums) {
        out.values[name] = mean_squares(sums.values, sums.counts);
    }
    out.chunks = done;
    return true;
}

std::filesystem::path importance_file(const std::filesystem::path& model) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(model, ec);
    const auto stamp = std::filesystem::last_write_time(model, ec).time_since_epoch().count();
    // FNV-1a over what identifies the file: short, and the same every time.
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : model.string() + "|" + std::to_string(bytes) + "|" + std::to_string(stamp)) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.imatrix", static_cast<unsigned long long>(hash));
    return paths::data_dir() / "condense" / "importance" / name;
}

// A file of its own shape, read back only here: a magic word, how many
// chunks, then each weight as its name and its values.
namespace {
constexpr char kMagic[8] = {'C', 'R', 'I', 'M', 'A', 'T', '1', '\n'};
}

bool save_importance(const ImportanceMatrix& matrix, const std::filesystem::path& file, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::string bytes(kMagic, sizeof(kMagic));
    const auto put = [&bytes](const void* data, std::size_t n) { bytes.append(static_cast<const char*>(data), n); };
    const auto chunks  = static_cast<std::uint32_t>(matrix.chunks);
    const auto entries = static_cast<std::uint32_t>(matrix.values.size());
    put(&chunks, sizeof(chunks));
    put(&entries, sizeof(entries));
    for (const auto& [name, values] : matrix.values) {
        const auto length = static_cast<std::uint32_t>(name.size());
        const auto count  = static_cast<std::uint64_t>(values.size());
        put(&length, sizeof(length));
        put(name.data(), name.size());
        put(&count, sizeof(count));
        put(values.data(), values.size() * sizeof(float));
    }
    return util::write_file_atomically(file, bytes, error);
}

bool load_importance(const std::filesystem::path& file, ImportanceMatrix& matrix) {
    std::ifstream in(file, std::ios::binary);
    char magic[sizeof(kMagic)] = {};
    if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        return false;
    }
    std::uint32_t chunks = 0;
    std::uint32_t entries = 0;
    if (!in.read(reinterpret_cast<char*>(&chunks), sizeof(chunks)) || !in.read(reinterpret_cast<char*>(&entries), sizeof(entries))) {
        return false;
    }
    ImportanceMatrix read;
    read.chunks = static_cast<int>(chunks);
    for (std::uint32_t i = 0; i < entries; ++i) {
        std::uint32_t length = 0;
        std::uint64_t count  = 0;
        if (!in.read(reinterpret_cast<char*>(&length), sizeof(length)) || length > 4096) {
            return false;
        }
        std::string name(length, '\0');
        if (!in.read(name.data(), length) || !in.read(reinterpret_cast<char*>(&count), sizeof(count))
            || count > (std::uint64_t{1} << 32)) {
            return false;
        }
        std::vector<float> values(static_cast<std::size_t>(count));
        if (!in.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(count * sizeof(float)))) {
            return false;
        }
        read.values.emplace(std::move(name), std::move(values));
    }
    matrix = std::move(read);
    return true;
}

}  // namespace crucible::lab
