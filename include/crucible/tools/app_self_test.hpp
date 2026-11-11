// SPDX-License-Identifier: MIT
//
// What a made app's --self-test checks (tools/app_runner.cpp), as a function
// the unit tests can hand loads to. The run itself needs a window, which no
// unit test can open, so CI runs it on a real desktop of each system; this
// is what decides whether what it saw passed, and the tests are what show
// that a reload gone wrong cannot pass.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace crucible::tools {

/// What --self-test puts in before the page, as the bridge is put in: on each
/// load of the page it says what that load found. Asked to reload, it does
/// what a page that saves and reloads in one click does, all at once and
/// waiting for nothing: something kept in localStorage and sessionStorage, a
/// window.crucible.save, and location.reload().
inline constexpr const char* kSelfTestProbe = R"JS(
(function () {
  var key = 'crucible-self-test';
  function read(name) {
    try { return window[name].getItem(key); } catch (e) { return 'threw ' + e.name; }
  }
  window.addEventListener('load', function () {
    var bridge = window.crucible || {};
    Promise.resolve(typeof bridge.load === 'function' ? bridge.load() : 'no window.crucible')
      .then(function (saved) {
        return window.__crucibleSelfTest({
          address: String(location.href), title: document.title, native: bridge.native === true,
          local: read('localStorage'), session: read('sessionStorage'), saved: saved === undefined ? null : saved
        });
      })
      .then(function (next) {
        if (next !== 'reload') return;
        localStorage.setItem(key, 'kept');
        sessionStorage.setItem(key, 'kept');
        bridge.save({ kept: key });
        location.reload();
      })
      .catch(function (e) { window.__crucibleSelfTest({ error: String(e) }); });
  });
})();
)JS";

/// Why the two loads a self-test saw are not the reload a made app depends
/// on, or nothing when they are. `loads` is what kSelfTestProbe said of each,
/// `served` how many times the program answered a request for its page, and
/// `address` the program's own address.
///
/// Both loads have to be at that address, which a page handed over as a
/// string is not, with window.crucible there. The program has to have served
/// the page once for each, so the second was asked of it again rather than
/// kept from the first. The first has to have found nothing kept, since the
/// run's data folder is new, and the second all of what the first kept:
/// localStorage, sessionStorage and window.crucible.save. A page served
/// before what it kept arrived comes back without it.
inline std::vector<std::string> self_test_failures(const std::vector<nlohmann::json>& loads, int served,
                                                   std::string_view address) {
    using json = nlohmann::json;
    const auto times = [](std::size_t count) {
        return count == 1 ? std::string("once") : std::to_string(count) + " times";
    };
    std::vector<std::string> failures;
    if (loads.size() != 2) {
        failures.push_back("the page loaded " + times(loads.size()) + ", not twice");
        return failures;
    }
    for (std::size_t i = 0; i < loads.size(); ++i) {
        const std::string number = std::to_string(i + 1);
        const json&       load   = loads[i];
        if (!load.is_object()) {
            failures.push_back("load " + number + " said nothing");
            continue;
        }
        if (load.contains("error")) {
            failures.push_back("load " + number + " threw: " + load["error"].dump());
            continue;
        }
        const std::string at = load.value("address", std::string());
        if (address.empty() || at.rfind(address, 0) != 0) {
            failures.push_back("load " + number + " was at " + (at.empty() ? std::string("no address") : at)
                               + " rather than " + std::string(address)
                               + ": the page was handed over as a string, which a reload empties");
        }
        if (!load.value("native", false)) {
            failures.push_back("load " + number + " had no window.crucible");
        }
    }
    if (served != 2) {
        failures.push_back("the program served the page " + times(served < 0 ? 0U : static_cast<std::size_t>(served))
                           + ", rather than once for each load");
    }
    const json& first  = loads[0];
    const json& second = loads[1];
    const auto  found  = [](const json& load, const char* name) {
        return load.is_object() && load.contains(name) ? load[name] : json();
    };
    if (!found(first, "local").is_null() || !found(first, "session").is_null() || !found(first, "saved").is_null()) {
        failures.push_back("the first load found something kept already, in a data folder made for the run");
    }
    if (found(second, "local") != json("kept")) {
        failures.push_back("after the reload localStorage held " + found(second, "local").dump());
    }
    if (found(second, "session") != json("kept")) {
        failures.push_back("after the reload sessionStorage held " + found(second, "session").dump());
    }
    if (found(second, "saved") != json{{"kept", "crucible-self-test"}}) {
        failures.push_back("after the reload window.crucible.load() gave " + found(second, "saved").dump());
    }
    return failures;
}

}  // namespace crucible::tools
