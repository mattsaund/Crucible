// SPDX-License-Identifier: MIT
//
// Knowing whether this copy of Crucible is the current one.
//
// Everything here is arithmetic on strings and JSON that GitHub sent. None of
// it touches the network: a test that needs api.github.com to be up is a test
// that goes red for reasons that have nothing to do with this code, and the
// only interesting part -- is 0.10.0 newer than 0.9.0 -- needs no network to
// get wrong.
#include "test_helpers.hpp"

#include "crucible/app/update.hpp"

using namespace crucible;

// ---------------------------------------------------------------------------
// Comparing versions
// ---------------------------------------------------------------------------

TEST(a_bigger_number_is_a_newer_version) {
    CHECK(update::compare("0.6.0", "0.5.0") > 0);
    CHECK(update::compare("0.5.0", "0.6.0") < 0);
    CHECK(update::compare("0.5.0", "0.5.0") == 0);
}

TEST(versions_are_compared_as_numbers_and_not_as_text) {
    // The whole reason this function exists. As strings, "0.10.0" sorts below
    // "0.9.0", and a release would stop being offered the moment the minor
    // number reached double digits.
    CHECK(update::compare("0.10.0", "0.9.0") > 0);
    CHECK(update::compare("1.0.0", "0.99.99") > 0);
    CHECK(update::compare("0.5.10", "0.5.9") > 0);
}

TEST(a_tag_may_wear_a_v_and_still_be_the_same_version) {
    // Git tags are v0.5.0 by convention; the binary calls itself 0.5.0. They
    // are the same release, and a check that disagrees would offer an update to
    // the version already installed, forever.
    CHECK(update::compare("v0.5.0", "0.5.0") == 0);
    CHECK(update::compare("V0.6.0", "0.5.0") > 0);
}

TEST(a_missing_part_counts_as_zero) {
    CHECK(update::compare("0.5", "0.5.0") == 0);
    CHECK(update::compare("1", "1.0.0") == 0);
    CHECK(update::compare("0.5.1", "0.5") > 0);
}

TEST(a_release_candidate_is_older_than_the_release) {
    CHECK(update::compare("0.6.0-rc1", "0.6.0") < 0);
    CHECK(update::compare("0.6.0", "0.6.0-rc1") > 0);
    // ...but still newer than the release before it.
    CHECK(update::compare("0.6.0-rc1", "0.5.0") > 0);
}

TEST(pre_releases_of_one_version_are_ordered_by_their_suffixes) {
    // They all compared equal, so a pre.3 was never offered the pre.4.
    CHECK(update::compare("1.0.0-pre.4", "1.0.0-pre.3") > 0);
    CHECK(update::compare("v1.0.0-pre.3", "1.0.0-pre.4") < 0);
    CHECK(update::compare("1.0.0-pre.10", "1.0.0-pre.9") > 0);   // as numbers
    CHECK(update::compare("1.0.0-pre.4", "v1.0.0-pre.4") == 0);
    // Words by their letters, as semantic versioning has it: a beta, then a
    // pre-release, then a release candidate, then the release.
    CHECK(update::compare("1.0.0-beta.2", "1.0.0-pre.1") < 0);
    CHECK(update::compare("1.0.0-rc.1", "1.0.0-pre.7") > 0);
    CHECK(update::compare("1.0.0-rc.1", "1.0.0") < 0);
    // A number before a word, and the shorter first when one runs out.
    CHECK(update::compare("1.0.0-1", "1.0.0-alpha") < 0);
    CHECK(update::compare("1.0.0-pre", "1.0.0-pre.1") < 0);
    // Build metadata says nothing about which is newer.
    CHECK(update::compare("1.0.0-pre.4+linux", "1.0.0-pre.4") == 0);
    // And the numbers still come first.
    CHECK(update::compare("1.0.0-pre.1", "0.9.9") > 0);
    CHECK(update::compare("1.0.1-beta.1", "1.0.0") > 0);
}

TEST(nonsense_compares_as_zero_rather_than_as_an_update) {
    // A tag that is not a version at all must not read as "newer", because the
    // consequence of that is a permanent notice telling people to update to
    // something that does not exist.
    CHECK(update::compare("", "0.5.0") < 0);
    CHECK(update::compare("nightly", "0.5.0") < 0);
    CHECK(update::compare("nightly", "nightly") == 0);
}

// ---------------------------------------------------------------------------
// Reading GitHub's answer
// ---------------------------------------------------------------------------

TEST(a_release_reply_gives_a_version_and_a_page) {
    const char* body = R"({
        "tag_name": "v0.6.0",
        "name": "Crucible 0.6.0",
        "html_url": "https://github.com/mattsaund/Crucible/releases/tag/v0.6.0"
    })";
    std::string version;
    std::string page;
    CHECK(update::parse_latest(body, version, page));
    CHECK_EQ(version, "v0.6.0");
    CHECK_EQ(page, "https://github.com/mattsaund/Crucible/releases/tag/v0.6.0");
}

TEST(a_reply_with_no_page_still_points_somewhere_real) {
    std::string version;
    std::string page;
    CHECK(update::parse_latest(R"({"tag_name":"v0.7.0"})", version, page));
    CHECK_EQ(version, "v0.7.0");
    CHECK(page.find("mattsaund/Crucible") != std::string::npos);
}

TEST(a_rate_limit_note_is_not_a_release) {
    // What api.github.com actually sends when it has had enough of you. It is
    // a perfectly good JSON object, and treating it as an answer would blank
    // the cached version every hour.
    const char* body = R"({"message":"API rate limit exceeded","documentation_url":"https://docs.github.com"})";
    std::string version;
    std::string page;
    CHECK(!update::parse_latest(body, version, page));
}

TEST(a_reply_that_is_not_json_is_not_a_release) {
    std::string version;
    std::string page;
    CHECK(!update::parse_latest("<html>504 Gateway Timeout</html>", version, page));
    CHECK(!update::parse_latest("", version, page));
    CHECK(!update::parse_latest("[]", version, page));
    CHECK(!update::parse_latest(R"({"tag_name":""})", version, page));
}

// ---------------------------------------------------------------------------
// The Pre-releases box
// ---------------------------------------------------------------------------

namespace {

std::string release_json(const std::string& tag, bool draft, bool prerelease) {
    const std::string base = "https://github.com/mattsaund/Crucible/releases/";
    return R"({"tag_name": ")" + tag + R"(", "draft": )" + (draft ? "true" : "false")
         + R"(, "prerelease": )" + (prerelease ? "true" : "false")
         + R"(, "html_url": ")" + base + "tag/" + tag + R"(", "assets": [{"name": "Crucible-x86_64.AppImage",)"
         + R"( "size": )" + std::to_string(tag.size()) + R"(, "browser_download_url": ")" + base
         + "download/" + tag + R"(/Crucible-x86_64.AppImage"}]})";
}

}  // namespace

TEST(the_box_off_asks_for_the_newest_release_and_on_for_the_list) {
    // GitHub's latest is never a pre-release, so the box needs the list.
    CHECK(update::releases_url(false).find("/releases/latest") != std::string::npos);
    CHECK(update::releases_url().find("/releases/latest") != std::string::npos);
    const std::string list = update::releases_url(true);
    CHECK(list.find("/releases/latest") == std::string::npos);
    CHECK(list.find("repos/mattsaund/Crucible/releases?") != std::string::npos);
}

TEST(the_box_decides_whether_a_pre_release_is_offered_and_installed) {
    // What /releases/latest answers: the release alone. The box off.
    std::string version;
    std::string page;
    std::string url;
    std::uint64_t bytes = 0;
    const std::string latest = release_json("v0.9.9", false, false);
    CHECK(update::parse_latest(latest, version, page));
    CHECK_EQ(version, std::string("v0.9.9"));
    CHECK(update::find_asset(latest, "Crucible-x86_64.AppImage", url, bytes));
    CHECK(url.find("/download/v0.9.9/") != std::string::npos);

    // What /releases answers, newest made first: a draft (shown only to the
    // repository's own people), a pre-release and the release. The box on:
    // the pre-release, which Update now downloads too, and never the draft.
    const std::string list = "[" + release_json("v1.0.0-pre.6", true, true) + ","
                           + release_json("v1.0.0-pre.5", false, true) + ","
                           + release_json("v0.9.9", false, false) + "]";
    CHECK(update::parse_latest(list, version, page));
    CHECK_EQ(version, std::string("v1.0.0-pre.5"));
    CHECK_EQ(page, std::string("https://github.com/mattsaund/Crucible/releases/tag/v1.0.0-pre.5"));
    CHECK(update::find_asset(list, "Crucible-x86_64.AppImage", url, bytes));
    CHECK(url.find("/download/v1.0.0-pre.5/") != std::string::npos);
    CHECK_EQ(bytes, std::uint64_t{12});

    // The newest by version, wherever it is listed: a release made after its
    // pre-releases is newer than all of them, and an older line's fix made
    // last is not newer than the pre-release before it.
    const std::string released = "[" + release_json("v0.9.10", false, false) + ","
                               + release_json("v1.0.0", false, false) + ","
                               + release_json("v1.0.0-pre.5", false, true) + "]";
    CHECK(update::parse_latest(released, version, page));
    CHECK_EQ(version, std::string("v1.0.0"));
    const std::string fixed = "[" + release_json("v0.9.10", false, false) + ","
                            + release_json("v1.0.0-pre.5", false, true) + "]";
    CHECK(update::parse_latest(fixed, version, page));
    CHECK_EQ(version, std::string("v1.0.0-pre.5"));

    // Nothing published is nothing to offer.
    const std::string drafts = "[" + release_json("v1.0.0-pre.6", true, true) + "]";
    CHECK(!update::parse_latest(drafts, version, page));
    CHECK(!update::find_asset(drafts, "Crucible-x86_64.AppImage", url, bytes));
    CHECK(!update::parse_latest("[]", version, page));
    CHECK(!update::parse_latest(R"([{"message":"API rate limit exceeded"}])", version, page));
}

TEST(an_answer_found_with_the_box_the_other_way_is_no_answer) {
    update::State found;
    found.latest      = "v0.9.9";
    found.page        = "https://github.com/mattsaund/Crucible/releases/tag/v0.9.9";
    found.checked_at  = 1'700'000'000;
    found.prereleases = false;

    // The same way: kept, and not due again for a day.
    const update::State same = update::for_channel(found, false);
    CHECK_EQ(same.latest, found.latest);
    CHECK(!update::due(same, found.checked_at + 60));

    // Ticked since: nothing known, so nothing offered, and asked at once.
    const update::State other = update::for_channel(found, true);
    CHECK(other.latest.empty());
    CHECK(other.prereleases);
    CHECK(!update::newer_than_this(other));
    CHECK(update::due(other, found.checked_at + 60));
}

TEST(the_cache_remembers_which_way_the_box_was) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());
    update::State written;
    written.latest      = "v1.0.0-pre.5";
    written.checked_at  = 1'700'000'123;
    written.prereleases = true;
    CHECK(update::write_cache(written));
    update::State read;
    CHECK(update::read_cache(read));
    CHECK_EQ(read.latest, std::string("v1.0.0-pre.5"));
    CHECK(read.prereleases);

    // A file from before the box: what it found was a release.
    std::ofstream(update::cache_file()) << R"({"latest":"v0.9.9","page":"","checked_at":5})";
    update::State old;
    CHECK(update::read_cache(old));
    CHECK(!old.prereleases);
    CHECK(update::for_channel(old, false).latest == "v0.9.9");
}

// ---------------------------------------------------------------------------
// When to ask again
// ---------------------------------------------------------------------------

TEST(a_check_is_due_once_a_day_and_not_before) {
    update::State state;
    state.checked_at = 1000;
    CHECK(!update::due(state, 1000 + update::kInterval - 1));
    CHECK(update::due(state, 1000 + update::kInterval));
    CHECK(update::due(state, 1000 + update::kInterval * 3));
}

TEST(check_now_asks_even_when_the_last_check_was_an_hour_ago) {
    // It asked only once a day had passed, so a release out since the
    // morning's check read "Up to date" however often Check now was pressed.
    update::State cached;
    cached.latest     = "v0.9.9";
    cached.checked_at = 1'700'000'000;
    const std::int64_t hour_later = cached.checked_at + 60 * 60;
    CHECK(!update::will_ask(cached, false, false, hour_later));   // the daily check waits
    CHECK(update::will_ask(cached, false, true, hour_later));     // Check now does not
    // Nor does the box, once it is the other way.
    CHECK(update::will_ask(cached, true, false, hour_later));
    CHECK(update::will_ask(cached, false, false, cached.checked_at + update::kInterval));
}

TEST(a_fresh_install_checks_straight_away) {
    const update::State state;  // never checked
    CHECK(update::due(state, 1'700'000'000));
}

TEST(a_clock_that_went_backwards_does_not_park_the_check_in_the_future) {
    // A restored backup, or a machine that was wrong about the year until NTP
    // corrected it. Without this, checked_at sits ahead of now and the
    // subtraction never reaches a day.
    update::State state;
    state.checked_at = 2'000'000'000;
    CHECK(update::due(state, 1'700'000'000));
}

TEST(an_update_is_only_news_when_it_is_newer_than_this_build) {
    update::State state;
    state.latest = std::string(update::current());
    CHECK(!update::newer_than_this(state));

    state.latest = "99.0.0";
    CHECK(update::newer_than_this(state));

    state.latest = "0.0.1";
    CHECK(!update::newer_than_this(state));

    state.latest.clear();
    CHECK(!update::newer_than_this(state));
}

TEST(the_update_command_is_one_line_somebody_can_paste) {
    const std::string command(update::update_command());
    CHECK(!command.empty());
    CHECK(command.find("install") != std::string::npos);
    CHECK(command.find("mattsaund/Crucible") != std::string::npos);
    CHECK(command.find("/main/install.") != std::string::npos);
}

TEST(a_copy_built_from_source_is_told_to_build_the_release_that_was_found) {
    // The one-line install builds main, and main can be a pre-release ahead
    // of the newest release: the command names the tag the check found, and
    // runs that tag's own installer.
    const std::string command = update::update_command("v1.0.0-pre.4");
    CHECK(command.find("/mattsaund/Crucible/v1.0.0-pre.4/install.") != std::string::npos);
    CHECK(command.find("/main/") == std::string::npos);
#if defined(_WIN32)
    CHECK(command.find("[scriptblock]::Create((irm ") != std::string::npos);
    CHECK(command.find(" -Branch v1.0.0-pre.4") != std::string::npos);
#else
    CHECK(command.find("install.sh | bash -s -- --branch v1.0.0-pre.4") != std::string::npos);
#endif
    // A tag is pasted into a shell, so only one made of a tag's characters
    // is used, and none an option could hide in. Anything else builds main.
    for (const char* odd : {"v1.0.0; rm -rf ~", "v1 0", "--branch=x", "$(id)", "v1.0.0\nwhoami", ""}) {
        const std::string fallback = update::update_command(odd);
        CHECK(fallback.find("/main/install.") != std::string::npos);
        CHECK(fallback.find("rm -rf") == std::string::npos);
        CHECK(fallback.find("Branch") == std::string::npos && fallback.find("branch") == std::string::npos);
    }
}

TEST(this_build_says_which_version_it_is) {
    const std::string version(update::current());
    CHECK(!version.empty());
    CHECK(version != "0.0.0");
    // And it is a version the comparison can read, not a word.
    CHECK(update::compare(version, "0.0.1") > 0);
}

// ---------------------------------------------------------------------------
// Updating in place
// ---------------------------------------------------------------------------

TEST(each_install_is_updated_by_the_file_its_release_carries) {
    CHECK_EQ(update::asset_for(update::Install::Setup), std::string("Crucible-Setup.exe"));
    CHECK_EQ(update::asset_for(update::Install::AppBundle), std::string("Crucible-macOS.dmg"));
    CHECK_EQ(update::asset_for(update::Install::AppImage), std::string("Crucible-x86_64.AppImage"));
    CHECK(update::asset_for(update::Install::Source).empty());
}

TEST(a_release_says_where_its_installer_downloads_from) {
    const std::string body = R"({"tag_name": "v0.9.0", "assets": [
        {"name": "Crucible-macOS.dmg", "size": 5000000,
         "browser_download_url": "https://github.com/x/releases/download/v0.9.0/Crucible-macOS.dmg"},
        {"name": "Crucible-Setup.exe", "size": 4600000,
         "browser_download_url": "https://github.com/x/releases/download/v0.9.0/Crucible-Setup.exe"}]})";
    std::string   url;
    std::uint64_t bytes = 0;
    CHECK(update::find_asset(body, "Crucible-Setup.exe", url, bytes));
    CHECK(url.find("Crucible-Setup.exe") != std::string::npos);
    CHECK_EQ(bytes, std::uint64_t{4600000});
    CHECK(!update::find_asset(body, "Crucible-x86_64.AppImage", url, bytes));
    CHECK(!update::find_asset("rate limited", "Crucible-Setup.exe", url, bytes));
}

TEST(the_swap_waits_for_crucible_to_close_and_starts_the_new_one) {
    // Windows: the setup program over the same folder, then the program.
    const std::string windows = update::swap_script(update::Install::Setup, 4242,
                                                    "C:/data/update/Crucible-Setup.exe", "C:/Programs/Crucible");
    CHECK(windows.find("PID eq 4242") != std::string::npos);
    CHECK(windows.find("/SILENT") != std::string::npos);
    CHECK(windows.find("/DIR=\"C:/Programs/Crucible\"") != std::string::npos);
    CHECK(windows.find("crucible.exe") != std::string::npos);

    // A Mac: the old app goes only once the new one is copied whole.
    const std::string mac = update::swap_script(update::Install::AppBundle, 4242,
                                                "/data/update/Crucible-macOS.dmg", "/Applications/Crucible.app");
    CHECK(mac.find("kill -0 4242") != std::string::npos);
    CHECK(mac.find("ditto \"$app\" '/Applications/Crucible.app.new' && rm -rf '/Applications/Crucible.app'")
          != std::string::npos);
    CHECK(mac.find("open '/Applications/Crucible.app'") != std::string::npos);

    // An AppImage, in a folder with a quote in its name.
    const std::string image = update::swap_script(update::Install::AppImage, 7, "/tmp/new.AppImage",
                                                  "/home/me/Matt's apps/Crucible.AppImage");
    CHECK(image.find("mv -f '/tmp/new.AppImage' '/home/me/Matt'\\''s apps/Crucible.AppImage'")
          != std::string::npos);
    CHECK(image.find("exec '/home/me/Matt'\\''s apps/Crucible.AppImage'") != std::string::npos);
}
