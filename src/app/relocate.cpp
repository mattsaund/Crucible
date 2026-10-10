// SPDX-License-Identifier: MIT
//
// Moving the Scratchpad: the folders first, then everything that names them.
//
// Each folder is moved by renaming it, which is instant and all or nothing,
// and a folder that will not move -- open in another program on Windows, say
// -- is left where it was with every record of it still pointing there, to be
// moved the next time. Nothing is copied and then deleted, so there is no
// moment at which a chat's work exists in neither place.
#include "crucible/app/relocate.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/session/store.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/platform.hpp"

namespace crucible::relocate {
namespace {

namespace fs = std::filesystem;
// Ordered, so that a file written back keeps its keys where they were.
using json = nlohmann::ordered_json;

/// A folder that moved: where it was, and where it is.
struct Move {
    fs::path from;
    fs::path to;
};

/// What a file browser leaves in a folder it has shown. Nobody's work, and
/// no reason to keep a folder that is otherwise empty.
bool is_litter(const fs::path& path) {
    const std::string name = path.filename().string();
    return name == ".DS_Store" || name == "Thumbs.db" || name == "desktop.ini";
}

/// Two parts of a path, compared as this system's filesystem compares them.
bool same_part(const fs::path& a, const fs::path& b) {
#if defined(_WIN32)
    const std::wstring x = a.wstring();
    const std::wstring y = b.wstring();
    return std::equal(x.begin(), x.end(), y.begin(), y.end(),
                      [](wchar_t p, wchar_t q) { return std::towlower(p) == std::towlower(q); });
#else
    return a == b;
#endif
}

/// What is left of `path` below `folder` when it is `folder` or in it, a part
/// at a time -- so that /a/bc is not taken to be in /a/b.
std::optional<fs::path> below(const fs::path& path, const fs::path& folder) {
    auto part = path.begin();
    for (const fs::path& wanted : folder) {
        if (part == path.end() || !same_part(*part, wanted)) {
            return std::nullopt;
        }
        ++part;
    }
    fs::path rest;
    for (; part != path.end(); ++part) {
        rest /= *part;
    }
    return rest;
}

/// `text`, when it is a path in a folder that moved, with the folder put
/// where it went. `moves` is searched in order, so the deepest come first.
std::optional<std::string> moved(const std::string& text, const std::vector<Move>& moves) {
    for (const Move& move : moves) {
        // A path is the start of the text, so most strings -- a transcript's
        // replies -- are ruled out by their first letters, before any parsing.
        const std::string from = move.from.string();
        if (text.size() < from.size()
            || !std::equal(from.begin(), from.end(), text.begin(), [](char a, char b) {
#if defined(_WIN32)
                   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
#else
                   return a == b;
#endif
               })) {
            continue;
        }
        try {
            if (const std::optional<fs::path> rest = below(fs::path(text), move.from)) {
                return (rest->empty() ? move.to : move.to / *rest).string();
            }
        } catch (const std::exception&) {
            // Text that is no path this system can spell: not one of ours.
        }
    }
    return std::nullopt;
}

/// Every string in `node` that names something in a moved folder, renamed.
bool rewrite(json& node, const std::vector<Move>& moves) {
    if (node.is_string()) {
        if (const std::optional<std::string> now = moved(node.get_ref<const std::string&>(), moves)) {
            node = *now;
            return true;
        }
        return false;
    }
    bool changed = false;
    if (node.is_structured()) {
        for (json& item : node) {
            changed = rewrite(item, moves) || changed;
        }
    }
    return changed;
}

/// The same for a JSON file, written back only if something in it changed,
/// and laid out as it was: a chat's record with tabs, the rest with spaces.
void rewrite_file(const fs::path& file, const std::vector<Move>& moves) {
    std::string text;
    {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    json document = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !rewrite(document, moves)) {
        return;
    }
    const bool tabs = text.find("\n\t") != std::string::npos;
    const std::string out = tabs ? document.dump(1, '\t', false, json::error_handler_t::replace)
                                 : document.dump(2, ' ', false, json::error_handler_t::replace);
    // Beside it and then over it, so a file is never half written.
    fs::path fresh = file;
    fresh += ".moving";
    {
        std::ofstream write(fresh, std::ios::binary | std::ios::trunc);
        if (!write) {
            return;
        }
        write << out << '\n';
        if (!write) {
            std::error_code ec;
            fs::remove(fresh, ec);
            return;
        }
    }
    std::error_code ec;
    fs::rename(fresh, file, ec);
    if (ec) {
        fs::remove(fresh, ec);
    }
}

/// Whether `folder` holds nothing but litter.
bool only_litter(const fs::path& folder) {
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!is_litter(it->path())) {
            return false;
        }
    }
    return !ec;
}

/// Remove `folder` if it holds nothing but litter, the litter with it.
void remove_if_empty(const fs::path& folder) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec) || !only_litter(folder)) {
        return;
    }
    fs::remove_all(folder, ec);
}

/// Put what is in `from` into `to`. Where both have a file, the newer one is
/// kept: the same folder in both places means a Crucible from before was
/// still running after the first move and made it again, and what it wrote
/// last is the folder as it is now. A repository's own folder is not mixed
/// file by file -- the one already there is kept, and the other left where
/// it was, with nothing deleted.
void merge(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::exists(to, ec)) {
        fs::create_directories(to.parent_path(), ec);
        fs::rename(from, to, ec);
        return;
    }
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        entries.push_back(it->path());
    }
    for (const fs::path& entry : entries) {
        const fs::path there = to / entry.filename();
        std::error_code each;
        const bool folder = fs::is_directory(entry, each);
        if (!fs::exists(there, each)) {
            fs::rename(entry, there, each);
        } else if (folder && fs::is_directory(there, each)) {
            if (entry.filename() != ".git") {
                merge(entry, there);
            }
        } else if (!folder && !fs::is_directory(there, each)) {
            if (fs::last_write_time(entry, each) > fs::last_write_time(there, each)) {
                fs::remove(there, each);
                fs::rename(entry, there, each);
            } else {
                fs::remove(entry, each);   // superseded by the newer one already there
            }
        }
    }
    remove_if_empty(from);
}

/// The JSON files under `folder`, at any depth.
std::vector<fs::path> json_files(const fs::path& folder) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".json") {
            files.push_back(it->path());
        }
    }
    return files;
}

fs::path canonical_or_as_is(const fs::path& path) {
    std::error_code ec;
    fs::path out = fs::weakly_canonical(path, ec);
    return ec || out.empty() ? path : out;
}

}  // namespace

std::vector<std::string> scratchpad(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) {
        return {};
    }
    fs::create_directories(to, ec);
    if (ec) {
        return {"Could not make " + format::short_path(to) + " (" + ec.message() + "), so the Scratchpad is still in "
                + format::short_path(from) + "."};
    }

    std::vector<fs::path> entries;
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end());

    // The records of what happened in a folder are kept under a name made
    // from the folder's path -- worked out while the folder is still there to
    // be resolved, and again where it lands.
    std::vector<Move> moves;
    std::vector<Move> histories;
    std::vector<std::string> said;
    std::size_t folders = 0;
    for (const fs::path& entry : entries) {
        std::error_code each;
        if (is_litter(entry)) {
            fs::remove(entry, each);
            continue;
        }
        const Project before = Project::at(entry);
        fs::path target = to / entry.filename();
        if (fs::is_directory(target, each) && fs::is_directory(entry, each)) {
            // Moved before, and made again by a Crucible that was still
            // running in it: the same chat's folder, put back together.
            merge(entry, target);
        } else {
            for (int n = 2; fs::exists(target, each) && n < 1000; ++n) {
                target = to / (entry.filename().string() + "-" + std::to_string(n));
            }
            fs::rename(entry, target, each);
        }
        if (each) {
            said.push_back("Could not move " + format::short_path(entry) + " (" + each.message()
                           + "). It is where it was, and will be moved the next time Crucible starts.");
            continue;
        }
        const Project after = Project::at(target);
        moves.push_back({before.root, after.root});
        histories.push_back({before.dir, after.dir});
        if (fs::is_directory(target, each)) {
            ++folders;
        }
    }

    // The Scratchpad itself, once there is nothing left in it: a project
    // somebody opened at it, and the trust given to it.
    const Project whole_before = Project::at(from);
    remove_if_empty(from);
    if (!fs::exists(from, ec)) {
        const Project whole_after = Project::at(to);
        moves.push_back({whole_before.root, whole_after.root});
        histories.push_back({whole_before.dir, whole_after.dir});
        // And the folder as it was spelled, should that differ from how it
        // resolves -- a home folder reached through a link.
        if (canonical_or_as_is(to) != to || whole_before.root != from) {
            moves.push_back({from, to});
        }
    }

    for (const Move& history : histories) {
        if (fs::is_directory(history.from, ec) && history.from != history.to) {
            merge(history.from, history.to);
        }
    }
    for (const Move& history : histories) {
        for (const fs::path& file : json_files(history.to)) {
            rewrite_file(file, moves);
        }
    }
    rewrite_file(paths::projects_dir() / "recent.json", moves);
    rewrite_file(paths::trust_file(), moves);

    if (!moves.empty()) {
        std::string line = "The Scratchpad is in " + format::short_path(to) + " now, out of sight";
        if (folders > 0) {
            line += folders == 1 ? ", and its folder" : ", and its " + std::to_string(folders) + " folders";
            line += " and their chats moved with it";
        }
        said.insert(said.begin(), line + ".");
    }
    return said;
}

std::vector<std::string> scratchpad() {
    const fs::path from = paths::old_scratchpad_dir();
    std::error_code ec;
    if (!fs::is_directory(from, ec)) {
        return {};
    }
    std::vector<std::string> said = scratchpad(from, paths::scratchpad_dir());
    util::hide_folder(paths::home_folder());
    // ~/Crucible was made to hold the Scratchpad. Gone with it, unless
    // something of the person's own is in there too.
    remove_if_empty(from.parent_path());
    return said;
}

}  // namespace crucible::relocate
