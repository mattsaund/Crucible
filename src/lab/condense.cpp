// SPDX-License-Identifier: MIT
//
// Condensing a model. See condense.hpp for why it runs as a child.
#include "crucible/lab/condense.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <system_error>
#include <thread>

#include <gguf.h>
#include <llama.h>
#include <nlohmann/json.hpp>

#include "crucible/lab/imatrix.hpp"
#include "crucible/lab/recipe.hpp"
#include "crucible/util/format.hpp"

namespace crucible::lab {
namespace {

using json = nlohmann::json;

struct Type {
    const char* name;
    llama_ftype ftype;
    double      bits;
    const char* note;
    bool        samples = false;   // made with an importance matrix only
};

/// Offered in the Create tab, best first. `bits` is about what each lands at
/// on a large model, for ordering and for saying no to a pointless one; the
/// sizes shown come from a dry run, not from this.
constexpr Type kOffered[] = {
    {"Q8_0",   LLAMA_FTYPE_MOSTLY_Q8_0,   8.50, "near original"},
    {"Q6_K",   LLAMA_FTYPE_MOSTLY_Q6_K,   6.56, "very close"},
    {"Q5_K_M", LLAMA_FTYPE_MOSTLY_Q5_K_M, 5.69, "close"},
    {"Q5_K_S", LLAMA_FTYPE_MOSTLY_Q5_K_S, 5.54, "close"},
    {"Q4_K_M", LLAMA_FTYPE_MOSTLY_Q4_K_M, 4.85, "balanced"},
    {"Q4_K_S", LLAMA_FTYPE_MOSTLY_Q4_K_S, 4.58, "balanced, smaller"},
    {"IQ4_XS", LLAMA_FTYPE_MOSTLY_IQ4_XS, 4.25, "some loss"},
    {"Q3_K_L", LLAMA_FTYPE_MOSTLY_Q3_K_L, 4.27, "some loss"},
    {"Q3_K_M", LLAMA_FTYPE_MOSTLY_Q3_K_M, 3.91, "noticeable loss"},
    {"Q3_K_S", LLAMA_FTYPE_MOSTLY_Q3_K_S, 3.50, "noticeable loss"},
    // IQ3_XXS before Q2_K: a little larger (the 27B Qwen came out 11.3 GB to
    // 10.9) and the better of the two, as llama.cpp ranks them.
    {"IQ3_XXS", LLAMA_FTYPE_MOSTLY_IQ3_XXS, 3.06, "large loss",      true},
    {"Q2_K",   LLAMA_FTYPE_MOSTLY_Q2_K,   3.35, "large loss"},
    {"IQ2_M",   LLAMA_FTYPE_MOSTLY_IQ2_M,   2.70, "very large loss", true},
    {"IQ2_S",   LLAMA_FTYPE_MOSTLY_IQ2_S,   2.50, "very large loss", true},
    {"IQ2_XS",  LLAMA_FTYPE_MOSTLY_IQ2_XS,  2.31, "very large loss", true},
    {"IQ2_XXS", LLAMA_FTYPE_MOSTLY_IQ2_XXS, 2.06, "extreme loss",    true},
    {"IQ1_M",   LLAMA_FTYPE_MOSTLY_IQ1_M,   1.75, "extreme loss",    true},
    {"IQ1_S",   LLAMA_FTYPE_MOSTLY_IQ1_S,   1.56, "extreme loss",    true},
};

/// Accepted on the command line as well: what the fine-tuner exports at.
constexpr Type kAlso[] = {
    {"F16",    LLAMA_FTYPE_MOSTLY_F16,    16.0, ""},
    {"BF16",   LLAMA_FTYPE_MOSTLY_BF16,   16.0, ""},
    {"Q4_0",   LLAMA_FTYPE_MOSTLY_Q4_0,   4.55, ""},
    {"Q4_1",   LLAMA_FTYPE_MOSTLY_Q4_1,   5.0,  ""},
    {"Q5_0",   LLAMA_FTYPE_MOSTLY_Q5_0,   5.5,  ""},
    {"Q5_1",   LLAMA_FTYPE_MOSTLY_Q5_1,   6.0,  ""},
    {"IQ4_NL", LLAMA_FTYPE_MOSTLY_IQ4_NL, 4.5,  ""},
};

const Type* type_named(std::string_view name) {
    for (const Type& type : kOffered) {
        if (name == type.name) {
            return &type;
        }
    }
    for (const Type& type : kAlso) {
        if (name == type.name) {
            return &type;
        }
    }
    return nullptr;
}

/// The header's file type, by the name a file would carry. Only the common
/// ones: anything else is read from the name, or left unnamed.
std::string ftype_name(std::uint32_t ftype) {
    for (const Type& type : kOffered) {
        if (static_cast<std::uint32_t>(type.ftype) == ftype) {
            return type.name;
        }
    }
    for (const Type& type : kAlso) {
        if (static_cast<std::uint32_t>(type.ftype) == ftype) {
            return type.name;
        }
    }
    if (ftype == 0) {
        return "F32";
    }
    if (ftype == static_cast<std::uint32_t>(LLAMA_FTYPE_MOSTLY_MXFP4_MOE)) {
        return "MXFP4";
    }
    return {};
}

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

/// "-00001-of-00003" at the end of a split model's first part, or nothing.
std::string_view without_part(std::string_view stem) {
    static const std::regex part(R"(-\d{5}-of-\d{5}$)");
    const std::string copy(stem);
    std::smatch match;
    if (std::regex_search(copy, match, part)) {
        return stem.substr(0, static_cast<std::size_t>(match.position(0)));
    }
    return stem;
}

/// One whole line on standard output. Locked, because progress is told from
/// inside llama.cpp's log callback, and two halves of two lines would be
/// neither.
std::mutex g_out;
void write_line(const std::string& text) {
    const std::lock_guard<std::mutex> lock(g_out);
    std::cout << text << '\n';
    std::cout.flush();
}

/// One event line on standard output, which the trainer's reader parses.
/// Everything else the child says goes to standard error. Only --condense
/// speaks in events, and --quantize-sizes answers in one such line; see
/// condense_main for why --quantize does neither.
void emit(const json& event) {
    write_line(event.dump(-1, ' ', false, json::error_handler_t::replace));
}

/// What llama.cpp's quantizer says as it goes: one line per tensor that
/// starts "[  12/ 779]", and at the end the size it came out at. Read from
/// its log because that is the only place it says either.
struct Listening {
    std::function<void(int, int)> progress;
    double                        quant_mib = 0.0;
    std::string                   failure;
    bool                          verbose = false;
};

void listen(ggml_log_level level, const char* text, void* user) {
    if (text == nullptr || user == nullptr) {
        return;
    }
    auto* listening = static_cast<Listening*>(user);
    int done  = 0;
    int total = 0;
    if (text[0] == '[' && std::sscanf(text, "[%d/%d]", &done, &total) == 2 && total > 0) {
        if (listening->progress) {
            listening->progress(done, total);
        }
    }
    if (const char* at = std::strstr(text, "quant size"); at != nullptr) {
        double mib = 0.0;
        if (std::sscanf(at, "quant size = %lf", &mib) == 1
            || std::sscanf(at, "quant size  = %lf", &mib) == 1) {
            listening->quant_mib = mib;
        }
    }
    if (level == GGML_LOG_LEVEL_ERROR) {
        std::string line(text);
        while (!line.empty() && (line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        const std::size_t colon = line.find("failed to quantize: ");
        if (colon != std::string::npos) {
            listening->failure = line.substr(colon + 20);
        } else if (listening->failure.empty() && !line.empty() && line.find_first_not_of("=\n ") != std::string::npos) {
            listening->failure = line;
        }
    }
    if (listening->verbose || level >= GGML_LOG_LEVEL_WARN) {
        std::cerr << text;
    }
}

std::uint64_t file_bytes(const std::filesystem::path& file) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

/// Every part of a split model, for its total size. The first part is what
/// llama.cpp is handed; it finds the rest beside it.
std::uint64_t all_parts(const std::filesystem::path& first) {
    const std::string stem = first.stem().string();
    static const std::regex part(R"(^(.*)-00001-of-(\d{5})$)");
    std::smatch match;
    if (!std::regex_match(stem, match, part)) {
        return file_bytes(first);
    }
    const int count = std::stoi(match[2].str());
    std::uint64_t total = 0;
    for (int i = 1; i <= count; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "-%05d-of-%05d", i, count);
        total += file_bytes(first.parent_path() / (match[1].str() + name + first.extension().string()));
    }
    return total;
}

int fail(const std::string& message, const std::string& hint = {}) {
    json event{{"event", "failed"}, {"message", message}};
    if (!hint.empty()) {
        event["hint"] = hint;
    }
    emit(event);
    return 1;
}

std::string read_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// --condense: a recipe from the Create tab, reported the way a fine-tune is.
int run_recipe(const std::filesystem::path& recipe_file, const std::filesystem::path& export_dir) {
    Recipe      recipe;
    std::string error;
    if (!parse(read_file(recipe_file), recipe, error)) {
        return fail("could not read the recipe: " + error);
    }
    const std::filesystem::path source(recipe.base.path.empty() ? recipe.base.id : recipe.base.path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        return fail(source.string() + " is not there any more");
    }
    if (!known_quantization(recipe.quantization)) {
        return fail(recipe.quantization + " is not a quantization this can make");
    }

    emit({{"event", "phase"}, {"name", "measuring"}, {"detail", source.filename().string()}});
    const Original now = read_source(source);
    if (!now.ok) {
        return fail(now.error);
    }
    if (!worth_condensing(now, recipe.quantization)) {
        return fail(source.filename().string() + " is already this small",
                    "Pick a smaller quantization.");
    }
    // A name nothing has yet. The one it would be called may be a file the
    // person downloaded at that size, or on a disk that ignores case, the
    // original itself; either was written over when this took the name.
    const std::filesystem::path out = free_path(condensed_path(source, export_dir, recipe.quantization));
    if (out.empty()) {
        return fail("there is no free name for it in " + source.parent_path().string());
    }
    const Quantized sized = quantize(source, out, recipe.quantization, true);
    if (!sized.ok) {
        return fail(sized.error);
    }
    if (!shrinks_enough(now.bits, static_cast<double>(sized.bytes) * 8.0 / static_cast<double>(now.elements))) {
        return fail(source.filename().string() + " would come out nearly as big",
                    "Its weights do not fit " + recipe.quantization + "'s blocks. Pick another size.");
    }

    // Room for it, and for the partial file to be written beside the source
    // while the source is still there.
    const std::filesystem::space_info space = std::filesystem::space(out.parent_path(), ec);
    const std::uint64_t needed = sized.bytes + (std::uint64_t{256} << 20);
    if (!ec && space.available < needed) {
        return fail("needs " + format::bytes(needed) + " free where it goes, and there is "
                        + format::bytes(space.available),
                    "Make room in " + out.parent_path().string() + ", or pick a smaller quantization.");
    }

    // The lowest-bit types are made with an importance matrix: the model run
    // over sample text first, on the cards. Measured once per model file and
    // kept, so a second size of the same model starts at once.
    ImportanceMatrix importance;
    const bool sampling = samples_first(recipe.quantization);
    if (sampling) {
        const std::filesystem::path kept = importance_file(source);
        if (!load_importance(kept, importance)) {
            emit({{"event", "phase"}, {"name", "sampling"}, {"detail", "running it over sample text"}});
            std::string text;
            if (!sample_text(text, error)) {
                return fail(error, "It needs Hugging Face once, for the sample text.");
            }
            int shown = -1;
            if (!measure_importance(source, text, importance, error, [&shown](int done, int total) {
                    if (done != shown) {
                        shown = done;
                        emit({{"event", "progress"}, {"done", done}, {"total", total}});
                    }
                })) {
                return fail(error);
            }
            save_importance(importance, kept, error);   // kept for next time; a failure only costs a re-run
            error.clear();
        }
    }

    // Where the unfinished file is, said before there is one: stopped, this
    // process is gone before it could tidy up, and the trainer removes it.
    const std::string from = now.quantization.empty() ? std::string("the original") : now.quantization;
    emit({{"event", "phase"}, {"name", "condensing"},
          {"detail", from + " to " + recipe.quantization + ", about " + format::bytes(sized.bytes)},
          {"partial", part_of(out).string()}});
    int last = -1;
    const Quantized made = quantize(source, out, recipe.quantization, false, [&last](int done, int total) {
        // One event per tensor at most, and none when nothing moved.
        if (done != last) {
            last = done;
            emit({{"event", "progress"}, {"done", done}, {"total", total}});
        }
    }, sampling ? &importance : nullptr);
    if (!made.ok) {
        return fail(made.error);
    }
    emit({{"event", "done"}, {"path", made.path.string()}, {"gguf", true}, {"bytes", made.bytes}});
    return 0;
}

/// --quantize-sizes: what the file is now, and what each type would weigh.
int run_sizes(const std::filesystem::path& source, const std::string& list) {
    const Original now = read_source(source);
    json out{{"ok", now.ok}, {"error", now.error}, {"bytes", now.bytes}, {"elements", now.elements},
             {"bits", now.bits}, {"quantization", now.quantization}, {"type", now.type}};
    json sizes = json::object();
    std::stringstream names(list);
    std::string       name;
    // A name for the dry runs, which write nothing to it, somewhere that is
    // not the person's models folder in case one ever did.
    std::error_code ec;
    const std::filesystem::path nowhere = std::filesystem::temp_directory_path(ec) / "crucible-dry-run.gguf";
    while (now.ok && std::getline(names, name, ',')) {
        if (!known_quantization(name)) {
            continue;
        }
        const Quantized sized = quantize(source, nowhere, name, true);
        sizes[name] = sized.ok ? json(sized.bytes) : json(nullptr);
    }
    out["sizes"] = std::move(sizes);
    emit(out);
    return now.ok ? 0 : 1;
}

}  // namespace

const std::vector<Quantization>& quantizations() {
    static const std::vector<Quantization> list = [] {
        std::vector<Quantization> out;
        for (const Type& type : kOffered) {
            out.push_back({type.name, type.bits, type.note, type.samples});
        }
        return out;
    }();
    return list;
}

bool known_quantization(std::string_view name) {
    return type_named(name) != nullptr;
}

bool samples_first(std::string_view name) {
    const Type* type = type_named(name);
    return type != nullptr && type->samples;
}

std::string quantization_in_name(std::string_view file_name) {
    std::string_view stem = file_name;
    if (const std::size_t dot = stem.rfind('.'); dot != std::string_view::npos) {
        stem = stem.substr(0, dot);
    }
    stem = without_part(stem);
    // The last piece after a dash or a dot that reads as a quantization:
    // Q4_K_M, IQ4_XS, F16, BF16, MXFP4, and Unsloth's UD- versions of them.
    static const std::regex quant(
        R"((?:^|[-._])((?:UD-)?(?:I?Q\d(?:_[A-Z0-9]+)*|B?F16|F32|MXFP4(?:_MOE)?))$)",
        std::regex::icase);
    const std::string copy(stem);
    std::smatch match;
    if (std::regex_search(copy, match, quant)) {
        return upper(match[1].str());
    }
    return {};
}

std::string condensed_name(std::string_view file_name, std::string_view quantization) {
    std::string_view stem = file_name;
    if (const std::size_t dot = stem.rfind('.'); dot != std::string_view::npos) {
        stem = stem.substr(0, dot);
    }
    stem = without_part(stem);
    std::string base(stem);
    const std::string had = quantization_in_name(file_name);
    if (!had.empty() && base.size() >= had.size()) {
        // Case as the file had it: compared in capitals, cut where it starts.
        const std::string tail = upper(base.substr(base.size() - had.size()));
        if (tail == had) {
            base.erase(base.size() - had.size());
            while (!base.empty() && (base.back() == '-' || base.back() == '.' || base.back() == '_')) {
                base.pop_back();
            }
        }
    }
    if (base.empty()) {
        base = "model";
    }
    return base + "-" + std::string(quantization) + ".gguf";
}

std::filesystem::path condensed_path(const std::filesystem::path& source,
                                     const std::filesystem::path& models_dir,
                                     std::string_view quantization) {
    const std::string name = condensed_name(source.filename().string(), quantization);
    std::error_code ec;
    const std::filesystem::path root   = std::filesystem::weakly_canonical(models_dir, ec);
    const std::filesystem::path folder = std::filesystem::weakly_canonical(source.parent_path(), ec);
    const auto rel = folder.lexically_relative(root);
    const bool inside = !root.empty() && !rel.empty() && *rel.begin() != "..";
    return (inside ? source.parent_path() : models_dir) / name;
}

std::filesystem::path part_of(const std::filesystem::path& out) {
    return out.string() + ".part";
}

std::filesystem::path free_path(const std::filesystem::path& wanted) {
    const auto taken = [](const std::filesystem::path& file) {
        std::error_code ec;
        return std::filesystem::exists(file, ec) || std::filesystem::exists(part_of(file), ec);
    };
    if (!taken(wanted)) {
        return wanted;
    }
    // "flint-" and "Q4_K_M", so the number goes between and the name still
    // ends in its size: the pickers and the next condense read it from there.
    const std::string stem      = wanted.stem().string();
    const std::string extension = wanted.extension().string();
    const std::string quant     = quantization_in_name(wanted.filename().string());
    std::string head = stem;
    std::string tail;
    if (!quant.empty() && stem.size() > quant.size()
        && upper(stem.substr(stem.size() - quant.size())) == quant) {
        head = stem.substr(0, stem.size() - quant.size());
        tail = stem.substr(stem.size() - quant.size());
    }
    // The separator the name already uses, on both sides of the number.
    std::string separator = "-";
    if (!tail.empty() && !head.empty() && (head.back() == '-' || head.back() == '.' || head.back() == '_')) {
        separator = std::string(1, head.back());
        head.pop_back();
    }
    for (int n = 2; n < 1000; ++n) {
        const std::string name = tail.empty() ? head + separator + std::to_string(n)
                                              : head + separator + std::to_string(n) + separator + tail;
        const std::filesystem::path candidate = wanted.parent_path() / (name + extension);
        if (!taken(candidate)) {
            return candidate;
        }
    }
    return {};
}

Original read_source(const std::filesystem::path& file) {
    Original out;
    out.bytes = all_parts(file);
    if (out.bytes == 0) {
        out.error = file.string() + " is not there";
        return out;
    }
    gguf_init_params params{};
    params.no_alloc = true;
    params.ctx      = nullptr;
    gguf_context* gguf = gguf_init_from_file(file.string().c_str(), params);
    if (gguf == nullptr) {
        out.error = file.filename().string() + " is not a GGUF model";
        return out;
    }
    // Counted over the tensors in this part. A split model's other parts hold
    // the rest, so the count is scaled by how much of the bytes this part is:
    // close enough for an average, which is all it is used for.
    std::uint64_t here = 0;
    for (std::int64_t i = 0; i < gguf_get_n_tensors(gguf); ++i) {
        const std::int64_t* ne = gguf_get_tensor_ne(gguf, i);
        if (ne == nullptr) {
            continue;
        }
        std::uint64_t n = 1;
        for (int d = 0; d < 4; ++d) {
            n *= static_cast<std::uint64_t>(std::max<std::int64_t>(1, ne[d]));
        }
        here += n;
    }
    std::uint32_t ftype = UINT32_MAX;
    if (const std::int64_t key = gguf_find_key(gguf, "general.file_type");
        key >= 0 && gguf_get_kv_type(gguf, key) == GGUF_TYPE_UINT32) {
        ftype = gguf_get_val_u32(gguf, key);
    }
    gguf_free(gguf);

    const std::uint64_t part = file_bytes(file);
    out.elements = part > 0 && out.bytes > part
                       ? static_cast<std::uint64_t>(static_cast<double>(here) * static_cast<double>(out.bytes)
                                                    / static_cast<double>(part))
                       : here;
    if (out.elements == 0) {
        out.error = file.filename().string() + " has no weights in it";
        return out;
    }
    out.bits         = static_cast<double>(out.bytes) * 8.0 / static_cast<double>(out.elements);
    out.type         = ftype != UINT32_MAX ? ftype_name(ftype) : std::string();
    out.quantization = quantization_in_name(file.filename().string());
    if (out.quantization.empty()) {
        out.quantization = out.type;
    }
    out.ok = true;
    return out;
}

bool worth_condensing(const Original& source, std::string_view quantization) {
    const Type* type = type_named(upper(quantization));
    if (type == nullptr) {
        return false;
    }
    // Never to what it already is, by whichever of its name and its header
    // says so. That is the same file again, under the same name.
    const std::string_view name(type->name);
    if (name == upper(source.quantization) || name == upper(source.type)) {
        return false;
    }
    // Like against like where the source's type is known, the header's first
    // since it is what llama.cpp wrote. The measured average only where it is
    // not, as for MXFP4 or Unsloth's mixtures.
    double bits = source.bits;
    for (const std::string& known : {source.type, source.quantization}) {
        if (const Type* at = type_named(upper(known)); at != nullptr) {
            bits = at->bits;
            break;
        }
    }
    return shrinks_enough(bits, type->bits);
}

bool shrinks_enough(double source_bits, double result_bits) {
    // A quarter of a bit a weight is the least worth a pass over the whole
    // model: less than that and the file barely shrinks while the quality of
    // a second rounding is already spent.
    return source_bits <= 0.0 || result_bits < source_bits - 0.25;
}

Quantized quantize(const std::filesystem::path& in, const std::filesystem::path& out,
                   std::string_view quantization, bool dry_run,
                   const std::function<void(int, int)>& progress, const ImportanceMatrix* importance) {
    Quantized result;
    const Type* type = type_named(quantization);
    if (type == nullptr) {
        result.error = std::string(quantization) + " is not a quantization this can make";
        return result;
    }
    if (type->samples && importance == nullptr && !dry_run) {
        result.error = std::string(quantization) + " needs the model sampled first";
        return result;
    }
    // Never over what it reads. Asked of the disk, which knows whether two
    // spellings of a name are one file; a comparison of the text does not.
    std::error_code ec;
    if (!dry_run && std::filesystem::equivalent(in, out, ec)) {
        result.error = "it would be written over " + in.filename().string() + ", the model it is made from";
        return result;
    }
    const bool was_there = !dry_run && std::filesystem::exists(out, ec);
    // The matrix as llama.cpp reads it: name, values and count for each
    // weight, and an empty one to end the list.
    std::vector<llama_model_imatrix_data> matrix;
    if (importance != nullptr) {
        matrix.reserve(importance->values.size() + 1);
        for (const auto& [name, values] : importance->values) {
            matrix.push_back({name.c_str(), values.data(), values.size()});
        }
        matrix.push_back({nullptr, nullptr, 0});
    } else if (dry_run && type->samples) {
        // Sized as it will be made. Without a matrix llama.cpp picks larger
        // types for some weights (IQ3_S where IQ3_XXS needs one), and an
        // IQ3_XXS was measured bigger than a Q2_K. A dry run reads no values,
        // only whether there are any, so an empty list is enough.
        matrix.push_back({nullptr, nullptr, 0});
    }

    Listening listening;
    listening.progress = progress;
    llama_log_set(listen, &listening);

    llama_model_quantize_params params = llama_model_quantize_default_params();
    params.ftype            = type->ftype;
    // From a quantized file as much as from a full-precision one: that is
    // the whole point here. Q8_0 rounds to Q4_K_M with little lost beyond
    // what Q4_K_M loses from the original.
    params.allow_requantize = true;
    params.dry_run          = dry_run;
    params.imatrix          = matrix.empty() ? nullptr : matrix.data();
    // All but one core: quantizing is work for every one of them, and the
    // window and whatever else is open still have to be drawn.
    const unsigned cores = std::thread::hardware_concurrency();
    params.nthread = static_cast<int32_t>(cores > 1 ? cores - 1 : 1);

    const std::filesystem::path part = part_of(out);
    if (!dry_run) {
        std::filesystem::create_directories(out.parent_path(), ec);
        std::filesystem::remove(part, ec);
    }
    const std::uint32_t status =
        llama_model_quantize(in.string().c_str(), (dry_run ? out : part).string().c_str(), &params);
    llama_log_set(nullptr, nullptr);

    if (status != 0) {
        std::filesystem::remove(part, ec);
        result.error = listening.failure.empty() ? "llama.cpp could not quantize " + in.filename().string()
                                                 : listening.failure;
        return result;
    }
    if (dry_run) {
        result.ok    = listening.quant_mib > 0.0;
        result.bytes = static_cast<std::uint64_t>(listening.quant_mib * 1024.0 * 1024.0);
        result.path  = out;
        if (!result.ok) {
            result.error = "llama.cpp did not say how large it would be";
        }
        return result;
    }
    // Something there now that was not when this started arrived while it
    // worked, a download finishing say, and is not this run's to replace.
    std::filesystem::path target = out;
    if (!was_there && std::filesystem::exists(out, ec)) {
        target = free_path(out);
    }
    if (target.empty()) {
        std::filesystem::remove(part, ec);
        result.error = "there is no free name for it beside " + out.string();
        return result;
    }
    std::filesystem::rename(part, target, ec);
    if (ec) {
        std::filesystem::remove(part, ec);
        result.error = "could not put it at " + target.string() + ": " + ec.message();
        return result;
    }
    result.ok    = true;
    result.bytes = file_bytes(target);
    result.path  = target;
    return result;
}

bool is_condense_command(int argc, char** argv) {
    if (argc < 2 || argv[1] == nullptr) {
        return false;
    }
    const std::string_view first(argv[1]);
    return first == "--condense" || first == "--quantize" || first == "--quantize-sizes";
}

int condense_main(const std::vector<std::string>& args) {
    if (args.empty()) {
        return 2;
    }
    llama_backend_init();
    const std::string& mode = args[0];

    if (mode == "--condense") {
        std::filesystem::path recipe;
        std::filesystem::path export_dir;
        for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
            if (args[i] == "--recipe")     { recipe     = args[i + 1]; }
            if (args[i] == "--export-dir") { export_dir = args[i + 1]; }
        }
        if (recipe.empty() || export_dir.empty()) {
            std::cerr << "crucible: --condense needs --recipe FILE and --export-dir DIR\n";
            return 2;
        }
        const int status = run_recipe(recipe, export_dir);
        llama_backend_free();
        return status;
    }

    if (mode == "--quantize-sizes") {
        if (args.size() < 3) {
            std::cerr << "crucible: --quantize-sizes needs a model and a list of types\n";
            return 2;
        }
        const int status = run_sizes(args[1], args[2]);
        llama_backend_free();
        return status;
    }

    // --quantize IN OUT TYPE [--dry-run]: llama-quantize's arguments.
    if (args.size() < 4) {
        std::cerr << "usage: crucible --quantize IN.gguf OUT.gguf TYPE [--dry-run]\n"
                     "  TYPE is one of Q8_0 Q6_K Q5_K_M Q5_K_S Q4_K_M Q4_K_S IQ4_XS Q3_K_L Q3_K_M Q3_K_S Q2_K,\n"
                     "  or F16, BF16, Q4_0, Q5_0\n";
        return 2;
    }
    // Plain lines, the way llama-quantize talks, and not the events
    // --condense speaks. This is the fine-tuner's quantize step: finetune.py
    // passes what it prints on into the stream the trainer reads, and a
    // "done" from here once finished the fine-tune at this file in its run
    // folder, a moment before the file was moved into the models folder.
    const bool dry   = args.size() > 4 && args[4] == "--dry-run";
    int        shown = -1;
    const Quantized made = quantize(args[1], args[2], upper(args[3]), dry, [&shown](int done, int total) {
        // A line every tenth of the way: this is read as a log, not drawn.
        const int tenth = done * 10 / total;
        if (tenth != shown) {
            shown = tenth;
            write_line("quantized " + std::to_string(done) + " of " + std::to_string(total) + " tensors");
        }
    });
    llama_backend_free();
    if (!made.ok) {
        std::cerr << "crucible: " << made.error << "\n";
        return 1;
    }
    write_line(std::string(dry ? "would write " : "wrote ") + made.path.string() + ", "
               + format::bytes(made.bytes));
    return 0;
}

}  // namespace crucible::lab
