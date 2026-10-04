// SPDX-License-Identifier: MIT
//
// The one door into Crucible.
//
// Everything an interface can ask the program to do goes through here, as JSON
// in and JSON out. The window drives it, and so will the TypeScript interface
// through the webview bridge, and so will the Python orchestrator through a
// pipe. One surface rather than three, because three would drift: the day the
// window can do something the others cannot is the day "Crucible" stops
// meaning one thing.
//
// Strings rather than typed methods, and that is deliberate. A typed C++ API
// would need an adapter per caller -- one marshalling to the webview, one to a
// pipe -- and each adapter is a place for the three to disagree. A protocol
// has no adapters. It also means the obvious tools work: a request can be
// typed by hand, a session can be replayed from a log, and a new interface
// costs nothing to add.
//
// What this is not: it is not a network server. There is no port, no origin,
// no authentication, because nothing here is reachable from off the machine.
// The webview calls it in-process and Python is a child process on a pipe. If
// that ever changes, this comment is the thing to come back and argue with --
// a local-only program that quietly grew a listening socket would be a
// different promise than the one the README makes.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "crucible/config/config.hpp"
#include "crucible/engine/engine.hpp"
#include "crucible/engine/state.hpp"

namespace crucible::api {

/// What the surface needs from whoever owns the session.
///
/// Not a constructor full of references, because some of these are facts the
/// surface has no business owning -- which project is open is a property of
/// the session, and asking for it is better than keeping a second copy that
/// can go stale.
struct Deps {
    Engine*   engine = nullptr;
    AppState* state  = nullptr;

    /// The trusted project directory, or an empty path when none is open.
    /// Cook refuses to start without one, and that refusal belongs here
    /// rather than in each interface.
    std::function<std::filesystem::path()> project_root;

    /// The configuration in force. A copy each time rather than a pointer:
    /// the session owns it and applies changes on its own thread, and a
    /// reference handed out here would be read while it was being replaced.
    std::function<Config()> config;

    /// Apply a configuration: validate, save and hand it to the engine.
    /// Returns a reason it could not, or an empty string. The surface does
    /// not do this itself because what "apply" means -- re-resolving model
    /// paths, reconfiguring seats, writing the file -- is the session's.
    std::function<std::string(Config)> apply_config;

    /// Open a project directory. Returns a reason it could not, or empty.
    /// May answer empty while still not having opened it: a directory that
    /// has not been trusted yet puts the question up instead, and the
    /// interface learns the answer from the next snapshot.
    std::function<std::string(std::filesystem::path)> open_project;

    /// The folder waiting to be trusted, or an empty path.
    ///
    /// Opening an untrusted directory does not open it -- it asks. An
    /// interface that did not know that would report success and then show
    /// the old project, which is the kind of silence that looks like a bug in
    /// the picker.
    std::function<std::filesystem::path()> pending_trust;

    /// Answer it. True trusts the folder and opens it; false leaves both
    /// alone. Trust is granted once per directory and remembered.
    std::function<void(bool)> answer_trust;
};

/// Turn one JSON request into one JSON reply. Never throws.
///
/// Requests are `{"id": 1, "method": "submit", "params": {...}}`. `id` is
/// echoed back and is the caller's to choose; `params` may be omitted for the
/// methods that take none.
///
/// Replies are `{"id": 1, "ok": true, "result": ...}` or
/// `{"id": 1, "ok": false, "error": "what went wrong"}`. A reply always has
/// the same shape, including for malformed input, so a caller never has to
/// parse two formats to find out it made a mistake.
class Surface {
public:
    explicit Surface(Deps deps) : deps_(std::move(deps)) {}

    std::string handle(std::string_view request);

    /// Every method this build understands, for `methods` and for the tests
    /// that keep the documentation honest.
    static std::vector<std::string> methods();

private:
    Deps deps_;
};

/// The state an interface draws, as JSON.
///
/// Exposed on its own because it is the one piece worth having outside a
/// request: the window pushes it to the webview whenever the engine wakes the
/// loop, rather than the interface polling for it.
std::string snapshot_json(const Snapshot& snapshot);

}  // namespace crucible::api
