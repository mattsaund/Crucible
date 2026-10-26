// SPDX-License-Identifier: MIT
//
// Condensing a model: the same weights in fewer bits.
//
// A Q8_0 found in the Models tab that will not fit on this machine's cards can
// be made to: llama.cpp quantizes it again, to Q4_K_M or the like, here and
// without downloading anything. It is not free. Every bit taken away costs
// some of the model's quality, and a file already at four bits has little
// left to give, which is why the Create tab says how big each choice comes out
// and whether it then fits, and leaves the choice to the person.
//
// The work runs in a child process: Crucible itself, started with --condense.
// Quantizing a large model takes minutes, cannot be interrupted from inside
// llama.cpp, and holds gigabytes of buffers; a child can be stopped, and what
// goes wrong in it does not take the window with it. It speaks the trainer's
// language, JSON events on standard output, so the Create tab follows it the
// way it follows a fine-tune. See trainer.hpp.
//
// What it writes is new and beside the original, never over it or over
// anything else there: a condensed copy that took the name of a file already
// in the models folder would have replaced a model somebody downloaded.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::lab {

/// One quantization the Create tab offers.
struct Quantization {
    const char* name;      ///< "Q4_K_M", as llama.cpp and file names spell it
    double      bits;      ///< about what it averages per weight
    const char* note;      ///< what it costs, in a few words
    bool        samples;   ///< measured over sample text first (see imatrix.hpp)
};

/// The quantizations offered, best first. The very low-bit ones (IQ3_XXS,
/// IQ2 and IQ1) sample the model first: llama.cpp makes them only with an
/// importance matrix, measured by running the original over sample text.
const std::vector<Quantization>& quantizations();

/// Whether `name` is a quantization `quantize` accepts: one offered, or one
/// of the others the fine-tuner exports at (F16, Q4_0...).
bool known_quantization(std::string_view name);

/// The quantization a GGUF file name says it is at ("flint-Q6_K.gguf" gives
/// "Q6_K", "x.UD-Q4_K_XL.gguf" gives "UD-Q4_K_XL"), or empty.
std::string quantization_in_name(std::string_view file_name);

/// The name a condensed copy gets: the source's, with its quantization
/// replaced or added, and a split model's part number dropped.
/// "flint-Q6_K.gguf" at Q4_K_M is "flint-Q4_K_M.gguf".
std::string condensed_name(std::string_view file_name, std::string_view quantization);

/// Where a condensed copy goes: beside the source when the source is in the
/// models folder, where the pickers will find it, and at the top of the
/// models folder when it is not.
std::filesystem::path condensed_path(const std::filesystem::path& source,
                                     const std::filesystem::path& models_dir,
                                     std::string_view quantization);

/// `wanted`, or when that name is taken, the first free one with a number
/// before its quantization: "flint-2-Q4_K_M.gguf", then "flint-3-Q4_K_M.gguf".
/// A name with a ".part" beside it is taken too: a download, or another
/// condense, is on its way to it. Asked of the disk rather than compared as
/// text, because whether "flint-q4_k_m.gguf" is the same file depends on the
/// disk: it is on Windows, on a Mac and on exFAT, and it is not on ext4.
/// Empty when there is no free name.
std::filesystem::path free_path(const std::filesystem::path& wanted);

/// What a GGUF is now, read from its header.
struct Original {
    bool          ok = false;
    std::string   error;
    std::uint64_t bytes    = 0;   ///< the file, all of its parts
    std::uint64_t elements = 0;   ///< weights, counted over every tensor
    double        bits     = 0.0; ///< bytes over weights: what it averages now
    std::string   quantization;  ///< from the name, else from the header's file type
    std::string   type;          ///< the header's file type alone, by its usual name, or empty
};
Original read_source(const std::filesystem::path& file);

/// Whether condensing `source` to `quantization` makes it smaller by enough
/// to be worth it. Never to the type it is already at, by its name or its
/// header, and not to a size it is already at or under.
///
/// Measured against the source's own type where that is known, not against
/// the bits the file averages: a small model keeps its embeddings in more
/// bits than its type, so a 135M at Q4_K_M averages over five, and compared
/// with that, Q4_K_M looked like a step down from itself.
bool worth_condensing(const Original& source, std::string_view quantization);

/// The same rule for a size measured by a dry run: a model whose weights do
/// not divide into a type's blocks gets a larger type for those, and can come
/// out nearly as big as it went in whatever the type promises.
bool shrinks_enough(double source_bits, double result_bits);

/// How a quantization went.
struct Quantized {
    bool                  ok = false;
    std::string           error;
    std::uint64_t         bytes = 0;   ///< what was written, or what a dry run would write
    std::filesystem::path path;        ///< where it was written, which is `out` unless that was taken meanwhile
};

/// The file `quantize` writes `out` as until it is whole.
std::filesystem::path part_of(const std::filesystem::path& out);

struct ImportanceMatrix;

/// Quantize `in` to `out` at `quantization`, in this process. With `dry_run`
/// nothing is written and `bytes` is what would be. `progress` is told
/// (tensors done, tensors in all) as it goes. Written to a ".part" file and
/// renamed at the end, so a stopped run leaves nothing that looks finished.
///
/// Never over `in`. A file already at `out` is replaced, as llama-quantize
/// would; one that appeared there while this worked is somebody else's, and
/// the result goes to the next free name instead (see free_path). A type that
/// samples first needs `importance`, except for a dry run.
Quantized quantize(const std::filesystem::path& in, const std::filesystem::path& out,
                   std::string_view quantization, bool dry_run,
                   const std::function<void(int, int)>& progress = {},
                   const ImportanceMatrix* importance = nullptr);

/// Whether `quantization` is one that samples the model first.
bool samples_first(std::string_view quantization);

/// The child process's command lines, all answered without a window:
///   --condense --recipe FILE --out DIR --export-dir DIR
///       a Create tab recipe, reporting the way the trainer does: the only
///       one that speaks in events
///   --quantize IN OUT TYPE [--dry-run]
///       llama-quantize's arguments, so the fine-tuner can use it too, and
///       its kind of output: plain lines, which the fine-tuner passes on
///       into the stream the trainer reads as log
///   --quantize-sizes IN TYPE[,TYPE...]
///       one JSON line: what IN is now and what each TYPE would weigh
/// Returns the exit status. `args` excludes the program name.
int condense_main(const std::vector<std::string>& args);

/// Whether the command line is one of condense_main's.
bool is_condense_command(int argc, char** argv);

}  // namespace crucible::lab
