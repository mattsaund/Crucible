// SPDX-License-Identifier: MIT
//
// The one door into Crucible.
//
// Everything an interface can ask the program to do goes through here, as JSON
// in and JSON out. The page in the window drives it through the webview
// bridge, and the Python orchestrator will drive it through a pipe. One
// surface rather than two, because two would drift: the day the window can do
// something the other cannot is the day "Crucible" stops meaning one thing.
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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/app/update.hpp"
#include "crucible/config/config.hpp"
#include "crucible/engine/engine.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/lab/pyenv.hpp"
#include "crucible/lab/trainer.hpp"
#include "crucible/runtime/builder.hpp"

namespace crucible::api {

/// Whoever owns the session, as the surface sees them.
///
/// The surface is a vocabulary; this is what the words are about. Which
/// project is open, what the configuration is, what it means to change it --
/// those are facts and decisions that belong to the session, and the surface
/// asks rather than keeping a second copy that can go stale.
///
/// Every member has an answer that means "there is none here", so that a
/// surface can stand on nothing at all. That is not only for the tests: the
/// two installs are worth being able to drive before an engine exists, since
/// the moment you most need a runtime is the one where nothing can run yet.
class Host {
public:
    virtual ~Host() = default;

    // --- the long-lived things it owns ------------------------------------
    //
    // Pointers, null when there is none. Each is one thing the session owns
    // for its whole life, and the surface only ever borrows it for a call.

    virtual Engine*                engine() { return nullptr; }
    virtual AppState*              state() { return nullptr; }
    virtual lab::Trainer*          trainer() { return nullptr; }
    virtual RuntimeBuilder*        runtime_builder() { return nullptr; }
    virtual lab::pyenv::Installer* trainer_installer() { return nullptr; }

    // --- the configuration -------------------------------------------------

    /// The configuration in force. A copy each time rather than a reference:
    /// the session replaces it when it changes, and a reference handed out
    /// here would be read while that happened.
    virtual Config config() const { return {}; }

    /// Apply a configuration: validate, save and hand it to the engine.
    /// Returns a reason it could not, or an empty string. The surface does
    /// not do this itself because what "apply" means -- re-resolving model
    /// paths, reconfiguring seats, writing the file -- is the session's.
    virtual std::string apply_config(Config) {
        return "the configuration cannot be changed from here";
    }

    // --- the project -------------------------------------------------------

    /// The trusted project directory, or an empty path when none is open.
    /// Cook refuses to start without one, and that refusal belongs here
    /// rather than in each interface.
    virtual std::filesystem::path project_root() const { return {}; }

    /// Open a project directory. Returns a reason it could not, or empty.
    /// May answer empty while still not having opened it: a directory that
    /// has not been trusted yet puts the question up instead, and the
    /// interface learns the answer from the next snapshot.
    virtual std::string open_project(const std::filesystem::path&) {
        return "no project can be opened from here";
    }

    /// The folder waiting to be trusted, or an empty path.
    ///
    /// Opening an untrusted directory does not open it -- it asks. An
    /// interface that did not know that would report success and then show
    /// the old project, which is the kind of silence that looks like a bug in
    /// the picker.
    virtual std::filesystem::path pending_trust() const { return {}; }

    /// Answer it. True trusts the folder and opens it; false leaves both
    /// alone. Trust is granted once per directory and remembered.
    virtual void answer_trust(bool) {}

    // --- the transcript ----------------------------------------------------

    /// Load a stored conversation back into the transcript. Returns a reason
    /// it could not, or empty. The engine has to be told as well as the
    /// screen -- an expert that cannot see what is already on it would answer
    /// the next question with no idea what came before.
    virtual std::string open_session(const std::string&) {
        return "conversations cannot be reopened from here";
    }

    /// Ask a turn again, or take it off the transcript.
    ///
    /// Both rewrite what the expert can see as well as what is on screen, and
    /// both refuse while the engine is busy. False when this host cannot.
    virtual bool retry_turn(std::size_t) { return false; }
    virtual bool delete_turn(std::size_t) { return false; }

    // --- trying a fine-tune before keeping it ------------------------------
    //
    // `test_begin` seats the trained file so it can be asked something and
    // returns a reason it could not, or empty; `test_end` takes the seat
    // away; `keep` finishes the recipe and puts the model on the roster.
    // They are the session's because seating a model means reconfiguring
    // the engine, and what a configuration means is the session's to say.

    virtual std::string test_begin(const std::string&) {
        return "this build cannot seat a fine-tune";
    }
    virtual void        test_end() {}
    virtual std::string keep(const std::string&) {
        return "this build cannot keep a fine-tune";
    }

    // --- saying things and being told --------------------------------------

    /// A line for the interface to show: "Physics has joined the experts".
    virtual void say(std::string) {}

    /// What is known about newer versions of Crucible.
    virtual update::State update() const { return {}; }

    /// Poke whatever is drawing, from any thread.
    ///
    /// The installers run on threads of their own and report progress as
    /// they go. Without this the page would show the first line of a
    /// ten-minute compile until something else happened to redraw it.
    virtual void wake() {}
};

/// A request, opened and looked up but not yet answered.
///
/// It exists because of one question the window has to ask before it answers
/// anything: can this be done somewhere other than the thread that draws? See
/// Surface::prepare.
struct Prepared {
    /// True when the answer may be worked out on any thread.
    bool background = false;

    /// Everything else, which is nobody's business but the surface's.
    struct Impl;
    std::shared_ptr<const Impl> impl;
};

/// Turns one JSON request into one JSON reply. Never throws.
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
    explicit Surface(Host& host) : host_(host) {}

    /// Answer `request`. The whole thing, on the calling thread.
    std::string handle(std::string_view request);

    /// The same in two steps, for a caller with a thread it must not block.
    ///
    /// A method is one of two kinds. Most *act*: they touch the session, and
    /// so they run on the session's thread, and they are quick. A few only
    /// *look something up* -- which models are on disk, what a provider
    /// offers, whether Python can see the cards -- and those can take seconds
    /// and touch nothing. `prepare` reads the request on the session's thread
    /// and takes a copy of what a lookup is allowed to know; `run` then
    /// answers it, on any thread at all when `background` is set.
    ///
    /// The two kinds are different function types underneath, so a lookup
    /// cannot reach the session by accident: it is never handed one.
    Prepared    prepare(std::string_view request);
    std::string run(const Prepared& prepared);

    /// The state an interface draws, as JSON text: the engine's snapshot and
    /// what the session knows that the engine does not.
    ///
    /// Exposed on its own because it is the one thing worth having outside a
    /// request -- the window pushes it to the page whenever the engine wakes
    /// it, rather than the page polling.
    std::string snapshot();

    /// Every method this build understands, for `methods` and for the tests
    /// that keep the documentation honest.
    static std::vector<std::string> methods();

private:
    Host& host_;
};

/// The engine's half of the snapshot, as JSON text.
///
/// What `Surface::snapshot` starts from, and separately useful because it
/// needs no session: it is a pure function of the state it is given.
std::string snapshot_json(const Snapshot& snapshot);

}  // namespace crucible::api
