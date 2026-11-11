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
#include "crucible/session/store.hpp"
#include "crucible/tools/app_self_test.hpp"
#include "crucible/tools/apps.hpp"
#include "crucible/tools/git.hpp"
#include "crucible/tools/page_storage.hpp"
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
    CHECK(tools::git::escaping_argument({"-c", "core.pager=cat", "-C", "/elsewhere", "log"}) == "-C");
    // After the command, -C is the command's own.
    CHECK(tools::git::escaping_argument({"log", "-C", "--stat"}).empty());
    CHECK(tools::git::escaping_argument({"commit", "-C", "HEAD"}).empty());
    // What a clone or an init makes stays in the project.
    TempDir dir;
    CHECK(tools::git::escaping_argument({"clone", "https://x/y.git", "/tmp/elsewhere"}, dir.path())
          == "/tmp/elsewhere");
    CHECK(tools::git::escaping_argument({"clone", "https://x/y.git", "../outside"}, dir.path()) == "../outside");
    CHECK(tools::git::escaping_argument({"clone", "https://x/y.git", "vendor/y"}, dir.path()).empty());
    CHECK(tools::git::escaping_argument({"init", "/tmp/elsewhere"}, dir.path()) == "/tmp/elsewhere");
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

    // A closing tag in capitals closes the script all the same, rather than
    // the page's text after it ending up inside the script.
    write(dir.path() / "shout.html", "<SCRIPT SRC=\"site/app.js\"></SCRIPT><p id=\"after\">x</p>");
    const tools::preview::Bundle shouted = tools::preview::bundle(dir.path(), "shout.html");
    const std::size_t script = shouted.html.find("console.log('hi')");
    const std::size_t closed = shouted.html.find("</SCRIPT>");
    CHECK(script != std::string::npos);
    CHECK(closed != std::string::npos && closed > script);
    CHECK(shouted.html.find("<p id=\"after\">", closed) != std::string::npos);
    CHECK(shouted.html.find("</script>") == std::string::npos);   // not closed twice
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

TEST(what_a_page_saves_in_the_preview_is_there_after_a_restart) {
    // The window holds it while it is open (ui/preview.js); these keep it in
    // Crucible's own folder for the project, for the next start.
    const TempDir        data;
    const ScopedDataHome scoped(data.path());
    const TempDir        project;
    const std::string    root = project.path().string();
    api::Host            nobody;
    api::Surface         surface(nobody);
    const auto kept = [&surface](const std::string& at, const std::string& page) {
        return ask(surface, {{"method", "preview.kept"}, {"params", {{"root", at}, {"page", page}}}});
    };
    const auto keep = [&surface](const std::string& at, const std::string& page, const std::string& kind,
                                 const nlohmann::json& value) {
        return ask(surface, {{"method", "preview.keep"},
                             {"params", {{"root", at}, {"page", page}, {"kind", kind}, {"value", value}}}});
    };

    // Nothing kept yet, and nothing said about it.
    nlohmann::json first = kept(root, "index.html");
    CHECK(first["ok"] == true);
    CHECK(first["result"].empty());

    // What window.crucible.save was given and what localStorage holds, each
    // read back as it was.
    CHECK(keep(root, "index.html", "data", {{"total", 9}, {"rows", {1, 2}}})["ok"] == true);
    CHECK(keep(root, "index.html", "local", {{"theme", "dark"}, {"rows", "[3]"}})["ok"] == true);
    const nlohmann::json back = kept(root, "index.html");
    CHECK_EQ(back["result"]["data"].dump(), std::string(R"({"rows":[1,2],"total":9})"));
    CHECK_EQ(back["result"]["local"].dump(), std::string(R"({"rows":"[3]","theme":"dark"})"));

    // In the project's own folder, a folder for the page holding the two
    // files a made app keeps.
    std::filesystem::path folder;
    for (const auto& entry : std::filesystem::directory_iterator(Project::at(project.path()).dir / "preview")) {
        folder = entry.path();
    }
    CHECK(folder.filename().string().rfind("index-html-", 0) == 0);
    CHECK(std::filesystem::is_regular_file(folder / "data.json"));
    CHECK(std::filesystem::is_regular_file(folder / "storage.json"));

    // Saved again, the newer is what is there; null is something saved too.
    CHECK(keep(root, "index.html", "data", nullptr)["ok"] == true);
    CHECK(kept(root, "index.html")["result"].contains("data"));
    CHECK(kept(root, "index.html")["result"]["data"].is_null());

    // Per page and per project: another page, the same page of another
    // project, start with nothing.
    CHECK(kept(root, "app/index.html")["result"].empty());
    CHECK(kept(root + "-other", "index.html")["result"].empty());

    // sessionStorage is not kept, a storage is names and values, and the
    // project and the page are both needed.
    CHECK(keep(root, "index.html", "session", {{"tab", "1"}})["ok"] == false);
    CHECK(keep(root, "index.html", "local", "not names")["ok"] == false);
    CHECK(keep(root, "", "data", 1)["ok"] == false);
    CHECK(kept("", "index.html")["ok"] == false);
    CHECK_EQ(kept(root, "index.html")["result"]["local"].dump(), std::string(R"({"rows":"[3]","theme":"dark"})"));

    // A file that was not written whole is as good as none.
    write(folder / "storage.json", "{\"rows\": \"[3");
    CHECK(!kept(root, "index.html")["result"].contains("local"));
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

TEST(a_made_apps_address_answers_with_its_page_and_nothing_else) {
    // crucible-app serves its page from an address of its own, so that a
    // reload has somewhere to go back to (tools/app_runner.cpp): a scheme of
    // its own on Linux and on a Mac, and an https address on Windows. Linux
    // hands over the path asked for, the other two the whole address. The
    // page is "/" or "/index.html", whatever the query, and nothing else
    // there is the program's.
    using tools::address_path;
    using tools::is_app_page;
    CHECK_EQ(std::string(address_path("https://crucible-app.example/")), std::string("/"));
    CHECK_EQ(std::string(address_path("https://crucible-app.example/index.html?from=reload#top")),
             std::string("/index.html"));
    CHECK_EQ(std::string(address_path("https://crucible-app.example/favicon.ico")), std::string("/favicon.ico"));
    CHECK_EQ(std::string(address_path("crucible-app://app/")), std::string("/"));
    // A query before any path is no path, even one that holds a slash.
    CHECK_EQ(std::string(address_path("https://crucible-app.example?next=/index.html")), std::string());
    CHECK_EQ(std::string(address_path("about:blank")), std::string());
    CHECK(is_app_page("/"));
    CHECK(is_app_page("/index.html"));
    CHECK(is_app_page(address_path("https://crucible-app.example/?x=1")));
    CHECK(!is_app_page(""));
    CHECK(!is_app_page("/favicon.ico"));
    CHECK(!is_app_page("/index.html/more"));
}

TEST(a_made_apps_self_test_passes_only_a_reload_that_kept_everything) {
    // crucible-app --self-test (tools/app_runner.cpp) is what CI runs on a
    // real desktop of each system, and this is its verdict. A check that
    // could not fail would prove nothing, so each way a reload goes wrong has
    // to fail it, and say how.
    using nlohmann::json;
    using tools::self_test_failures;
    const std::string address = "https://crucible-app.example/";
    const json first{{"address", address}, {"native", true}, {"local", nullptr}, {"session", nullptr},
                     {"saved", nullptr}};
    const json second{{"address", address}, {"native", true}, {"local", "kept"}, {"session", "kept"},
                      {"saved", {{"kept", "crucible-self-test"}}}};
    CHECK(self_test_failures({first, second}, 2, address).empty());

    const auto says = [](const std::vector<std::string>& failures, const std::string& what) {
        for (const std::string& failure : failures) {
            if (failure.find(what) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    // Handed over as a string, where a reload empties the window.
    json blank = second;
    blank["address"] = "about:blank";
    CHECK(says(self_test_failures({first, blank}, 2, address), "handed over as a string"));
    // Kept from the first load rather than asked of the program again.
    CHECK(says(self_test_failures({first, second}, 1, address), "served the page once"));
    // Served before what the page kept had arrived.
    for (const char* lost : {"local", "session"}) {
        json forgot = second;
        forgot[lost] = nullptr;
        CHECK(says(self_test_failures({first, forgot}, 2, address), "after the reload"));
    }
    json unsaved = second;
    unsaved["saved"] = nullptr;
    CHECK(says(self_test_failures({first, unsaved}, 2, address), "window.crucible.load() gave null"));
    // No bridge, a page whose check threw, and a second load that never came.
    json bare = second;
    bare["native"] = false;
    CHECK(says(self_test_failures({first, bare}, 2, address), "no window.crucible"));
    CHECK(says(self_test_failures({first, json{{"error", "TypeError"}}}, 2, address), "threw"));
    CHECK(says(self_test_failures({first}, 1, address), "loaded once"));
    // And a run that found its data folder holding something already.
    json stale = first;
    stale["local"] = "kept";
    CHECK(says(self_test_failures({stale, second}, 2, address), "kept already"));
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

TEST(a_made_app_replaces_its_own_copy_and_nothing_else) {
    // On Windows the app went into %LOCALAPPDATA%\Programs\<name>, cleared
    // first: an app named after a page called "Python" or "Crucible" deleted
    // the program installed there. A folder of that name that is not a made
    // app is left alone now; an earlier copy of the app is replaced.
    if (tools::apps::runner().empty()) {
        std::printf("      (no crucible-app built beside the tests; skipped)\n");
        return;
    }
    TempDir project;
    { std::ofstream out(project.path() / "index.html"); out << "<title>Budget</title><p>hi</p>"; }
    TempDir apps;
    std::filesystem::create_directories(apps.path() / "Budget");
    { std::ofstream out(apps.path() / "Budget" / "precious.txt"); out << "keep me"; }

    set_env("CRUCIBLE_APPS_DIR", apps.path().string());
    std::string error;
    const std::optional<tools::apps::Made> refused = tools::apps::package(project.path(), "index.html", "", error);
    CHECK(!refused.has_value());
    CHECK(error.find("not an app made here") != std::string::npos);
    CHECK(std::filesystem::exists(apps.path() / "Budget" / "precious.txt"));

    std::filesystem::remove_all(apps.path() / "Budget");
    error.clear();
    const std::optional<tools::apps::Made> first = tools::apps::package(project.path(), "index.html", "", error);
    const std::optional<tools::apps::Made> again = tools::apps::package(project.path(), "index.html", "", error);
    unset_env("CRUCIBLE_APPS_DIR");
    CHECK(first.has_value());
    CHECK(again.has_value());
}

TEST(deleting_or_renaming_a_link_touches_the_link_and_never_what_it_points_at) {
#if !defined(_WIN32)
    // A project with a link to a folder beside it, the way a shared docs
    // folder is often linked in, and a link to its own git folder.
    const TempDir dir;
    const std::filesystem::path root   = dir.path() / "proj";
    const std::filesystem::path shared = dir.path() / "shared";
    write(root / "src" / "a.py", "print(1)\n");
    write(shared / "guide.md", "keep me\n");
    write(root / ".git" / "HEAD", "ref: refs/heads/main\n");
    std::filesystem::create_directory_symlink(shared, root / "docs");
    std::filesystem::create_directory_symlink(root / ".git", root / "history");
    ProjectHost  host(root);
    api::Surface surface(host);

    // Renamed, the link moves and the folder it names stays put.
    const nlohmann::json renamed = ask(surface, {{"method", "project.rename"},
                                                 {"params", {{"path", "docs"}, {"to", "manual"}}}});
    CHECK(renamed["ok"] == true);
    CHECK(std::filesystem::is_symlink(root / "manual"));
    CHECK(std::filesystem::exists(shared / "guide.md"));

    // Deleted, the link goes and what it pointed at is all still there.
    CHECK(ask(surface, {{"method", "project.delete"}, {"params", {{"path", "manual"}}}})["ok"] == true);
    CHECK(!std::filesystem::exists(std::filesystem::symlink_status(root / "manual")));
    CHECK(std::filesystem::exists(shared / "guide.md"));
    CHECK(ask(surface, {{"method", "project.delete"}, {"params", {{"path", "history"}}}})["ok"] == true);
    CHECK(std::filesystem::exists(root / ".git" / "HEAD"));

    // git's own folder, in any case, and the project itself, are refused.
    for (const char* path : {".git", ".GIT", ".git/HEAD", "", ".", "src/.."}) {
        CHECK(ask(surface, {{"method", "project.delete"}, {"params", {{"path", path}}}})["ok"] == false);
    }
    CHECK(ask(surface, {{"method", "project.rename"},
                        {"params", {{"path", "src/a.py"}, {"to", ".Git/a.py"}}}})["ok"] == false);
    CHECK(std::filesystem::exists(root / "src" / "a.py"));

    // On a filesystem where case matters, two names that differ only in case
    // are two files, and one is not renamed over the other.
    write(root / "README.md", "one\n");
    write(root / "readme.md", "two\n");
    if (!std::filesystem::equivalent(root / "README.md", root / "readme.md")) {
        CHECK(ask(surface, {{"method", "project.rename"},
                            {"params", {{"path", "readme.md"}, {"to", "README.md"}}}})["ok"] == false);
    }
#endif
}
