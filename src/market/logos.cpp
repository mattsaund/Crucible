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
#include "crucible/util/sha256.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::market {
namespace {

using json = nlohmann::json;

constexpr const char* kSite = "https://huggingface.co/";

/// The most a picture may be. The hub sends a logo 200 pixels square, a few
/// kilobytes, and a card's own picture is most often some tens of them;
/// nothing near this size is a logo.
constexpr std::uint64_t kMostBytes = 256 * 1024;

/// The most the folder keeps, a thousand logos and more.
constexpr std::uint64_t kMostKept = 8 * 1024 * 1024;

/// How many repositories' owners are remembered.
constexpr std::size_t kMostRemembered = 5000;

/// The longest address of a picture a card names that is asked for.
constexpr std::size_t kLongestAddress = 2048;

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

/// What a logo is kept under: an owner's name, or the address of a picture a
/// card names, which the cards of many repositories can share: a copy's card
/// is most often its base model's, picture and all.
bool logo_name(std::string_view name) {
    return hub_name(name) || (name.rfind("https://", 0) == 0 && name.size() <= kLongestAddress);
}

/// The file a logo is kept in, after "logo-": an owner's name in lower case,
/// as the hub's names are, or a picture's address by a digest of it, since an
/// address can hold anything. A name like "con" or "nul" on its own is a
/// device on Windows, not a file.
std::filesystem::path logo_file(const std::filesystem::path& folder, std::string_view owner) {
    if (hub_name(owner)) {
        return folder / ("logo-" + format::to_lower(owner));
    }
    util::Sha256 digest;
    digest.update(owner);
    return folder / ("logo-~" + digest.hex().substr(0, 32));
}

bool ends_with(std::string_view text, std::string_view end) {
    return text.size() >= end.size() && text.substr(text.size() - end.size()) == end;
}

/// Whether `host` is one of the hub's own: huggingface.co, hf.co, and those
/// under them, where it keeps what is uploaded.
bool hub_host(std::string_view host) {
    const bool plain = !host.empty() && std::all_of(host.begin(), host.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    });
    return plain && (host == "huggingface.co" || ends_with(host, ".huggingface.co") || host == "hf.co"
                     || ends_with(host, ".hf.co"));
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

/// What is remembered of one repository: whose logo it has, when that was
/// noted, so the oldest go when there are too many, and where the picture
/// of its own is, when it has one.
struct Noted {
    std::string  owner;
    std::int64_t when = 0;
    std::string  thumbnail;
};

/// What logos_dir() remembers, by repository id in lower case. Kept as
/// owners.json, read once and held here after.
struct Remembered {
    std::filesystem::path        folder;
    bool                         read = false;
    std::map<std::string, Noted> owners;
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
    // [owner, when], and the picture's address after them for a repository
    // with one of its own.
    for (const auto& item : parsed.items()) {
        const json& entry = item.value();
        if (entry.is_array() && (entry.size() == 2 || entry.size() == 3) && entry[0].is_string()
            && entry[1].is_number_integer() && (entry.size() == 2 || entry[2].is_string())) {
            index.owners[item.key()] = {entry[0].get<std::string>(), entry[1].get<std::int64_t>(),
                                        entry.size() == 3 ? entry[2].get<std::string>() : std::string()};
        }
    }
    return index;
}

void save(Remembered& index) {
    if (index.owners.size() > kMostRemembered) {
        std::vector<std::pair<std::int64_t, std::string>> by_age;
        by_age.reserve(index.owners.size());
        for (const auto& [repo, entry] : index.owners) {
            by_age.emplace_back(entry.when, repo);
        }
        std::sort(by_age.begin(), by_age.end());
        const std::size_t drop = index.owners.size() - kMostRemembered / 5 * 4;
        for (std::size_t i = 0; i < drop; ++i) {
            index.owners.erase(by_age[i].second);
        }
    }
    json out = json::object();
    for (const auto& [repo, entry] : index.owners) {
        out[repo] = entry.thumbnail.empty() ? json::array({entry.owner, entry.when})
                                            : json::array({entry.owner, entry.when, entry.thumbnail});
    }
    write_whole(index.folder / "owners.json", out.dump());
}

// --- asking the hub ------------------------------------------------------------

/// What the hub says of `repo`, in `out`: what it was made from and the
/// picture its card names. True when it answered, with both empty for a
/// repository made from no other and with no picture, or one it would not
/// describe; false when it could not be reached.
bool ask_listing(std::string_view repo, Listing& out) {
    out = Listing{};
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
        out = parse_model(response.body, Format::Gguf).listing;
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
         // Brackets and braces in a card's address are its own, not a set of
         // addresses for curl to fetch one by one.
         "--globoff",
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

/// What asking the hub for a picture came to.
enum class Fetched { Picture, None, Failed };

/// Ask `url` for a picture, into `out`, by way of the part file `part`.
/// `status` is what it answered with, 0 when nothing did.
Fetched fetch_picture(const std::string& url, const std::filesystem::path& part, Logo& out, int& status,
                      std::string& error) {
    bool too_large = false;
    status         = fetch_to(url, part, too_large, error);
    if (status == 200) {
        std::string            bytes = read_all(part);
        const std::string_view type  = picture_type(bytes);
        // A user who never chose a picture has a pattern the hub draws for
        // them, which is no logo, and is not a picture of the four kinds; nor
        // is a page that a card's address leads to.
        if (type.empty() || bytes.size() > kMostBytes) {
            return Fetched::None;
        }
        out.mime  = std::string(type);
        out.bytes = std::move(bytes);
        return Fetched::Picture;
    }
    return !too_large && (status == 0 || status == 408 || status == 429 || status >= 500) ? Fetched::Failed
                                                                                          : Fetched::None;
}

/// Ask for `owner`'s picture at the first of `urls` that has one, going on to
/// the next only when the hub says it has nothing there. `owner` is an
/// owner's name or a picture's address.
Fetched fetch_logo(const std::filesystem::path& folder, std::string_view owner,
                   const std::vector<std::string>& urls, Logo& out, std::string& error) {
    out       = Logo{};
    out.owner = std::string(owner);
    if (!util::http::available(error)) {
        return Fetched::Failed;
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path part   = part_file(folder);
    Fetched                     result = Fetched::None;
    for (const std::string& url : urls) {
        int status = 0;
        result     = fetch_picture(url, part, out, status, error);
        if (status != 404) {
            break;
        }
    }
    std::filesystem::remove(part, ec);
    return result;
}

/// The picture of its own that `repo`'s card names, at `url`, into `out` as
/// `repo`'s: kept under its address, or fetched once and kept, and an old one
/// when the hub cannot be reached. False when there is none to give, known or
/// for now, and then the owner's is shown instead.
bool own_logo(const std::filesystem::path& folder, std::string_view repo, const std::string& url, Logo& out) {
    Logo       kept;
    bool       fresh = false;
    const bool have  = kept_logo(folder, url, kept, fresh);
    Logo       fetched;
    Fetched    result = Fetched::Failed;
    if (!have || !fresh) {
        std::string ignored;
        result = fetch_logo(folder, url, {url}, fetched, ignored);
        if (result != Fetched::Failed) {
            // A picture, or that there is none, so the hub is not asked again soon.
            keep_logo(folder, fetched);
        }
    }
    // What was kept when the hub was not asked or could not be reached, and
    // otherwise what it said; either may be none.
    Logo& picture = result == Fetched::Failed ? kept : fetched;
    if (picture.mime.empty()) {
        return false;
    }
    out       = std::move(picture);
    out.owner = std::string(repo);
    return true;
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

std::string thumbnail_url(std::string_view repo, std::string_view thumbnail) {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!thumbnail.empty() && blank(thumbnail.front())) {
        thumbnail.remove_prefix(1);
    }
    while (!thumbnail.empty() && blank(thumbnail.back())) {
        thumbnail.remove_suffix(1);
    }
    if (!hub_repo(repo) || thumbnail.empty() || thumbnail.size() > kLongestAddress) {
        return {};
    }
    // An address: the hub's own, asked for over https whatever it says, and
    // written in nothing but what an address is written in.
    const std::string lowered = format::to_lower(thumbnail);
    for (const std::string_view scheme : {std::string_view("https://"), std::string_view("http://")}) {
        if (lowered.rfind(scheme, 0) != 0) {
            continue;
        }
        const std::string_view rest = thumbnail.substr(scheme.size());
        const bool printable = std::all_of(rest.begin(), rest.end(), [](char c) {
            return c > ' ' && c < 0x7F && c != '"' && c != '\\' && c != '<' && c != '>';
        });
        const std::size_t host_end = rest.find_first_of("/?#");
        if (!printable || host_end == std::string_view::npos || rest[host_end] != '/'
            || !hub_host(format::to_lower(rest.substr(0, host_end)))) {
            return {};
        }
        std::string url = "https://" + std::string(rest);
        return url.size() <= kLongestAddress ? url : std::string();
    }
    // A file of the repository, "logo.png" or "images/card.jpg", where the hub
    // serves it, each part of its name written as an address writes it. Not a
    // way out of the repository, nor an address of another kind, "data:".
    std::string_view path = thumbnail;
    while (path.rfind("./", 0) == 0) {
        path.remove_prefix(2);
    }
    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    if (path.empty() || path.find_first_of(":\\?#") != std::string_view::npos) {
        return {};
    }
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string           written;
    std::size_t           start = 0;
    while (start <= path.size()) {
        const std::size_t      end  = std::min(path.find('/', start), path.size());
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") {
            return {};
        }
        for (const char c : part) {
            const auto u = static_cast<unsigned char>(c);
            if (std::isalnum(u) != 0 || c == '-' || c == '.' || c == '_' || c == '~') {
                written += c;
            } else {
                written += '%';
                written += kHex[u >> 4];
                written += kHex[u & 0x0F];
            }
        }
        if (end < path.size()) {
            written += '/';
        }
        start = end + 1;
    }
    std::string url = std::string(kSite) + std::string(repo) + "/resolve/main/" + written;
    return url.size() <= kLongestAddress ? url : std::string();
}

std::string logo_key(const Listing& listing) {
    return thumbnail_url(listing.id, listing.thumbnail).empty() ? logo_owner(listing.id, listing.base_model)
                                                                : listing.id;
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
    if (!logo_name(owner)) {
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
    if (!logo_name(logo.owner) || (picture && (logo.bytes.size() > kMostBytes || picture_type(logo.bytes).empty()))) {
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
        Noted&            entry     = index.owners[format::to_lower(listing.id)];
        const std::string owner     = logo_owner(listing.id, listing.base_model);
        const std::string thumbnail = thumbnail_url(listing.id, listing.thumbnail);
        if (entry.owner != owner || entry.thumbnail != thumbnail) {
            entry   = {owner, now, thumbnail};
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
    return found == index.owners.end() ? std::string() : found->second.owner;
}

std::string remembered_thumbnail(std::string_view repo) {
    const std::lock_guard<std::mutex> lock(remembered_mutex());
    const Remembered& index = remembered_in(logos_dir());
    const auto        found = index.owners.find(format::to_lower(repo));
    return found == index.owners.end() ? std::string() : found->second.thumbnail;
}

bool logo(std::string_view repo, Logo& out, std::string& error) {
    out = Logo{};
    if (!hub_repo(repo)) {
        error = "which repository?";
        return false;
    }
    std::string owner     = remembered_owner(repo);
    std::string thumbnail = remembered_thumbnail(repo);
    if (owner.empty()) {
        // Asked for by its id alone, and no search has shown it: what its card
        // says it was made from, and the picture it names, are asked of the
        // hub and remembered. When the hub cannot say, the repository's own
        // owner stands in, unremembered.
        Listing listing;
        if (ask_listing(repo, listing)) {
            listing.id = std::string(repo);
            remember_logos({listing});
        }
        owner     = logo_owner(repo, listing.base_model);
        thumbnail = thumbnail_url(repo, listing.thumbnail);
    }
    const std::filesystem::path folder = logos_dir();
    if (!thumbnail.empty() && own_logo(folder, repo, thumbnail, out)) {
        return true;
    }
    Logo       kept;
    bool       fresh = false;
    const bool have  = kept_logo(folder, owner, kept, fresh);
    if (have && fresh) {
        out = std::move(kept);
        return true;
    }
    Logo fetched;
    // As an organization, which most owners are, and then as a user: the hub
    // knows a name as one of the two and says it does not know the other.
    if (fetch_logo(folder, owner, {avatar_url(owner, false), avatar_url(owner, true)}, fetched, error)
        == Fetched::Failed) {
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
