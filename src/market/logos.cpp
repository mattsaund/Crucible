// SPDX-License-Identifier: MIT
//
// See logos.hpp.
#include "crucible/market/logos.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/market/hub.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::market {
namespace {

using json = nlohmann::json;

constexpr const char* kSite = "https://huggingface.co/";

/// The most a picture may be. The hub sends a logo 200 pixels square, a few
/// kilobytes; nothing near this size is a logo.
constexpr std::uint64_t kMostBytes = 256 * 1024;

/// The most the folder keeps, a thousand logos and more.
constexpr std::uint64_t kMostKept = 8 * 1024 * 1024;

/// How many repositories' owners are remembered.
constexpr std::size_t kMostRemembered = 5000;

/// How long a picture is believed, and how long "none" is: an owner with no
/// picture is likelier to add one than an owner with one is to change it.
constexpr std::chrono::hours kPictureFor{24 * 30};
constexpr std::chrono::hours kNoneFor{24 * 7};

/// A name the hub could have for an owner or a repository: letters, digits,
/// '-', '_' and '.', with no dot at either end. Checked before a name goes
/// into an address or a file name, since it comes from a page; and Windows
/// drops a dot that ends a file's name, which would make two names one.
bool hub_name(std::string_view name) {
    if (name.empty() || name.size() > 96 || name.front() == '.' || name.back() == '.') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.';
    });
}

/// "owner/name", each a name the hub could have.
bool hub_repo(std::string_view id) {
    const std::size_t slash = id.find('/');
    return slash != std::string_view::npos && hub_name(id.substr(0, slash)) && hub_name(id.substr(slash + 1));
}

/// The file an owner's logo is kept in: named for the owner in lower case,
/// as the hub's names are, after "logo-". A name like "con" or "nul" on its
/// own is a device on Windows, not a file.
std::filesystem::path logo_file(const std::filesystem::path& folder, std::string_view owner) {
    return folder / ("logo-" + format::to_lower(owner));
}

/// A name no other writer is using, to write beside the file it replaces.
std::filesystem::path part_file(const std::filesystem::path& folder) {
    static std::atomic<unsigned> count{0};
    static const unsigned        salt = std::random_device{}();
    return folder / ("." + std::to_string(salt) + "-" + std::to_string(count++) + ".part");
}

std::string read_all(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Write `bytes` to `file` at once: into a part file beside it, then moved
/// over it, so nothing ever reads half of one.
bool write_whole(const std::filesystem::path& file, std::string_view bytes) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path part = part_file(file.parent_path());
    bool written = false;
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        written = !out.fail();
    }
    if (written) {
        std::filesystem::rename(part, file, ec);
        written = !ec;
    }
    if (!written) {
        std::filesystem::remove(part, ec);
    }
    return written;
}

/// Keep the folder under kMostKept, the logos fetched longest ago going
/// first. Taken down to three quarters of it, so it is not listed again for
/// every logo after. A part file a write never finished goes too.
void make_room(const std::filesystem::path& folder) {
    struct One {
        std::filesystem::path           path;
        std::filesystem::file_time_type written;
        std::uintmax_t                  bytes = 0;
    };
    std::vector<One> kept;
    std::uintmax_t   total = 0;
    const auto       now   = std::filesystem::file_time_type::clock::now();
    std::error_code  ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        std::error_code   one_ec;
        const auto        written = it->last_write_time(one_ec);
        if (one_ec) {
            continue;
        }
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0) {
            if (now - written > std::chrono::hours(1)) {
                std::filesystem::remove(it->path(), one_ec);
            }
            continue;
        }
        const std::uintmax_t bytes = it->file_size(one_ec);
        if (name.rfind("logo-", 0) != 0 || one_ec) {
            continue;
        }
        kept.push_back({it->path(), written, bytes});
        total += bytes;
    }
    if (total <= kMostKept) {
        return;
    }
    std::sort(kept.begin(), kept.end(), [](const One& a, const One& b) { return a.written < b.written; });
    for (const One& one : kept) {
        if (total <= kMostKept / 4 * 3) {
            break;
        }
        if (std::filesystem::remove(one.path, ec)) {
            total -= one.bytes;
        }
    }
}

// --- whose logo each repository has ------------------------------------------

/// What logos_dir() remembers, by repository id in lower case: whose logo it
/// has, and when that was noted, so the oldest go when there are too many.
/// Kept as owners.json, read once and held here after.
struct Remembered {
    std::filesystem::path                                        folder;
    bool                                                         read = false;
    std::map<std::string, std::pair<std::string, std::int64_t>> owners;
};

std::mutex& remembered_mutex() {
    static std::mutex mutex;
    return mutex;
}

/// The index for `folder`, read the first time it is asked for. Under the
/// mutex. Read again when the folder is another, as it is in a test.
Remembered& remembered_in(const std::filesystem::path& folder) {
    static Remembered index;
    if (index.read && index.folder == folder) {
        return index;
    }
    index        = Remembered{};
    index.folder = folder;
    index.read   = true;
    const json parsed = json::parse(read_all(folder / "owners.json"), nullptr, false);
    if (!parsed.is_object()) {
        return index;
    }
    for (const auto& item : parsed.items()) {
        const json& entry = item.value();
        if (entry.is_array() && entry.size() == 2 && entry[0].is_string() && entry[1].is_number_integer()) {
            index.owners[item.key()] = {entry[0].get<std::string>(), entry[1].get<std::int64_t>()};
        }
    }
    return index;
}

void save(Remembered& index) {
    if (index.owners.size() > kMostRemembered) {
        std::vector<std::pair<std::int64_t, std::string>> by_age;
        by_age.reserve(index.owners.size());
        for (const auto& [repo, entry] : index.owners) {
            by_age.emplace_back(entry.second, repo);
        }
        std::sort(by_age.begin(), by_age.end());
        const std::size_t drop = index.owners.size() - kMostRemembered / 5 * 4;
        for (std::size_t i = 0; i < drop; ++i) {
            index.owners.erase(by_age[i].second);
        }
    }
    json out = json::object();
    for (const auto& [repo, entry] : index.owners) {
        out[repo] = json::array({entry.first, entry.second});
    }
    write_whole(index.folder / "owners.json", out.dump());
}

// --- asking the hub ------------------------------------------------------------

/// What the hub says `repo` was made from. True when it answered, with
/// `base` empty for a repository made from no other or one it would not
/// describe; false when it could not be reached.
bool ask_base_model(std::string_view repo, std::string& base) {
    base.clear();
    util::http::Request request;
    // Its tags and its card, read as a search's are (hub.cpp, listing_of).
    request.url             = std::string(kSite) + "api/models/" + std::string(repo)
                            + "?expand%5B%5D=tags&expand%5B%5D=cardData";
    request.headers         = {{"Accept", "application/json"}};
    request.timeout_seconds = 15;
    const util::http::Response response = util::http::send(request);
    if (!response.error.empty() || response.status == 0 || response.status == 408 || response.status == 429
        || response.status >= 500) {
        return false;
    }
    if (response.ok()) {
        base = parse_model(response.body, Format::Gguf).listing.base_model;
    }
    return true;
}

/// Fetch `url` into the file `to`: the status the last of its redirects
/// answered with, or 0, with `error`, when nothing did. Not util::http::send,
/// which reads a reply a line at a time: a picture is bytes, and its "\r\n"
/// would come back as "\n". `too_large` is set when it was refused for its
/// size.
int fetch_to(const std::string& url, const std::filesystem::path& to, bool& too_large, std::string& error) {
    too_large = false;
    const util::Ran ran = util::run(
        {"curl", "--silent", "--show-error", "--location", "--max-redirs", "3",
         // https only, the redirect included: a picture from anywhere else is
         // not the hub's.
         "--proto", "=https", "--proto-redir", "=https", "--max-filesize", std::to_string(kMostBytes),
         "--connect-timeout", "10", "--max-time", "15", "--user-agent", std::string("Crucible/") + CRUCIBLE_VERSION,
         // curl turns the \n into a line break itself: a real one in an
         // argument is not safe on every system's command line.
         "--output", to.string(), "--write-out", "\\n[[logo %{http_code}]]\\n", url},
        {}, {}, 25);
    if (!ran.started) {
        error = ran.error;
        return 0;
    }
    if (ran.stopped || ran.timed_out) {
        error = ran.stopped ? "stopped" : "Hugging Face took too long";
        return 0;
    }
    if (ran.status == 63) {   // curl's own number for "larger than --max-filesize"
        too_large = true;
        return 0;
    }
    const std::size_t marker = ran.output.rfind("[[logo ");
    const int status = marker == std::string::npos ? 0 : std::atoi(ran.output.c_str() + marker + 7);
    if (ran.status != 0 || status == 0) {
        // curl says why on a line of its own.
        const std::size_t said = ran.output.find("curl: (");
        error = "Hugging Face could not be reached";
        if (said != std::string::npos) {
            error += " (" + ran.output.substr(said, ran.output.find('\n', said) - said) + ")";
        }
        return 0;
    }
    return status;
}

/// What asking the hub for an owner's picture came to.
enum class Fetched { Picture, None, Failed };

Fetched fetch_logo(const std::filesystem::path& folder, std::string_view owner, Logo& out, std::string& error) {
    out       = Logo{};
    out.owner = std::string(owner);
    if (!util::http::available(error)) {
        return Fetched::Failed;
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path part   = part_file(folder);
    Fetched                     result = Fetched::None;
    // As an organization, which most owners are, and then as a user: the hub
    // knows a name as one of the two and says it does not know the other.
    for (const bool user : {false, true}) {
        bool      too_large = false;
        const int status    = fetch_to(avatar_url(owner, user), part, too_large, error);
        if (status == 404 && !too_large) {
            continue;
        }
        if (status == 200) {
            std::string            bytes = read_all(part);
            const std::string_view type  = picture_type(bytes);
            // A user who never chose a picture has a pattern the hub draws for
            // them, which is no logo, and is not a picture of the four kinds.
            if (!type.empty() && bytes.size() <= kMostBytes) {
                out.mime  = std::string(type);
                out.bytes = std::move(bytes);
                result    = Fetched::Picture;
            }
        } else if (!too_large && (status == 0 || status == 408 || status == 429 || status >= 500)) {
            result = Fetched::Failed;
        }
        break;
    }
    std::filesystem::remove(part, ec);
    return result;
}

}  // namespace

std::string logo_owner(std::string_view repo, std::string_view base_model) {
    // A base model named without an owner, "gpt2" from before the hub had
    // them, names nobody: the repository's own owner is next.
    for (const std::string_view id : {base_model, repo}) {
        if (hub_repo(id)) {
            return std::string(id.substr(0, id.find('/')));
        }
    }
    return {};
}

std::string_view picture_type(std::string_view bytes) {
    const auto starts = [bytes](std::string_view with, std::size_t at = 0) {
        return bytes.size() >= at + with.size() && bytes.substr(at, with.size()) == with;
    };
    if (starts(std::string_view("\x89" "PNG\r\n\x1a\n", 8))) {
        return "image/png";
    }
    if (starts("\xFF\xD8\xFF")) {
        return "image/jpeg";
    }
    if (starts("GIF87a") || starts("GIF89a")) {
        return "image/gif";
    }
    if (starts("RIFF") && starts("WEBP", 8)) {
        return "image/webp";
    }
    return {};
}

std::string avatar_url(std::string_view owner, bool user) {
    return std::string(kSite) + (user ? "api/users/" : "api/organizations/") + std::string(owner)
         + "/avatar?redirect=true";
}

std::filesystem::path logos_dir() { return paths::data_dir() / "logos"; }

bool kept_logo(const std::filesystem::path& folder, std::string_view owner, Logo& out, bool& fresh) {
    out   = Logo{};
    fresh = false;
    if (!hub_name(owner)) {
        return false;
    }
    const std::filesystem::path file = logo_file(folder, owner);
    std::error_code             ec;
    const auto                  written = std::filesystem::last_write_time(file, ec);
    const std::uintmax_t        size    = ec ? 0 : std::filesystem::file_size(file, ec);
    if (ec || size > kMostBytes) {
        return false;
    }
    std::string            bytes = read_all(file);
    const std::string_view type  = picture_type(bytes);
    // Anything else in the file was not written whole by keep_logo, or went
    // while it was read: as good as not kept.
    if (bytes.size() != size || (!bytes.empty() && type.empty())) {
        return false;
    }
    out.owner = std::string(owner);
    out.mime  = std::string(type);
    out.bytes = std::move(bytes);
    fresh     = std::filesystem::file_time_type::clock::now() - written < (out.mime.empty() ? kNoneFor : kPictureFor);
    return true;
}

void keep_logo(const std::filesystem::path& folder, const Logo& logo) {
    const bool picture = !logo.mime.empty();
    if (!hub_name(logo.owner) || (picture && (logo.bytes.size() > kMostBytes || picture_type(logo.bytes).empty()))) {
        return;
    }
    if (write_whole(logo_file(folder, logo.owner), picture ? std::string_view(logo.bytes) : std::string_view())) {
        make_room(folder);
    }
}

void remember_logos(const std::vector<Listing>& listings) {
    const std::filesystem::path       folder = logos_dir();
    const auto                        now    = static_cast<std::int64_t>(std::time(nullptr));
    const std::lock_guard<std::mutex> lock(remembered_mutex());
    Remembered&                       index   = remembered_in(folder);
    bool                              changed = false;
    for (const Listing& listing : listings) {
        if (!hub_repo(listing.id)) {
            continue;
        }
        auto&             entry = index.owners[format::to_lower(listing.id)];
        const std::string owner = logo_owner(listing.id, listing.base_model);
        if (entry.first != owner) {
            entry   = {owner, now};
            changed = true;
        }
    }
    if (changed) {
        save(index);
    }
}

std::string remembered_owner(std::string_view repo) {
    const std::lock_guard<std::mutex> lock(remembered_mutex());
    const Remembered& index = remembered_in(logos_dir());
    const auto        found = index.owners.find(format::to_lower(repo));
    return found == index.owners.end() ? std::string() : found->second.first;
}

bool logo(std::string_view repo, Logo& out, std::string& error) {
    out = Logo{};
    if (!hub_repo(repo)) {
        error = "which repository?";
        return false;
    }
    std::string owner = remembered_owner(repo);
    if (owner.empty()) {
        // Asked for by its id alone, and no search has shown it: what its card
        // says it was made from is asked of the hub and remembered. When the
        // hub cannot say, the repository's own owner stands in, unremembered.
        std::string base;
        if (ask_base_model(repo, base)) {
            Listing listing;
            listing.id         = std::string(repo);
            listing.base_model = base;
            remember_logos({listing});
        }
        owner = logo_owner(repo, base);
    }
    const std::filesystem::path folder = logos_dir();
    Logo                        kept;
    bool                        fresh = false;
    const bool                  have  = kept_logo(folder, owner, kept, fresh);
    if (have && fresh) {
        out = std::move(kept);
        return true;
    }
    Logo fetched;
    if (fetch_logo(folder, owner, fetched, error) == Fetched::Failed) {
        if (!have) {
            out.owner = owner;
            return false;
        }
        out = std::move(kept);
        error.clear();
        return true;
    }
    keep_logo(folder, fetched);
    out = std::move(fetched);
    return true;
}

}  // namespace crucible::market
