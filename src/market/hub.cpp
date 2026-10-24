// SPDX-License-Identifier: MIT
//
// See hub.hpp.
#include "crucible/market/hub.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <map>
#include <regex>

#include <nlohmann/json.hpp>

#include "crucible/lab/hub.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/resources.hpp"

namespace crucible::market {
namespace {

using json = nlohmann::json;

constexpr const char* kSite = "https://huggingface.co/";
constexpr std::uint64_t kGiB = 1ULL << 30;

/// Percent-encode a query or a path segment.
std::string encode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

/// A number wherever the hub put one, and nothing where it did not.
std::uint64_t number(const json& node, const char* key) {
    const auto found = node.find(key);
    if (found == node.end()) {
        return 0;
    }
    if (found->is_number_unsigned()) { return found->get<std::uint64_t>(); }
    if (found->is_number_integer())  { return static_cast<std::uint64_t>(std::max<std::int64_t>(0, found->get<std::int64_t>())); }
    if (found->is_number_float())    { return static_cast<std::uint64_t>(std::max(0.0, found->get<double>())); }
    return 0;
}

std::string text(const json& node, const char* key) {
    const auto found = node.find(key);
    return found != node.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string lower(std::string_view text) { return format::to_lower(std::string(text)); }

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/// One repository out of whatever the hub sent about it, from a search or
/// from its own page: the two carry the same fields where they overlap.
Listing listing_of(const json& node) {
    Listing listing;
    listing.id = text(node, "id");
    if (listing.id.empty()) {
        listing.id = text(node, "modelId");
    }
    if (const std::size_t slash = listing.id.find('/'); slash != std::string::npos) {
        listing.author = listing.id.substr(0, slash);
        listing.name   = listing.id.substr(slash + 1);
    } else {
        listing.name = listing.id;
    }
    listing.downloads = number(node, "downloads");
    listing.likes     = number(node, "likes");
    listing.trending  = number(node, "trendingScore");
    listing.updated   = text(node, "lastModified");
    listing.pipeline  = text(node, "pipeline_tag");

    // "gated" is false, "auto" or "manual": anything but false needs a
    // browser and an accepted license before a download will work.
    if (const auto gated = node.find("gated"); gated != node.end()) {
        listing.gated = gated->is_boolean() ? gated->get<bool>() : !gated->is_null();
    }

    std::vector<std::string> tags;
    if (const auto found = node.find("tags"); found != node.end() && found->is_array()) {
        for (const json& tag : *found) {
            if (tag.is_string()) {
                tags.push_back(lower(tag.get<std::string>()));
            }
        }
    }
    const auto tagged = [&tags](std::initializer_list<std::string_view> wanted) {
        return std::any_of(tags.begin(), tags.end(), [&wanted](const std::string& tag) {
            return std::find(wanted.begin(), wanted.end(), tag) != wanted.end();
        });
    };
    for (const std::string& tag : tags) {
        if (tag.rfind("license:", 0) == 0) {
            listing.license = tag.substr(8);
        }
    }
    if (const auto card = node.find("cardData"); card != node.end() && card->is_object()) {
        if (listing.license.empty()) {
            listing.license = text(*card, "license");
        }
    }

    // How big it is: the weights' own count when the hub read one, then the
    // name -- "Llama-3.2-1B" -- which is most of what a quantized copy has.
    std::string chat_template;
    if (const auto gguf = node.find("gguf"); gguf != node.end() && gguf->is_object()) {
        listing.params_b     = static_cast<double>(number(*gguf, "total")) / 1e9;
        listing.architecture = text(*gguf, "architecture");
        listing.context      = number(*gguf, "context_length");
        chat_template        = lower(text(*gguf, "chat_template"));
    }
    if (listing.params_b <= 0.0) {
        if (const auto weights = node.find("safetensors"); weights != node.end() && weights->is_object()) {
            listing.params_b = static_cast<double>(number(*weights, "total")) / 1e9;
        }
    }
    // The hub counts one file of the repository, which can be its draft head
    // or its projector: a name that says "27B" is believed over a count far
    // from it.
    const double named_b = lab::hub::parameters_from_name(listing.name);
    if (listing.params_b <= 0.0 || (named_b > 0.0 && (listing.params_b > named_b * 2 || listing.params_b < named_b / 2))) {
        listing.params_b = named_b > 0.0 ? named_b : listing.params_b;
    }

    // What it can do, the way LM Studio tells: from what it is tagged with,
    // and from its chat template, which has a place for tool calls when it
    // takes them and for thinking when it thinks.
    const std::string pipeline = lower(listing.pipeline);
    listing.vision = pipeline == "image-text-to-text" || pipeline == "image-to-text"
                  || pipeline == "visual-question-answering" || pipeline == "any-to-any"
                  || tagged({"vision", "multimodal", "image-text-to-text"});
    listing.tools = tagged({"tool-use", "tool_use", "function-calling", "tools", "agent"})
                 || (contains(chat_template, "tools")
                     && (contains(chat_template, "tool_call") || contains(chat_template, "function")));
    listing.reasoning = tagged({"reasoning", "thinking"})
                     || contains(chat_template, "<think>") || contains(chat_template, "enable_thinking")
                     || contains(chat_template, "reasoning_content")
                     || contains(chat_template, "<|channel|>analysis");
    return listing;
}

bool get(const std::string& url, util::http::Response& response, std::string& error) {
    util::http::Request request;
    request.url             = url;
    request.headers         = {{"Accept", "application/json"}};
    request.timeout_seconds = 20;
    response = util::http::send(request);
    if (!response.ok()) {
        error = "Hugging Face could not be reached (" + response.reason() + ")";
        return false;
    }
    return true;
}

/// "Qwen3.8-27B-MLX-4bit" is "Qwen3.8-27B-MLX" in another number of bits:
/// the name, without the quantization it ends in. Empty when it ends in none.
std::string without_quant(std::string_view name) {
    const std::string quant = quant_of(name);
    if (quant.empty()) {
        return {};
    }
    const std::string lowered = lower(name);
    const std::size_t at      = lowered.rfind(lower(quant));
    if (at == std::string::npos || at == 0) {
        return {};
    }
    std::string base(name.substr(0, at));
    while (!base.empty() && (base.back() == '-' || base.back() == '_' || base.back() == '.')) {
        base.pop_back();
    }
    return base;
}

/// A path inside a repository with nothing in it that leaves a folder.
bool safe_relative(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\' || contains(path, ":")) {
        return false;
    }
    for (const auto& part : std::filesystem::path(std::string(path))) {
        if (part == ".." || part == ".") {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string_view format_id(Format format) { return format == Format::Mlx ? "mlx" : "gguf"; }
Format format_from_id(std::string_view id) { return id == "mlx" ? Format::Mlx : Format::Gguf; }

std::string_view sort_id(Sort sort) {
    switch (sort) {
        case Sort::Downloads: return "downloads";
        case Sort::Likes:     return "likes";
        case Sort::Updated:   return "updated";
        case Sort::Best:      break;
    }
    return "best";
}

Sort sort_from_id(std::string_view id) {
    if (id == "downloads") { return Sort::Downloads; }
    if (id == "likes")     { return Sort::Likes; }
    if (id == "updated")   { return Sort::Updated; }
    return Sort::Best;
}

std::string search_url(std::string_view query, Format format, Sort sort, int limit) {
    std::string url = std::string(kSite) + "api/models?filter=" + std::string(format_id(format))
                    + "&limit=" + std::to_string(std::clamp(limit, 1, 100));
    if (!query.empty()) {
        url += "&search=" + encode(query);
    }
    // No sort is the hub's own order -- what is in use now -- which is what a
    // search box means by "best match". The rest are what they say.
    switch (sort) {
        case Sort::Downloads: url += "&sort=downloads&direction=-1"; break;
        case Sort::Likes:     url += "&sort=likes&direction=-1"; break;
        case Sort::Updated:   url += "&sort=lastModified&direction=-1"; break;
        case Sort::Best:      break;
    }
    // Asked for by name: expanding replaces the default fields rather than
    // adding to them, so everything read in listing_of is named here.
    for (const char* field : {"downloads", "likes", "trendingScore", "lastModified", "gguf", "safetensors",
                              "pipeline_tag", "tags", "gated"}) {
        url += std::string("&expand%5B%5D=") + field;
    }
    return url;
}

void rank_best(std::vector<Listing>& listings) {
    // A few points of "trending" are a handful of downloads this week; ten
    // times the downloads, or the likes, count for as much.
    const auto score = [](const Listing& l) {
        return static_cast<double>(l.trending) + 2.0 * std::log10(1.0 + static_cast<double>(l.downloads))
             + 3.0 * std::log10(1.0 + static_cast<double>(l.likes));
    };
    std::stable_sort(listings.begin(), listings.end(),
                     [&score](const Listing& a, const Listing& b) { return score(a) > score(b); });
}

std::string next_from_link(std::string_view link) {
    // <https://huggingface.co/api/models?...&cursor=...>; rel="next"
    std::size_t at = 0;
    while (at < link.size()) {
        const std::size_t open  = link.find('<', at);
        const std::size_t close = open == std::string_view::npos ? open : link.find('>', open);
        if (close == std::string_view::npos) {
            break;
        }
        const std::size_t end = link.find(',', close);
        const std::string_view params = link.substr(close + 1, end == std::string_view::npos
                                                                   ? std::string_view::npos : end - close - 1);
        if (contains(params, "rel=\"next\"") || contains(params, "rel=next")) {
            return std::string(link.substr(open + 1, close - open - 1));
        }
        at = end == std::string_view::npos ? link.size() : end + 1;
    }
    return {};
}

Page parse_search(std::string_view json_text, std::string_view link_header) {
    Page page;
    const json parsed = json::parse(json_text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return page;
    }
    for (const json& node : parsed) {
        if (!node.is_object()) {
            continue;
        }
        Listing listing = listing_of(node);
        if (!listing.id.empty()) {
            page.listings.push_back(std::move(listing));
        }
    }
    page.next = next_from_link(link_header);
    return page;
}

bool search(const std::string& url, Page& out, std::string& error) {
    // Only ever a URL of the hub's: a Page::next comes back from the page,
    // and the window is not a way to fetch anything else.
    if (url.rfind(std::string(kSite) + "api/models?", 0) != 0) {
        error = "not a Hugging Face search";
        return false;
    }
    util::http::Response response;
    if (!get(url, response, error)) {
        return false;
    }
    out = parse_search(response.body, response.header("link"));
    return true;
}

std::string quant_of(std::string_view name) {
    // At a boundary on both sides, so the "Q4" of "Qwen" and the "F16" of
    // "BF16" are not read as quantizations of their own.
    static const std::regex gguf(
        R"((?:^|[-_./])((?:ud-)?(?:iq[1-4]_(?:xxs|xs|nl|s|m|l)|q[2-8]_k(?:_[a-z]{1,3})?|)"
        R"(q4_0_4_[48]|q4_0_8_8|q[2-8]_[01]|tq[12]_0|mxfp4(?:_moe)?|bf16|f16|f32|fp16|fp8))(?=$|[-_.]))");
    static const std::regex bits(R"((?:^|[-_./])(\d{1,2})[-_]?bits?(?=$|[-_.]))");
    static const std::regex named(R"((?:^|[-_./])(mxfp4|nvfp4|dwq|bf16|fp16|fp8)(?=$|[-_.]))");

    std::string stem = lower(name);
    if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".gguf") == 0) {
        stem.resize(stem.size() - 5);
    }
    std::smatch match;
    if (std::regex_search(stem, match, gguf)) {
        return upper(match[1].str());
    }
    if (std::regex_search(stem, match, bits)) {
        return match[1].str() + "bit";
    }
    if (std::regex_search(stem, match, named)) {
        return match[1].str();
    }
    return {};
}

namespace {

/// A draft head by its name: "x-FastMTP-32K.gguf", "x-draft.gguf". A model
/// built with MTP layers says so too, but it also says its quantization.
bool draft_head(const std::string& low_leaf) {
    std::string word;
    for (std::size_t i = 0; i <= low_leaf.size(); ++i) {
        const char c = i < low_leaf.size() ? low_leaf[i] : '-';
        if (c != '-' && c != '_' && c != '.') {
            word += c;
            continue;
        }
        const bool head = word == "draft" || word == "drafter" || word.rfind("eagle", 0) == 0
                       || (word.size() >= 3 && word.compare(word.size() - 3, 3, "mtp") == 0);
        if (head) {
            return quant_of(low_leaf).empty();
        }
        word.clear();
    }
    return false;
}

}  // namespace

std::vector<Option> gguf_options(std::string_view repo, const std::vector<File>& files) {
    static const std::regex split(R"(^(.*)-(\d{5})-of-(\d{5})\.gguf$)", std::regex::icase);

    // Which projector goes with the options, when the repository reads
    // pictures: full precision before anything rounded off, the half-sized
    // float before the full one.
    const File* projector = nullptr;
    int         rank      = 99;
    for (const File& file : files) {
        const std::string name = lower(std::filesystem::path(file.path).filename().string());
        if (!contains(name, "mmproj") || name.size() < 5 || name.compare(name.size() - 5, 5, ".gguf") != 0) {
            continue;
        }
        const std::string quant = quant_of(name);
        const int r = quant == "F16" ? 0 : quant == "BF16" ? 1 : quant == "Q8_0" ? 2 : quant == "F32" ? 3 : 4;
        if (r < rank) {
            rank      = r;
            projector = &file;
        }
    }

    // Single files, and split sets gathered by what comes before the part
    // number: "x-00001-of-00003.gguf" through "x-00003-of-00003.gguf".
    struct Set {
        std::string       name;
        std::vector<File> parts;
        std::size_t       expected = 1;
    };
    std::map<std::string, Set> sets;
    for (const File& file : files) {
        const std::string leaf = std::filesystem::path(file.path).filename().string();
        const std::string low  = lower(leaf);
        if (low.size() < 5 || low.compare(low.size() - 5, 5, ".gguf") != 0) {
            continue;
        }
        // A projector goes beside a model; an importance matrix is what a
        // quantization was made with; an MTP file is a draft head for
        // speculative decoding. None is a model to talk to.
        if (contains(low, "mmproj") || contains(low, "imatrix") || low.rfind("mtp-", 0) == 0
            || contains(lower(file.path), "mtp/") || draft_head(low)) {
            continue;
        }
        std::smatch match;
        std::string key;
        std::size_t expected = 1;
        if (std::regex_match(leaf, match, split)) {
            key      = std::filesystem::path(file.path).parent_path().string() + "/" + match[1].str();
            expected = static_cast<std::size_t>(std::stoul(match[3].str()));
        } else {
            key = file.path;
        }
        Set& set     = sets[key];
        set.expected = expected;
        set.parts.push_back(file);
        if (set.name.empty()) {
            const std::string quant = quant_of(expected > 1 ? key : leaf);
            set.name = quant.empty() ? std::filesystem::path(leaf).stem().string() : quant;
        }
    }

    std::vector<Option> options;
    std::map<std::string, int> named;
    for (auto& [key, set] : sets) {
        if (set.parts.size() != set.expected) {
            continue;   // a split set missing a part is not something to download
        }
        std::sort(set.parts.begin(), set.parts.end(),
                  [](const File& a, const File& b) { return a.path < b.path; });
        Option option;
        option.name = set.name;
        option.repo = std::string(repo);
        option.files = set.parts;
        if (projector != nullptr) {
            option.files.push_back(*projector);
        }
        for (const File& file : option.files) {
            option.bytes += file.bytes;
        }
        options.push_back(std::move(option));
    }
    // Two of one name -- a quantization in the root and again in a folder --
    // are told apart by the folder.
    for (const Option& option : options) {
        ++named[option.name];
    }
    for (Option& option : options) {
        if (named[option.name] > 1) {
            const std::string folder = std::filesystem::path(option.files.front().path).parent_path().string();
            if (!folder.empty()) {
                option.name += " (" + folder + ")";
            }
        }
    }
    std::sort(options.begin(), options.end(),
              [](const Option& a, const Option& b) { return a.bytes < b.bytes; });
    return options;
}

Option mlx_option(std::string_view repo, const std::vector<File>& files) {
    Option option;
    option.repo = std::string(repo);
    const std::string name = std::string(repo.substr(repo.find('/') == std::string_view::npos ? 0 : repo.find('/') + 1));
    option.name = quant_of(name);
    if (option.name.empty()) {
        option.name = "default";
    }
    for (const File& file : files) {
        const std::string low  = lower(file.path);
        const std::string leaf = lower(std::filesystem::path(file.path).filename().string());
        // The card and its pictures are for a browser, not for the model.
        if (leaf == ".gitattributes" || leaf == "readme.md" || low.rfind("images/", 0) == 0
            || low.rfind("assets/", 0) == 0 || low.rfind("figures/", 0) == 0) {
            continue;
        }
        const std::string ext = std::filesystem::path(leaf).extension().string();
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".webp" || ext == ".svg") {
            continue;
        }
        if (!safe_relative(file.path)) {
            continue;
        }
        option.files.push_back(file);
        option.bytes += file.bytes;
    }
    return option;
}

std::string model_url(std::string_view id) {
    return std::string(kSite) + "api/models/" + std::string(id) + "?blobs=true";
}

Detail parse_model(std::string_view json_text, Format format) {
    Detail detail;
    const json parsed = json::parse(json_text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return detail;
    }
    detail.listing = listing_of(parsed);
    if (const auto card = parsed.find("cardData"); card != parsed.end() && card->is_object()) {
        if (const auto base = card->find("base_model"); base != card->end()) {
            if (base->is_string()) {
                detail.base_model = base->get<std::string>();
            } else if (base->is_array() && !base->empty() && base->front().is_string()) {
                detail.base_model = base->front().get<std::string>();
            }
        }
    }
    std::vector<File> files;
    if (const auto siblings = parsed.find("siblings"); siblings != parsed.end() && siblings->is_array()) {
        for (const json& one : *siblings) {
            if (!one.is_object()) {
                continue;
            }
            File file;
            file.path = text(one, "rfilename");
            file.bytes = number(one, "size");
            // A file kept in LFS has its real size in the LFS record too.
            if (const auto lfs = one.find("lfs"); lfs != one.end() && lfs->is_object()) {
                if (const std::uint64_t real = number(*lfs, "size"); real > 0) {
                    file.bytes = real;
                }
                const std::string digest = lower(text(*lfs, "sha256"));
                if (digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == std::string::npos) {
                    file.sha256 = digest;
                }
            }
            if (!file.path.empty()) {
                files.push_back(std::move(file));
            }
        }
    }
    if (format == Format::Gguf) {
        detail.options = gguf_options(detail.listing.id, files);
        // A GGUF repository reads pictures when it ships a projector, whatever
        // its card says.
        detail.listing.vision = detail.listing.vision
            || std::any_of(files.begin(), files.end(), [](const File& f) { return contains(lower(f.path), "mmproj"); });
    } else {
        Option option = mlx_option(detail.listing.id, files);
        if (!option.files.empty()) {
            detail.options.push_back(std::move(option));
        }
    }
    return detail;
}

bool model(std::string_view id, Format format, Detail& out, std::string& error) {
    util::http::Response response;
    if (!get(model_url(id), response, error)) {
        return false;
    }
    out = parse_model(response.body, format);
    if (out.listing.id.empty()) {
        error = "Hugging Face did not describe " + std::string(id);
        return false;
    }
    if (format != Format::Mlx) {
        return true;
    }

    // An MLX repository is one quantization; its others are repositories of
    // their own, by the same author, named alike but for the bits. Asked for
    // so the choice is there the way a GGUF repository's quants are.
    const std::string base = without_quant(out.listing.name);
    if (base.empty() || out.listing.author.empty()) {
        return true;
    }
    util::http::Response siblings;
    std::string          ignored;
    const std::string    url = std::string(kSite) + "api/models?filter=mlx&limit=30&author="
                             + encode(out.listing.author) + "&search=" + encode(base);
    if (!get(url, siblings, ignored)) {
        return true;   // the one asked for is still there to download
    }
    const Page found = parse_search(siblings.body, "");
    int asked = 0;
    for (const Listing& sibling : found.listings) {
        if (sibling.id == out.listing.id || without_quant(sibling.name) != base || asked >= 8) {
            continue;
        }
        ++asked;
        util::http::Response page;
        if (!get(model_url(sibling.id), page, ignored)) {
            continue;
        }
        Detail other = parse_model(page.body, Format::Mlx);
        for (Option& option : other.options) {
            out.options.push_back(std::move(option));
        }
    }
    std::sort(out.options.begin(), out.options.end(),
              [](const Option& a, const Option& b) { return a.bytes < b.bytes; });
    return true;
}

std::string readme_url(std::string_view id) {
    return std::string(kSite) + std::string(id) + "/raw/main/README.md";
}

bool readme(std::string_view id, std::string& out, std::string& error) {
    util::http::Request request;
    request.url             = readme_url(id);
    request.timeout_seconds = 20;
    const util::http::Response response = util::http::send(request);
    if (response.status == 404) {
        out.clear();   // no card is an answer, not a failure
        return true;
    }
    if (!response.ok()) {
        error = "Hugging Face could not be reached (" + response.reason() + ")";
        return false;
    }
    out = response.body;
    return true;
}

Machine this_machine() {
    Machine machine;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    machine.apple = true;
#endif
    for (const ComputeDevice& gpu : gpu_devices()) {
        ++machine.gpus;
        // What a desktop keeps for itself on a card that drives a screen, and
        // what any card needs beside a model's weights for its own work. Not
        // on a Mac, whose figure is already the share a model may have.
        const std::uint64_t keep = machine.apple ? 0 : 512ULL << 20;
        machine.vram += gpu.memory_total > keep ? gpu.memory_total - keep : 0;
    }
    std::uint64_t used  = 0;
    std::uint64_t total = 0;
    if (util::system_memory(used, total)) {
        machine.ram = total;
    }
    return machine;
}

std::string_view fit_id(Fit fit) {
    switch (fit) {
        case Fit::Gpu:      return "gpu";
        case Fit::Partial:  return "partial";
        case Fit::Cpu:      return "cpu";
        case Fit::TooLarge: return "large";
        case Fit::MacOnly:  return "mac";
    }
    return "large";
}

Fit fit(std::uint64_t bytes, Format format, const Machine& machine) {
    // The weights, and room beside them for the context and the work of a
    // reply: a twentieth more and a gigabyte, which is what a model at its
    // default context takes on top of its file.
    const std::uint64_t need = bytes + bytes / 20 + kGiB;
    // What the machine can lend beyond the cards: most of its memory, less
    // what the system and everything else open are using.
    const std::uint64_t spare = machine.ram > 4 * kGiB ? (machine.ram - 4 * kGiB) * 4 / 5 : 0;

    if (format == Format::Mlx) {
        if (!machine.apple) {
            return Fit::MacOnly;
        }
        // One memory, with a share of it a model may have on the GPU.
        const std::uint64_t room = machine.vram > 0 ? machine.vram : machine.ram * 3 / 4;
        return need <= room ? Fit::Gpu : Fit::TooLarge;
    }
    if (machine.gpus > 0 && need <= machine.vram) {
        return Fit::Gpu;
    }
    if (machine.gpus > 0 && need <= machine.vram + spare) {
        return Fit::Partial;
    }
    if (need <= spare) {
        return Fit::Cpu;
    }
    return Fit::TooLarge;
}

std::filesystem::path destination(const std::filesystem::path& models_dir, std::string_view repo) {
    // An author and a name, each a folder; anything in them that is not a
    // plain name becomes one.
    const auto clean = [](std::string part) {
        for (char& c : part) {
            if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32) {
                c = '_';
            }
        }
        if (part.empty() || part == "." || part == "..") {
            part = "_";
        }
        return part;
    };
    const std::size_t slash = repo.find('/');
    if (slash == std::string_view::npos) {
        return models_dir / clean(std::string(repo));
    }
    return models_dir / clean(std::string(repo.substr(0, slash))) / clean(std::string(repo.substr(slash + 1)));
}

std::filesystem::path landing(const std::filesystem::path& folder, const File& file, Format format) {
    if (format == Format::Gguf || !safe_relative(file.path)) {
        return folder / std::filesystem::path(file.path).filename();
    }
    return folder / std::filesystem::path(file.path);
}

std::string file_url(std::string_view repo, std::string_view path) {
    std::string url = std::string(kSite) + std::string(repo) + "/resolve/main/";
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        url += encode(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
        if (slash == std::string_view::npos) {
            break;
        }
        url += '/';
        start = slash + 1;
    }
    return url;
}

}  // namespace crucible::market
