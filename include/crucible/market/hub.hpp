// SPDX-License-Identifier: MIT
//
// Models to download, from Hugging Face.
//
// The Models tab: search the hub for GGUF or MLX models, see what each comes
// in -- the quantizations of a GGUF repository, the sibling repositories of
// an MLX one -- and whether this machine can run it, then fetch it into the
// models folder. Read-only and unauthenticated, like lab/hub.hpp's search for
// base models: public repositories, no token, no account. A gated one says so
// rather than failing after the wait.
//
// The parsing is kept apart from the fetching, as in lab/hub.hpp: the shapes
// the hub returns are what break, and a canned reply in a test is the only way
// to check them without a network.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::market {

enum class Format { Gguf, Mlx };
enum class Sort { Best, Downloads, Likes, Updated };

std::string_view format_id(Format format);   ///< "gguf", "mlx"
Format           format_from_id(std::string_view id);
std::string_view sort_id(Sort sort);         ///< "best", "downloads", "likes", "updated"
Sort             sort_from_id(std::string_view id);

/// One repository, as a search or its own page describes it.
struct Listing {
    std::string   id;              ///< "unsloth/Qwen3.8-27B-GGUF"
    std::string   author;          ///< the part before the slash
    std::string   name;            ///< the part after it
    std::uint64_t downloads = 0;   ///< the hub's count, the last month's
    std::uint64_t likes     = 0;
    std::uint64_t trending  = 0;   ///< the hub's "in use now" score; 0 for most
    std::string   updated;         ///< when it last changed, ISO 8601
    double        params_b  = 0;   ///< billions; from the weights' own count, else the name
    std::string   architecture;    ///< "qwen35", from a GGUF's metadata
    std::uint64_t context   = 0;   ///< a GGUF's trained context length
    std::string   pipeline;        ///< "text-generation", "image-text-to-text"
    bool          vision    = false;  ///< reads pictures
    bool          tools     = false;  ///< its chat template takes tool calls
    bool          reasoning = false;  ///< its chat template has a place for thinking
    bool          gated     = false;  ///< needs a license accepted in a browser first
    std::string   license;            ///< "apache-2.0"
};

/// A page of a search, and where the next one is.
struct Page {
    std::vector<Listing> listings;
    std::string          next;     ///< the URL of the next page, empty at the end
};

/// The URL a first page is asked for.
std::string search_url(std::string_view query, Format format, Sort sort, int limit);

/// What a search answered, as listings. Tolerant: a field the hub drops or
/// adds must not empty the list.
Page parse_search(std::string_view json_text, std::string_view link_header);

/// "Best match": the hub's own order puts what is in use now first, and
/// leaves everything else -- nearly all of a narrow search -- in no order at
/// all. This keeps the first and orders the rest by how much each is used.
void rank_best(std::vector<Listing>& listings);

/// The next page's URL out of a Link header, or empty.
std::string next_from_link(std::string_view link_header);

/// Ask for one page. `url` is search_url's, or a Page::next.
bool search(const std::string& url, Page& out, std::string& error);

/// One file in a repository.
struct File {
    std::string   path;            ///< inside the repository: "BF16/x-00001-of-00002.gguf"
    std::uint64_t bytes = 0;
    std::string   sha256;          ///< the hub's digest of it, for a file kept in LFS; else empty
};

/// One thing to download: a GGUF quantization -- a file, or a split set, with
/// the vision projector beside it when there is one -- or a whole MLX
/// repository.
struct Option {
    std::string       name;        ///< "Q4_K_M", "4bit"
    std::string       repo;        ///< where its files are
    std::vector<File> files;       ///< what is fetched
    std::uint64_t     bytes = 0;   ///< all of it
};

/// A repository's own page: its listing, and what it can be downloaded as.
struct Detail {
    Listing             listing;
    std::string         base_model;
    std::vector<Option> options;   ///< smallest first
};

/// The URL of a repository's page, with every file's size.
std::string model_url(std::string_view id);

/// A repository's page, read. Its options are a GGUF repository's quants;
/// for MLX, the repository itself is the one option and its siblings are
/// asked for separately.
Detail parse_model(std::string_view json_text, Format format);

/// A GGUF repository's files as options: one per quantization, a split set
/// as one, smallest first. Projectors (mmproj) go with every option; an
/// importance matrix or a draft model is not an option.
std::vector<Option> gguf_options(std::string_view repo, const std::vector<File>& files);

/// The quantization a file or repository is named for: "Q4_K_M", "UD-Q4_K_XL",
/// "BF16", "4bit". Empty when its name says none.
std::string quant_of(std::string_view name);

/// An MLX repository's files, as the one option it is: everything but the
/// card and its pictures.
Option mlx_option(std::string_view repo, const std::vector<File>& files);

/// Ask for a repository's page; for MLX, its siblings in other quantizations
/// too -- same author, same name but for the bits.
bool model(std::string_view id, Format format, Detail& out, std::string& error);

/// A repository's README, as its author wrote it.
std::string readme_url(std::string_view id);
bool readme(std::string_view id, std::string& out, std::string& error);

/// What this machine has for a model to live in.
struct Machine {
    std::uint64_t vram  = 0;       ///< every GPU's memory, less what a desktop keeps
    std::uint64_t ram   = 0;       ///< the machine's memory
    int           gpus  = 0;
    bool          apple = false;   ///< Apple silicon: one memory, and MLX runs here
};

/// Asked of the devices the runtimes registered and of the system.
Machine this_machine();

/// Whether a download of `bytes` will run here: all of it on the GPUs, some
/// of it on the processor, only on the processor, not at all, or -- MLX
/// anywhere but a Mac -- not on this machine.
enum class Fit { Gpu, Partial, Cpu, TooLarge, MacOnly };
std::string_view fit_id(Fit fit);    ///< "gpu", "partial", "cpu", "large", "mac"
Fit fit(std::uint64_t bytes, Format format, const Machine& machine);

/// Where a repository is downloaded to: the models folder, its author, its
/// name -- the way LM Studio lays a folder out, which the catalog reads.
std::filesystem::path destination(const std::filesystem::path& models_dir, std::string_view repo);

/// Where one of an option's files lands. A GGUF's subfolders are dropped, so
/// a split set's parts sit together where llama.cpp looks for them; an MLX
/// repository keeps its own layout.
std::filesystem::path landing(const std::filesystem::path& folder, const File& file, Format format);

/// The URL a file is fetched from.
std::string file_url(std::string_view repo, std::string_view path);

}  // namespace crucible::market
