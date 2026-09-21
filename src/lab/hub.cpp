// SPDX-License-Identifier: MIT
#include "crucible/lab/hub.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

#include "crucible/util/subprocess.hpp"

namespace crucible::lab::hub {
namespace {

using json = nlohmann::json;

constexpr const char* kApi = "https://huggingface.co/api/";

/// Percent-encode what a search box can contain. The same rule web search
/// uses, and for the same reason: a query with a space in it is ordinary.
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

std::string_view segment(Kind kind) {
    return kind == Kind::Dataset ? "datasets" : "models";
}

/// The hub reports sizes in several shapes depending on the endpoint. Take a
/// number wherever one is, and nothing where there is not.
std::uint64_t number(const json& node, const char* key) {
    if (!node.contains(key)) {
        return 0;
    }
    const json& value = node[key];
    if (value.is_number_unsigned()) { return value.get<std::uint64_t>(); }
    if (value.is_number_integer())  { return static_cast<std::uint64_t>(std::max<std::int64_t>(0, value.get<std::int64_t>())); }
    if (value.is_number_float())    { return static_cast<std::uint64_t>(std::max(0.0, value.get<double>())); }
    return 0;
}

bool fetch(const std::string& url, std::string& body, std::string& error) {
    if (!util::on_path("curl")) {
        error = "curl is needed to browse Huggingface and is not installed";
        return false;
    }
    const std::vector<std::string> argv{
        "curl", "--silent", "--show-error", "--location", "--fail",
        "--max-time", "20",
        "--user-agent", "Crucible/0.1 (+local model lab)",
        "--header", "Accept: application/json",
        url,
    };
    util::Subprocess child;
    if (!child.start(argv, {}, /*extra_env=*/{}, error)) {
        return false;
    }
    std::string line;
    while (child.read_line(line)) {
        body += line;
        body += '\n';
    }
    if (const int status = child.wait(); status != 0) {
        error = "Huggingface could not be reached (curl exited "
              + std::to_string(status) + ")";
        return false;
    }
    return true;
}

}  // namespace

std::string search_url(Kind kind, std::string_view query, int limit) {
    // Sorted by downloads, because the whole difficulty of choosing a base
    // model is that there are two hundred thousand of them and the useful ones
    // are the ones other people already use.
    std::string url = std::string(kApi) + std::string(segment(kind))
                    + "?search=" + encode(query)
                    + "&limit=" + std::to_string(std::clamp(limit, 1, 100))
                    + "&sort=downloads&direction=-1";

    // Asked for by name, because a plain search does not carry them -- and
    // "gated" is the one that matters most: a repository behind an accepted
    // license is a download that fails after the user has waited for it.
    // Expanding replaces the default fields rather than adding to them, so
    // everything read below has to be named here. A dataset has no pipeline
    // tag and no weights, and asking it for them is an error.
    url += "&expand%5B%5D=gated&expand%5B%5D=downloads&expand%5B%5D=likes";
    if (kind == Kind::Model) {
        url += "&expand%5B%5D=pipeline_tag&expand%5B%5D=safetensors";
    }
    return url;
}

std::string tree_url(Kind kind, std::string_view id) {
    return std::string(kApi) + std::string(segment(kind)) + "/" + std::string(id)
         + "/tree/main";
}

std::string download_url(Kind kind, std::string_view id, std::string_view file) {
    // Datasets live under a prefix of their own; models sit at the root.
    const std::string prefix = kind == Kind::Dataset ? "datasets/" : "";
    return "https://huggingface.co/" + prefix + std::string(id) + "/resolve/main/"
         + std::string(file);
}

double parameters_from_name(std::string_view name) {
    // "Llama-3.2-1B" is one billion; "SmolLM-135M" is 0.135. Read right to
    // left, because the version numbers on the left look exactly like sizes:
    // taking the first match makes Llama 3.2 a three-billion model.
    double best = 0.0;
    for (std::size_t i = name.size(); i > 0; --i) {
        const char unit = static_cast<char>(std::toupper(static_cast<unsigned char>(name[i - 1])));
        if (unit != 'B' && unit != 'M') {
            continue;
        }
        // The digits immediately before it, with at most one decimal point.
        std::size_t end = i - 1;
        std::size_t start = end;
        bool seen_digit = false;
        bool seen_dot   = false;
        while (start > 0) {
            const char c = name[start - 1];
            if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
                seen_digit = true;
                --start;
            } else if ((c == '.' || c == ',') && !seen_dot && seen_digit) {
                seen_dot = true;
                --start;
            } else {
                break;
            }
        }
        if (!seen_digit) {
            continue;
        }
        // A size is a word of its own: "7B" and "-7B", not the "2" of "Qwen2".
        if (start > 0) {
            const char before = name[start - 1];
            if (std::isalnum(static_cast<unsigned char>(before)) != 0) {
                continue;
            }
        }
        std::string digits(name.substr(start, end - start));
        std::replace(digits.begin(), digits.end(), ',', '.');
        const double value = std::strtod(digits.c_str(), nullptr);
        if (value <= 0.0) {
            continue;
        }
        best = unit == 'M' ? value / 1000.0 : value;
        break;
    }
    return best;
}

std::vector<Item> parse_search(std::string_view json_text) {
    std::vector<Item> items;
    const json parsed = json::parse(json_text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return items;
    }
    for (const json& node : parsed) {
        if (!node.is_object()) {
            continue;
        }
        Item item;
        item.id = node.value("id", node.value("modelId", std::string()));
        if (item.id.empty()) {
            continue;
        }
        if (const std::size_t slash = item.id.find('/'); slash != std::string::npos) {
            item.author = item.id.substr(0, slash);
        }
        item.summary   = node.value("pipeline_tag", node.value("task_categories", std::string()));
        item.downloads = number(node, "downloads");
        item.likes     = number(node, "likes");
        // "gated" is false, "auto" or "manual": anything but false needs a
        // browser and an accepted license before a download will work.
        if (node.contains("gated") && !node["gated"].is_boolean()) {
            item.gated = true;
        } else {
            item.gated = node.value("gated", false);
        }

        if (node.contains("safetensors") && node["safetensors"].is_object()) {
            const std::uint64_t total = number(node["safetensors"], "total");
            if (total > 0) {
                item.parameters_b = static_cast<double>(total) / 1e9;
            }
        }
        if (item.parameters_b <= 0.0) {
            item.parameters_b = parameters_from_name(item.id);
        }
        items.push_back(std::move(item));
    }
    return items;
}

std::vector<File> parse_tree(std::string_view json_text) {
    std::vector<File> files;
    const json parsed = json::parse(json_text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return files;
    }
    for (const json& node : parsed) {
        if (!node.is_object() || node.value("type", "file") != "file") {
            continue;
        }
        File file;
        file.path = node.value("path", "");
        if (file.path.empty()) {
            continue;
        }
        file.bytes = number(node, "size");
        // A file stored in LFS reports its real size in a nested object, and
        // the outer one is the pointer file: a hundred and thirty bytes, which
        // is exactly the wrong number to show beside a model.
        if (node.contains("lfs") && node["lfs"].is_object()) {
            if (const std::uint64_t real = number(node["lfs"], "size"); real > 0) {
                file.bytes = real;
            }
        }
        files.push_back(std::move(file));
    }
    return files;
}

bool search(Kind kind, std::string_view query, int limit,
            std::vector<Item>& out, std::string& error) {
    std::string body;
    if (!fetch(search_url(kind, query, limit), body, error)) {
        return false;
    }
    out = parse_search(body);
    return true;
}

bool tree(Kind kind, std::string_view id, std::vector<File>& out, std::string& error) {
    std::string body;
    if (!fetch(tree_url(kind, id), body, error)) {
        return false;
    }
    out = parse_tree(body);
    return true;
}

}  // namespace crucible::lab::hub
