// SPDX-License-Identifier: MIT
//
// Version control, and a page of the project made showable.
//
// git is run as a program, so the tests that commit need one on the machine
// and say so when there is none -- CI has it, and so does any machine that
// built this. The parsers of what git prints, and the page bundler, need
// nothing.
#include "test_helpers.hpp"

#include <nlohmann/json.hpp>

#include "crucible/api/surface.hpp"
#include "crucible/tools/git.hpp"
#include "crucible/tools/preview.hpp"

namespace {

void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

/// A host that owns a project and nothing else, for the lookups that read it.
class ProjectHost : public api::Host {
public:
    explicit ProjectHost(std::filesystem::path root) : root_(std::move(root)) {}
    std::filesystem::path project_root() const override { return root_; }
private:
    std::filesystem::path root_;
};

nlohmann::json ask(api::Surface& surface, const nlohmann::json& request) {
    return nlohmann::json::parse(surface.handle(request.dump()), nullptr, false);
}

}  // namespace

// ---------------------------------------------------------------------------
// What git says, read
// ---------------------------------------------------------------------------

TEST(git_status_is_read_off_the_porcelain_form) {
    const tools::git::Status status = tools::git::detail::parse_status(
        "## main...origin/main [ahead 2, behind 1]\n"
        " M src/a.py\n"
        "A  src/b.py\n"
        "?? notes.md\n"
        "R  old.txt -> new.txt\n");
    CHECK(status.repo);
    CHECK_EQ(status.branch, std::string("main"));
    CHECK_EQ(status.ahead, 2);
    CHECK_EQ(status.behind, 1);
    CHECK_EQ(status.changes.size(), std::size_t{4});
    CHECK_EQ(status.changes[0].status, std::string("M"));
    CHECK(!status.changes[0].staged);
    CHECK(status.changes[1].staged);
    CHECK_EQ(status.changes[2].status, std::string("??"));
    CHECK_EQ(status.changes[3].path, std::string("new.txt"));

    const tools::git::Status fresh = tools::git::detail::parse_status("## No commits yet on main\n?? a\n");
    CHECK_EQ(fresh.branch, std::string("main"));
    CHECK_EQ(fresh.changes.size(), std::size_t{1});
}

TEST(a_log_line_is_four_fields_and_a_short_one_is_skipped) {
    const std::vector<tools::git::Commit> commits = tools::git::detail::parse_log(
        "3f2a1c7\tAdd the parser\tMatt\t2 hours ago\n"
        "broken line\n"
        "9b8c7d6\tFirst\tCrucible\t3 days ago\n");
    CHECK_EQ(commits.size(), std::size_t{2});
    CHECK_EQ(commits[0].hash, std::string("3f2a1c7"));
    CHECK_EQ(commits[0].subject, std::string("Add the parser"));
    CHECK_EQ(commits[1].when, std::string("3 days ago"));
}

TEST(arguments_that_would_move_git_out_of_the_project_are_refused) {
    CHECK(tools::git::escaping_argument({"-C", "/elsewhere", "status"}) == "-C");
    CHECK(tools::git::escaping_argument({"--git-dir=/x", "log"}) == "--git-dir=/x");
    CHECK(tools::git::escaping_argument({"status", "--short"}).empty());
}

TEST(a_repository_can_be_started_committed_to_and_read_back) {
    if (!tools::git::available()) {
        std::printf("      (no git on PATH; skipped)\n");
        return;
    }
    TempDir dir;
    CHECK(!tools::git::is_repo(dir.path()));
    CHECK(tools::git::init(dir.path()).empty());
    CHECK(tools::git::is_repo(dir.path()));

    write(dir.path() / "a.txt", "one\n");
    tools::git::Status before = tools::git::status(dir.path());
    CHECK(before.repo);
    CHECK_EQ(before.changes.size(), std::size_t{1});
    CHECK_EQ(before.changes[0].status, std::string("??"));

    // The diff of an untracked file in a repository with no commit: the
    // whole file, as additions, rather than nothing.
    const std::string fresh = tools::git::diff(dir.path());
    CHECK(fresh.find("+one") != std::string::npos);

    std::string summary;
    const std::string error = tools::git::commit(dir.path(), "First commit", summary);
    CHECK(error.empty());
    if (!error.empty()) {
        std::printf("      %s\n", error.c_str());
    }
    CHECK(summary.rfind("committed ", 0) == 0);
    CHECK(summary.find("First commit") != std::string::npos);

    // Nothing more to commit says so rather than failing.
    std::string again;
    CHECK(tools::git::commit(dir.path(), "Nothing", again).empty());
    CHECK_EQ(again, std::string("nothing to commit"));

    write(dir.path() / "a.txt", "one\ntwo\n");
    const std::string diff = tools::git::diff(dir.path(), "a.txt");
    CHECK(diff.find("+two") != std::string::npos);
    const std::vector<tools::git::Commit> log = tools::git::log(dir.path());
    CHECK_EQ(log.size(), std::size_t{1});
    CHECK_EQ(log[0].subject, std::string("First commit"));

    // A message is required: an empty one is the mistake, not a feature.
    std::string none;
    CHECK(!tools::git::commit(dir.path(), "  ", none).empty());
}

// ---------------------------------------------------------------------------
// The preview
// ---------------------------------------------------------------------------

TEST(a_page_is_bundled_with_its_local_files_inside_it) {
    TempDir dir;
    write(dir.path() / "site" / "index.html",
          "<!doctype html><html><head><link rel=\"stylesheet\" href=\"css/site.css?v=2\">"
          "<link rel=\"stylesheet\" href=\"https://cdn.example/x.css\"></head>"
          "<body><img src=\"../logo.png\" alt=\"\"><script src=\"app.js\"></script>"
          "<script src=\"../../outside.js\"></script></body></html>");
    write(dir.path() / "site" / "css" / "site.css", "body{background:url('../bg.png')}");
    write(dir.path() / "site" / "bg.png", "PNGBYTES");
    write(dir.path() / "logo.png", "LOGO");
    write(dir.path() / "site" / "app.js", "console.log('hi')");

    const tools::preview::Bundle bundle = tools::preview::bundle(dir.path(), "site/index.html");
    CHECK(bundle.ok);
    if (!bundle.ok) {
        std::printf("      %s\n", bundle.error.c_str());
        return;
    }
    CHECK(bundle.html.find("<style>") != std::string::npos);
    CHECK(bundle.html.find("background:url(\"data:image/png;base64,") != std::string::npos);
    CHECK(bundle.html.find("console.log('hi')") != std::string::npos);
    CHECK(bundle.html.find("src=\"data:image/png;base64,") != std::string::npos);
    // What is on the web stays a reference; what leaves the project is not read.
    CHECK(bundle.html.find("https://cdn.example/x.css") != std::string::npos);
    CHECK(bundle.html.find("outside.js") != std::string::npos);
    CHECK(bundle.html.find("src=\"app.js\"") == std::string::npos);
    CHECK_EQ(bundle.inlined.size(), std::size_t{4});
    CHECK(bundle.skipped.empty());   // outside.js never resolved, so it is not a file that was skipped

    CHECK(!tools::preview::bundle(dir.path(), "../elsewhere.html").ok);
    CHECK(!tools::preview::bundle(dir.path(), "site/missing.html").ok);
}

TEST(candidate_pages_put_index_first_and_skip_what_nobody_previews) {
    TempDir dir;
    write(dir.path() / "docs" / "guide.html", "x");
    write(dir.path() / "index.html", "x");
    write(dir.path() / "about.html", "x");
    write(dir.path() / "node_modules" / "pkg" / "index.html", "x");
    write(dir.path() / "build" / "index.html", "x");
    const std::vector<std::string> pages = tools::preview::candidates(dir.path());
    CHECK_EQ(pages.size(), std::size_t{3});
    if (pages.size() == 3) {
        CHECK_EQ(pages[0], std::string("index.html"));
        CHECK_EQ(pages[1], std::string("about.html"));
        CHECK_EQ(pages[2], std::string("docs/guide.html"));
    }
}

// ---------------------------------------------------------------------------
// The surface's side
// ---------------------------------------------------------------------------

TEST(the_project_methods_read_inside_the_project_and_refuse_outside_it) {
    TempDir dir;
    write(dir.path() / "src" / "main.py", "print('hi')\n");
    ProjectHost   host(dir.path());
    api::Surface  surface(host);

    const nlohmann::json tree = ask(surface, {{"method", "project.tree"}});
    CHECK(tree["ok"] == true);
    CHECK(tree["result"]["entries"].size() == 2);   // src/, src/main.py

    const nlohmann::json read = ask(surface, {{"method", "project.read"}, {"params", {{"path", "src/main.py"}}}});
    CHECK(read["ok"] == true);
    CHECK(read["result"]["content"] == "print('hi')\n");
    CHECK(read["result"]["language"] == "py");

    const nlohmann::json outside = ask(surface, {{"method", "project.read"}, {"params", {{"path", "../secret"}}}});
    CHECK(outside["ok"] == false);
    CHECK(outside["error"].get<std::string>().find("outside") != std::string::npos);

    // With no project, every one of them says so rather than reading the disk.
    api::Host     nobody;
    api::Surface  bare(nobody);
    for (const char* method : {"project.tree", "project.read", "git.status", "preview.candidates", "teach.list"}) {
        const nlohmann::json reply = ask(bare, {{"method", method}, {"params", {{"path", "x"}}}});
        CHECK(reply["ok"] == false);
    }
    // And git.ready answers anywhere: whether git is here is not about a project.
    CHECK(ask(bare, {{"method", "git.ready"}})["ok"] == true);
}

TEST(a_build_is_started_through_the_same_door_as_a_cook) {
    api::Host    nobody;
    api::Surface surface(nobody);
    const nlohmann::json reply = ask(surface, {{"method", "build.start"}, {"params", {{"directive", "a todo app"}}}});
    // No engine: refused the way cook.start is, not as an unknown method.
    CHECK(reply["ok"] == false);
    CHECK(reply["error"].get<std::string>().find("no method") == std::string::npos);
    const nlohmann::json empty = ask(surface, {{"method", "build.start"}});
    CHECK(empty["error"].get<std::string>().find("directive") != std::string::npos);
}
