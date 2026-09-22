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
// When to ask again
// ---------------------------------------------------------------------------

TEST(a_check_is_due_once_a_day_and_not_before) {
    update::State state;
    state.checked_at = 1000;
    CHECK(!update::due(state, 1000 + update::kInterval - 1));
    CHECK(update::due(state, 1000 + update::kInterval));
    CHECK(update::due(state, 1000 + update::kInterval * 3));
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
}

TEST(this_build_says_which_version_it_is) {
    const std::string version(update::current());
    CHECK(!version.empty());
    CHECK(version != "0.0.0");
    // And it is a version the comparison can read, not a word.
    CHECK(update::compare(version, "0.0.1") > 0);
}
