// SPDX-License-Identifier: MIT
//
// Inlining a page's local files. See preview.hpp.
#include "crucible/tools/preview.hpp"

#include <algorithm>
#include <optional>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include "crucible/tools/workshop.hpp"
#include "crucible/util/format.hpp"

namespace crucible::tools::preview {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool read_file(const std::filesystem::path& path, std::size_t max_bytes, std::string& out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || std::filesystem::file_size(path, ec) > max_bytes) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

/// Whether a reference is somewhere other than a file in the project.
bool is_remote(std::string_view href) {
    const std::string low = lower(href.substr(0, 12));
    return low.rfind("http:", 0) == 0 || low.rfind("https:", 0) == 0 || low.rfind("//", 0) == 0
        || low.rfind("data:", 0) == 0 || low.rfind("blob:", 0) == 0 || low.rfind("javascript:", 0) == 0
        || low.rfind("mailto:", 0) == 0 || low.rfind("#", 0) == 0;
}

/// The value of attribute `name` in `tag`, and where it sits, or npos.
std::string attribute(std::string_view tag, std::string_view name, std::size_t& value_at,
                      std::size_t& value_end) {
    const std::string low = lower(tag);
    std::size_t at = 0;
    while ((at = low.find(name, at)) != std::string::npos) {
        const bool starts = at == 0 || !(std::isalnum(static_cast<unsigned char>(low[at - 1])) != 0
                                         || low[at - 1] == '-' || low[at - 1] == '_');
        std::size_t after = at + name.size();
        while (after < low.size() && std::isspace(static_cast<unsigned char>(low[after])) != 0) {
            ++after;
        }
        if (!starts || after >= low.size() || low[after] != '=') {
            at += name.size();
            continue;
        }
        ++after;
        while (after < low.size() && std::isspace(static_cast<unsigned char>(low[after])) != 0) {
            ++after;
        }
        if (after >= tag.size()) {
            return {};
        }
        const char quote = tag[after];
        if (quote == '"' || quote == '\'') {
            const std::size_t end = tag.find(quote, after + 1);
            if (end == std::string::npos) {
                return {};
            }
            value_at  = after + 1;
            value_end = end;
            return std::string(tag.substr(value_at, value_end - value_at));
        }
        std::size_t end = after;
        while (end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[end])) && tag[end] != '>') {
            ++end;
        }
        value_at  = after;
        value_end = end;
        return std::string(tag.substr(value_at, value_end - value_at));
    }
    return {};
}

/// Strip a query or fragment off a reference: "style.css?v=3" is style.css.
std::string bare_reference(std::string_view href) {
    const std::size_t cut = href.find_first_of("?#");
    return std::string(cut == std::string_view::npos ? href : href.substr(0, cut));
}

struct Resolver {
    const std::filesystem::path& root;
    std::filesystem::path        page_dir;   ///< the folder the page is in, for relative references
    std::size_t                  max_bytes;
    Bundle*                      bundle;

    /// The file a reference names inside the project, or nothing.
    std::optional<std::filesystem::path> file_for(std::string_view href, const std::filesystem::path& from) const {
        std::string reference = bare_reference(href);
        if (reference.empty() || is_remote(reference)) {
            return std::nullopt;
        }
        // Percent-encoded spaces and the like, the two kinds a path in a page has.
        std::string decoded;
        for (std::size_t i = 0; i < reference.size(); ++i) {
            if (reference[i] == '%' && i + 2 < reference.size()
                && std::isxdigit(static_cast<unsigned char>(reference[i + 1])) != 0
                && std::isxdigit(static_cast<unsigned char>(reference[i + 2])) != 0) {
                decoded += static_cast<char>(std::stoi(reference.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                decoded += reference[i];
            }
        }
        // "/css/site.css" is from the project root, as a site's root; anything
        // else is from the folder of the file that referenced it.
        const std::filesystem::path relative = decoded.front() == '/' ? std::filesystem::path(decoded.substr(1))
                                                                     : from / decoded;
        return resolve_in_root(root, relative.generic_string());
    }

    std::string record(const std::filesystem::path& file, bool ok) {
        std::error_code ec;
        const std::filesystem::path shown = std::filesystem::relative(file, root, ec);
        const std::string name = ec || shown.empty() ? file.filename().string() : shown.generic_string();
        (ok ? bundle->inlined : bundle->skipped).push_back(name);
        return name;
    }

    /// `url(...)` references inside a stylesheet, as data: URIs.
    std::string css_with_data_uris(const std::string& css, const std::filesystem::path& from) {
        std::string out;
        std::size_t at = 0;
        while (true) {
            const std::size_t open = css.find("url(", at);
            if (open == std::string::npos) {
                out += css.substr(at);
                break;
            }
            const std::size_t close = css.find(')', open);
            if (close == std::string::npos) {
                out += css.substr(at);
                break;
            }
            std::string reference = css.substr(open + 4, close - open - 4);
            while (!reference.empty() && (reference.front() == ' ' || reference.front() == '"' || reference.front() == '\'')) {
                reference.erase(0, 1);
            }
            while (!reference.empty() && (reference.back() == ' ' || reference.back() == '"' || reference.back() == '\'')) {
                reference.pop_back();
            }
            out += css.substr(at, open - at);
            std::string data;
            if (const std::optional<std::filesystem::path> file = file_for(reference, from)) {
                const std::string mime = mime_of(*file);
                std::string bytes;
                if (!mime.empty() && read_file(*file, max_bytes, bytes)) {
                    data = "data:" + mime + ";base64," + format::base64(bytes);
                    record(*file, true);
                } else {
                    record(*file, false);
                }
            }
            out += "url(\"" + (data.empty() ? reference : data) + "\")";
            at = close + 1;
        }
        return out;
    }
};

}  // namespace

std::string mime_of(const std::filesystem::path& path) {
    static const std::map<std::string, std::string> types{
        {".png", "image/png"},   {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
        {".webp", "image/webp"}, {".svg", "image/svg+xml"}, {".ico", "image/x-icon"}, {".bmp", "image/bmp"},
        {".woff", "font/woff"},  {".woff2", "font/woff2"}, {".ttf", "font/ttf"}, {".otf", "font/otf"},
        {".css", "text/css"},    {".js", "text/javascript"}, {".mjs", "text/javascript"},
        {".json", "application/json"}, {".mp3", "audio/mpeg"}, {".wav", "audio/wav"},
        {".mp4", "video/mp4"},   {".webm", "video/webm"},
    };
    const auto found = types.find(lower(path.extension().string()));
    return found == types.end() ? std::string() : found->second;
}

Bundle bundle(const std::filesystem::path& root, std::string_view page, std::size_t max_bytes) {
    Bundle out;
    const std::optional<std::filesystem::path> file = resolve_in_root(root, page);
    if (!file) {
        out.error = std::string(page) + " is outside the project";
        return out;
    }
    std::string html;
    if (!read_file(*file, max_bytes, html)) {
        out.error = std::string(page) + " could not be read, or is larger than " + format::bytes(max_bytes);
        return out;
    }
    std::error_code ec;
    const std::filesystem::path page_dir =
        std::filesystem::relative(file->parent_path(), std::filesystem::weakly_canonical(root, ec), ec);
    Resolver resolver{root, ec ? std::filesystem::path() : page_dir, max_bytes, &out};

    // Each tag that references a file, in document order. The page is
    // rebuilt from pieces rather than edited in place so that offsets never
    // go stale under a replacement.
    std::string result;
    std::size_t at = 0;
    while (at < html.size()) {
        const std::size_t open = html.find('<', at);
        if (open == std::string::npos) {
            result += html.substr(at);
            break;
        }
        const std::size_t close = html.find('>', open);
        if (close == std::string::npos) {
            result += html.substr(at);
            break;
        }
        result += html.substr(at, open - at);
        std::string tag = html.substr(open, close - open + 1);
        const std::string low = lower(tag.substr(0, 10));
        at = close + 1;

        // Comments and the script a page already carries are left alone.
        if (low.rfind("<!--", 0) == 0) {
            const std::size_t end = html.find("-->", open);
            result += html.substr(open, end == std::string::npos ? std::string::npos : end + 3 - open);
            at = end == std::string::npos ? html.size() : end + 3;
            continue;
        }

        std::size_t value_at = 0, value_end = 0;
        if (low.rfind("<link", 0) == 0) {
            std::size_t rel_at = 0, rel_end = 0;
            const std::string rel  = lower(attribute(tag, "rel", rel_at, rel_end));
            const std::string href = attribute(tag, "href", value_at, value_end);
            const std::optional<std::filesystem::path> css =
                rel.find("stylesheet") != std::string::npos ? resolver.file_for(href, resolver.page_dir) : std::nullopt;
            std::string text;
            if (css && lower(css->extension().string()) == ".css" && read_file(*css, max_bytes, text)) {
                resolver.record(*css, true);
                const std::filesystem::path css_dir =
                    std::filesystem::relative(css->parent_path(), std::filesystem::weakly_canonical(root, ec), ec);
                result += "<style>\n" + resolver.css_with_data_uris(text, ec ? std::filesystem::path() : css_dir) + "\n</style>";
                continue;
            }
            if (css) {
                resolver.record(*css, false);
            }
            result += tag;
            continue;
        }
        if (low.rfind("<script", 0) == 0) {
            const std::string src = attribute(tag, "src", value_at, value_end);
            const std::optional<std::filesystem::path> js = resolver.file_for(src, resolver.page_dir);
            std::string text;
            if (js && read_file(*js, max_bytes, text)) {
                resolver.record(*js, true);
                // The tag's own attributes stay -- type="module" matters --
                // with the src taken out, and the file's text inside.
                std::string opened = tag;
                const std::size_t src_at = lower(opened).find("src");
                if (src_at != std::string::npos && value_end < opened.size()) {
                    opened.erase(src_at, value_end + 1 - src_at);
                }
                const std::size_t end = html.find("</script>", at);
                const std::size_t end_low = lower(html.substr(at, std::min<std::size_t>(html.size() - at, 400))).find("</script>");
                (void)end_low;
                result += opened + "\n" + text + "\n";
                if (end != std::string::npos) {
                    at = end;   // the closing tag is copied on the next pass
                } else {
                    result += "</script>";
                }
                continue;
            }
            if (js) {
                resolver.record(*js, false);
            }
            result += tag;
            continue;
        }
        if (low.rfind("<img", 0) == 0 || low.rfind("<source", 0) == 0 || low.rfind("<video", 0) == 0
            || low.rfind("<audio", 0) == 0) {
            const std::string src = attribute(tag, "src", value_at, value_end);
            const std::optional<std::filesystem::path> media = resolver.file_for(src, resolver.page_dir);
            std::string bytes;
            const std::string mime = media ? mime_of(*media) : std::string();
            if (media && !mime.empty() && read_file(*media, max_bytes, bytes)) {
                resolver.record(*media, true);
                tag.replace(value_at, value_end - value_at, "data:" + mime + ";base64," + format::base64(bytes));
            } else if (media) {
                resolver.record(*media, false);
            }
            result += tag;
            continue;
        }
        result += tag;
    }

    out.ok   = true;
    out.html = std::move(result);
    return out;
}

std::vector<std::string> candidates(const std::filesystem::path& root, std::size_t limit) {
    std::vector<std::string> found;
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::weakly_canonical(root, ec);
    if (ec || !std::filesystem::is_directory(base, ec)) {
        return found;
    }
    static const std::vector<std::string> skipped{"node_modules", ".git", "build", "dist", "out", "target",
                                                  "__pycache__", "venv", ".venv", "vendor", "coverage",
                                                  "DerivedData", "Pods"};
    for (std::filesystem::recursive_directory_iterator it{base, std::filesystem::directory_options::skip_permission_denied, ec}, end;
         it != end && !ec; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (std::find(skipped.begin(), skipped.end(), name) != skipped.end() || it.depth() >= 4
                || (name.size() > 1 && name.front() == '.')) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const std::string ext = lower(it->path().extension().string());
        if (ext != ".html" && ext != ".htm") {
            continue;
        }
        found.push_back(std::filesystem::relative(it->path(), base, ec).generic_string());
        if (found.size() >= limit * 4) {
            break;
        }
    }
    std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) {
        const bool a_index = a == "index.html" || a == "index.htm";
        const bool b_index = b == "index.html" || b == "index.htm";
        if (a_index != b_index) {
            return a_index;
        }
        const std::size_t a_depth = std::count(a.begin(), a.end(), '/');
        const std::size_t b_depth = std::count(b.begin(), b.end(), '/');
        return a_depth != b_depth ? a_depth < b_depth : a < b;
    });
    if (found.size() > limit) {
        found.resize(limit);
    }
    return found;
}

}  // namespace crucible::tools::preview
