// SPDX-License-Identifier: MIT
//
// What a fine-tune is made of, before it is run.
//
// A recipe is the whole of what the Create tab collects: a name, a base model
// to specialize, the data to specialize it on, the tools it should learn to
// reach for, and what to export at the end. It is saved as it is filled in, so
// assembling one is not a thing you have to finish in a sitting -- gathering a
// few gigabytes of training data rarely is.
//
// Nothing here trains anything. This is the part that can be reasoned about
// without a GPU: what has been chosen, what is still missing, and whether the
// machine can do what is being asked of it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::lab {

/// Where something came from. Both are first class: a hub repository is how
/// most people will start, and a directory of their own files is the reason to
/// run a lab locally at all.
enum class Source { Hub, Local };

std::string_view source_id(Source source);
Source           source_from_id(std::string_view id);

/// One chosen thing: a base model, a corpus, a tool pack.
struct Asset {
    Source      source = Source::Hub;
    std::string id;      ///< "unsloth/Llama-3.2-1B" on the hub, or a path locally
    std::string file;    ///< the file within a hub repo, when one was named
    std::string label;   ///< what to show: the repo's own name, or the file's

    /// Bytes on disk, or the size the hub reported. Zero when nothing has said
    /// yet -- which is not the same as an empty file, and is why the download
    /// step asks rather than assuming.
    std::uint64_t bytes = 0;

    /// Where it is on this machine, once it has been fetched. Empty until then.
    std::string path;

    bool chosen() const { return !id.empty(); }
    bool local() const { return source == Source::Local || !path.empty(); }
};

/// How the fine-tune is done.
///
/// Both train a small adapter beside a frozen base model rather than rewriting
/// its weights, which is the whole reason this runs on a consumer card at all:
/// full fine-tuning a 7B wants a data-center GPU, and an adapter on the same
/// model wants a laptop's.
///
/// What differs is how the frozen base is loaded. LoRA keeps it in sixteen
/// bits, which is faster and slightly better where it fits; QLoRA quantizes it
/// to four, which is how a 13B goes where a 7B would not.
enum class Method { Lora, Qlora };

std::string_view method_id(Method method);
Method           method_from_id(std::string_view id);

/// What comes out at the end.
enum class Export { Gguf, Mlx };

std::string_view export_id(Export format);
Export           export_from_id(std::string_view id);

/// A model being built.
struct Recipe {
    /// What the expert will be called, as typed. `id` is its slug, and is what
    /// names the directory and the files.
    std::string name;
    std::string id;

    /// What it is for, in the user's words. Kept because a model you come back
    /// to in a month is otherwise a file with a date on it.
    std::string purpose;

    Asset              base;
    std::vector<Asset> data;
    std::vector<Asset> tools;

    Method method = Method::Qlora;
    Export format = Export::Gguf;

    /// The quantization to export at: "Q4_K_M" and friends, or "F16" for none.
    std::string quantization = "Q4_K_M";

    /// What the base model is, in billions of parameters. Filled in from the
    /// hub or measured from the file, and the number every estimate here
    /// depends on.
    double parameters_b = 0.0;

    int   epochs        = 2;
    int   context       = 512;
    float learning_rate = 1e-5F;

    /// Set once training has produced something, so the tab can offer to test
    /// and export it rather than train it again.
    std::string trained_path;
};

/// The slug for a name: lower case, dashes, nothing a filesystem dislikes.
std::string slug_of(std::string_view name);

/// What is still missing before this can be trained, in the order the tab asks
/// for it. Empty means ready.
std::vector<std::string> missing(const Recipe& recipe);

/// Roughly what the exported file will weigh, for the quantization chosen.
/// Consumers pick a quantization by the size it lands at, so this is the number
/// the tab shows beside each one.
std::uint64_t export_bytes(double parameters_b, std::string_view quantization);

/// Bits per weight for a quantization name, or 0 for one that is not known.
double bits_per_weight(std::string_view quantization);

/// Whether this machine can train that model that way.
struct Fit {
    bool          possible = false;
    std::uint64_t needed   = 0;   ///< bytes of the memory the method trains in
    std::uint64_t have     = 0;   ///< bytes of it there are
    std::string   note;           ///< the one line the tab shows
};

/// `vram` is what the GPUs have between them and `ram` what the machine has.
/// The card is what counts; `ram` is the fallback for a machine with no GPU,
/// where a small fine-tune is slow but real.
///
/// The estimates are deliberately plain arithmetic: the frozen base is nearly
/// all of it -- two bytes a parameter in sixteen-bit LoRA, half a byte at
/// four-bit QLoRA -- and the adapter beside it is a rounding error. Both are
/// approximations, and both err the same way on purpose: refusing a run that
/// would have fit costs a click, and starting one that cannot costs an hour.
Fit estimate_fit(double parameters_b, Method method, std::uint64_t vram, std::uint64_t ram);

/// Read and write. A recipe lives at lab_dir()/<id>/recipe.json, beside
/// whatever has been downloaded for it.
std::string serialize(const Recipe& recipe);
bool        parse(std::string_view json, Recipe& out, std::string& error);

/// A model the lab has finished: the file, and the expert it was made to be.
///
/// What the expert roster offers beside the models directory -- a fine-tune
/// made here is the whole point of the tab, and having to go and find its file
/// on disk to use it would be a silly last step.
struct Made {
    std::string           name;    ///< the recipe's name, which is the seat's name
    std::string           id;      ///< its slug
    std::string           purpose; ///< what it is for, which the delegator routes on
    std::filesystem::path path;    ///< the exported file
    std::uint64_t         bytes = 0;
};

/// Every finished model, newest name order. A recipe whose file has been moved
/// or deleted is left out: offering a seat a model that is not there produces a
/// seat that cannot answer.
std::vector<Made> finished_models();

std::filesystem::path recipe_file(const Recipe& recipe);
bool                  save(const Recipe& recipe, std::string& error);
std::vector<Recipe>   saved_recipes();

}  // namespace crucible::lab
