// SPDX-License-Identifier: MIT
//
// The shelf's terminal (tools/terminal.hpp), and keeping what the shelf
// holds: a build as one archive, a thing on it shown in the viewer.
#include "test_helpers.hpp"

#include <chrono>
#include <thread>

#include <miniz.h>

#include "crucible/api/surface.hpp"
#include "crucible/session/shelf.hpp"
#include "crucible/tools/terminal.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/text.hpp"
#include "crucible/util/zip.hpp"

namespace {

using namespace crucible;
namespace terminal = tools::terminal;

/// What terminal `id` prints until `wanted` is in it, or ten seconds pass.
std::string read_until(int id, const std::string& wanted) {
    std::string   seen;
    std::uint64_t since = 0;
    const auto    until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < until && seen.find(wanted) == std::string::npos) {
        const std::optional<terminal::Output> out = terminal::read(id, since);
        if (!out) {
            break;
        }
        seen += out->data;
        since = out->next;
        if (!out->running && out->data.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return seen;
}

/// A shell plain enough to be the same everywhere it runs.
terminal::Options plain_shell(const std::filesystem::path& where) {
    terminal::Options options;
    options.cwd  = where;
    options.cols = 100;
    options.rows = 30;
#if defined(_WIN32)
    options.command = {"cmd.exe"};
#else
    options.command = {"/bin/sh"};
#endif
    return options;
}

}  // namespace

TEST(a_terminal_runs_a_shell_in_the_project_and_says_what_it_prints) {
    const TempDir temp;
    std::string   error;
    const int     id = terminal::open(plain_shell(temp.path()), error);
    CHECK(id > 0);
    CHECK_EQ(error, std::string());
    if (id == 0) {
        return;
    }
    // What is typed is run, where the project is, and what it printed read.
#if defined(_WIN32)
    CHECK(terminal::write(id, "echo made-in-%CD%\r"));
    CHECK(read_until(id, temp.path().filename().string()).find(temp.path().filename().string()) != std::string::npos);
#else
    CHECK(terminal::write(id, "echo made-in-$((40+2))\r"));
    CHECK(read_until(id, "made-in-42").find("made-in-42") != std::string::npos);
    CHECK(terminal::write(id, "pwd\r"));
    CHECK(read_until(id, temp.path().filename().string()).find(temp.path().filename().string()) != std::string::npos);
    // It is a terminal, and the size it was given, then the size it is told.
    CHECK(terminal::write(id, "stty size\r"));
    CHECK(read_until(id, "30 100").find("30 100") != std::string::npos);
    CHECK(terminal::resize(id, 120, 40));
    CHECK(terminal::write(id, "stty size\r"));
    CHECK(read_until(id, "40 120").find("40 120") != std::string::npos);
#endif
    // Listed while it is open, and gone once closed.
    bool listed = false;
    for (const terminal::Info& one : terminal::list()) {
        listed = listed || one.id == id;
    }
    CHECK(listed);
    CHECK(terminal::close(id));
    CHECK(!terminal::read(id, 0).has_value());
    CHECK(!terminal::write(id, "x"));
    CHECK(!terminal::close(id));
}

TEST(a_terminal_whose_shell_ends_says_how_and_takes_no_more) {
    const TempDir temp;
    std::string   error;
    const int     id = terminal::open(plain_shell(temp.path()), error);
    CHECK(id > 0);
    if (id == 0) {
        return;
    }
    CHECK(terminal::write(id, "exit 3\r"));
    bool ended = false;
    int  status = -1;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ended && std::chrono::steady_clock::now() < until) {
        const std::optional<terminal::Output> out = terminal::read(id, 0);
        ended  = out && !out->running;
        status = out ? out->status : -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(ended);
    CHECK_EQ(status, 3);
    CHECK(!terminal::write(id, "echo hello\r"));
    CHECK(terminal::close(id));
}

TEST(what_a_terminal_printed_is_read_on_from_where_the_reader_is_and_never_through_a_character) {
    const TempDir temp;
    std::string   error;
    const int     id = terminal::open(plain_shell(temp.path()), error);
    CHECK(id > 0);
    if (id == 0) {
        return;
    }
#if !defined(_WIN32)
    // Two reads in very small pieces put back together are what was printed,
    // whole characters each time.
    CHECK(terminal::write(id, "printf 'caf\\303\\251 \\342\\202\\254\\n'\r"));
    CHECK(read_until(id, "caf\xc3\xa9 \xe2\x82\xac").find("caf\xc3\xa9 \xe2\x82\xac") != std::string::npos);
    std::string   again;
    std::uint64_t since = 0;
    for (int step = 0; step < 10000; ++step) {
        const std::optional<terminal::Output> out = terminal::read(id, since, 5);
        if (!out || out->data.empty()) {
            break;
        }
        CHECK(crucible::detail::is_utf8(out->data));
        again += out->data;
        since = out->next;
    }
    CHECK(again.find("caf\xc3\xa9 \xe2\x82\xac") != std::string::npos);
#endif
    terminal::close_all();
    CHECK(!terminal::read(id, 0).has_value());
}

TEST(a_build_saved_as_one_archive_is_one_every_archive_program_reads) {
    const TempDir temp;
    const std::filesystem::path build = temp.path() / "site";
    std::filesystem::create_directories(build / "css");
    std::filesystem::create_directories(build / "node_modules" / "left-pad");
    std::filesystem::create_directories(build / ".git");
    {
        std::ofstream(build / "index.html") << "<h1>Hi</h1>\n" << std::string(5000, 'x');
        std::ofstream(build / "css" / "style.css") << "body { color: red; }\n";
        std::ofstream(build / "node_modules" / "left-pad" / "index.js") << "module.exports = 1;\n";
        std::ofstream(build / ".git" / "HEAD") << "ref: refs/heads/main\n";
        std::ofstream(build / "empty.txt");
    }
    const std::filesystem::path target = temp.path() / "out" / "site.zip";
    CHECK_EQ(shelf::save_archive(build, target), std::string());
    CHECK(std::filesystem::exists(target));
    CHECK(!std::filesystem::exists(temp.path() / "out" / "site.zip.part"));

    // miniz's reader, which is strict about the headers, finds each file
    // whole and nothing of what an archive of a build leaves out.
    mz_zip_archive zip{};
    CHECK(mz_zip_reader_init_file(&zip, target.string().c_str(), 0) != 0);
    CHECK_EQ(mz_zip_reader_get_num_files(&zip), mz_uint{3});
    std::set<std::string> names;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat;
        CHECK(mz_zip_reader_file_stat(&zip, i, &stat) != 0);
        names.insert(stat.m_filename);
    }
    CHECK(names.count("index.html") == 1 && names.count("css/style.css") == 1 && names.count("empty.txt") == 1);
    std::size_t size = 0;
    void*       page = mz_zip_reader_extract_file_to_heap(&zip, "index.html", &size, 0);
    CHECK(page != nullptr);
    if (page != nullptr) {
        const std::string text(static_cast<const char*>(page), size);
        CHECK(text.rfind("<h1>Hi</h1>\n", 0) == 0);
        CHECK_EQ(text.size(), std::size_t{12 + 5000});
        mz_free(page);
    }
    CHECK(mz_zip_validate_archive(&zip, 0) != 0);
    mz_zip_reader_end(&zip);

    // Named as the build, safe for a file name.
    CHECK_EQ(shelf::archive_name("My site: v2?"), std::string("My site- v2-.zip"));
    CHECK_EQ(shelf::archive_name(""), std::string("build.zip"));
    // An empty build is not saved.
    const std::filesystem::path empty = temp.path() / "empty";
    std::filesystem::create_directories(empty);
    CHECK(!shelf::save_archive(empty, temp.path() / "empty.zip").empty());
}

TEST(the_shelf_viewer_is_told_how_to_show_each_kind_of_thing) {
    const TempDir temp;
    const std::filesystem::path root = temp.path();
    {
        std::ofstream(root / "notes.md") << "# Plan\n\nSome *words*.\n";
        std::ofstream(root / "report.pdf", std::ios::binary) << "%PDF-1.4\n%%EOF\n";
        std::ofstream(root / "clip.mp4", std::ios::binary) << std::string(10, '\0');
        std::ofstream(root / "blob.bin", std::ios::binary) << std::string("\x00\x01\x02", 3);
    }
    struct Host : api::Host {
        std::filesystem::path root;
        std::filesystem::path project_root() const override { return root; }
    } host;
    host.root = root;
    api::Surface surface(host);
    const auto ask = [&surface](const nlohmann::json& request) {
        return nlohmann::json::parse(surface.handle(request.dump()));
    };
    const auto view = [&ask](const std::string& path) {
        return ask({{"id", 1}, {"method", "shelf.view"}, {"params", {{"path", path}}}})["result"];
    };
    const nlohmann::json notes = view("notes.md");
    CHECK_EQ(notes.value("show", std::string()), std::string("text"));
    CHECK_EQ(notes.value("language", std::string()), std::string("md"));
    CHECK(notes.value("text", std::string()).find("# Plan") != std::string::npos);
    CHECK_EQ(view("report.pdf").value("show", std::string()), std::string("pdf"));
    CHECK_EQ(view("report.pdf").value("mime", std::string()), std::string("application/pdf"));
    CHECK_EQ(view("clip.mp4").value("show", std::string()), std::string("video"));
    CHECK_EQ(view("blob.bin").value("show", std::string()), std::string("none"));
    // Outside the project is not there, whatever the path says.
    const nlohmann::json outside =
        ask({{"id", 1}, {"method", "shelf.view"}, {"params", {{"path", "../../etc/passwd"}}}});
    CHECK(outside["ok"] == false);

    // The bytes, in pieces put together, are the file.
    const nlohmann::json bytes = ask({{"id", 1}, {"method", "shelf.bytes"}, {"params", {{"path", "report.pdf"}}}});
    CHECK(bytes["ok"] == true);
    std::string decoded;
    CHECK(format::from_base64(bytes["result"].value("data", std::string()), decoded));
    CHECK_EQ(decoded, std::string("%PDF-1.4\n%%EOF\n"));
    CHECK(bytes["result"].value("done", false));
}
