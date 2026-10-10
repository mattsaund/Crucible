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
#include "crucible/tools/apps.hpp"
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

TEST(a_commit_of_named_paths_leaves_the_others_alone) {
    // Two agents side by side: the one that finished commits its file and
    // not the one the other is halfway through writing.
    if (!tools::git::available()) {
        std::printf("      (no git on PATH; skipped)\n");
        return;
    }
    TempDir dir;
    CHECK(tools::git::init(dir.path()).empty());
    write(dir.path() / "done.py", "print('done')\n");
    write(dir.path() / "halfway.py", "def unfinished(\n");
    std::string summary;
    CHECK(tools::git::commit(dir.path(), "Finish done.py", summary, {"done.py", "never-made.py"}).empty());
    CHECK(summary.rfind("committed ", 0) == 0);
    const tools::git::Status after = tools::git::status(dir.path());
    CHECK_EQ(after.changes.size(), std::size_t{1});
    CHECK_EQ(after.changes[0].path, std::string("halfway.py"));

    // Named paths that are not there at all commit nothing, and say so.
    std::string nothing;
    CHECK(tools::git::commit(dir.path(), "Nothing", nothing, {"never-made.py"}).empty());
    CHECK_EQ(nothing, std::string("nothing to commit"));
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

// ---------------------------------------------------------------------------
// A project's page, made a program
// ---------------------------------------------------------------------------

TEST(an_app_is_named_from_its_page_and_its_names_are_safe_everywhere) {
    TempDir dir;
    write(dir.path() / "index.html", "<html><head><title>Budget: Gross & Net</title></head></html>");
    CHECK_EQ(tools::apps::name_for(dir.path(), "index.html"), std::string("Budget Gross & Net"));
    CHECK_EQ(tools::apps::detail::slug("Budget: Gross & Net"), std::string("budget-gross-net"));
    CHECK_EQ(tools::apps::detail::file_name("  a/b\\\\c?.  "), std::string("abc"));
    const std::string plist = tools::apps::detail::info_plist("A & B", "dev.crucible.made.a-b", "a-b");
    CHECK(plist.find("<string>A &amp; B</string>") != std::string::npos);
    CHECK(plist.find("<key>CFBundleExecutable</key><string>a-b</string>") != std::string::npos);
    const std::string entry = tools::apps::detail::desktop_entry("Budget", "/home/x/.local/share/crucible-apps/budget/budget");
    CHECK(entry.find("Exec=\"/home/x/.local/share/crucible-apps/budget/budget\"") != std::string::npos);
}

TEST(a_page_is_packaged_with_the_runner_beside_it) {
    if (tools::apps::runner().empty()) {
        std::printf("      (no crucible-app built beside the tests; skipped)\n");
        return;
    }
    TempDir project;
    TempDir apps;
    write(project.path() / "index.html",
          "<html><head><title>Budget</title><link rel=\"stylesheet\" href=\"style.css\"></head>"
          "<body><script src=\"app.js\"></script></body></html>");
    write(project.path() / "style.css", "body { color: blue; }");
    write(project.path() / "app.js", "window.crucible.load();");
    set_env("CRUCIBLE_APPS_DIR", apps.path().string());
    std::string error;
    const std::optional<tools::apps::Made> made = tools::apps::package(project.path(), "index.html", "", error);
    unset_env("CRUCIBLE_APPS_DIR");
    CHECK(made.has_value());
    if (!made) {
        std::printf("      %s\n", error.c_str());
        return;
    }
    CHECK_EQ(made->name, std::string("Budget"));
    CHECK(std::filesystem::is_regular_file(made->launch));
    std::ifstream in(made->program / "app" / "index.html");
    const std::string page((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // One file, its stylesheet and script inside it.
    CHECK(page.find("color: blue") != std::string::npos);
    CHECK(page.find("window.crucible.load()") != std::string::npos);
    std::ifstream about(made->program / "app" / "app.json");
    const std::string json_text((std::istreambuf_iterator<char>(about)), std::istreambuf_iterator<char>());
    CHECK(json_text.find("\"name\": \"Budget\"") != std::string::npos);
}
