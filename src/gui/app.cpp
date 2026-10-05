// SPDX-License-Identifier: MIT
//
// The session: everything Crucible is, apart from what draws it.
//
// Config, engine, trust, the session store, which project is open and what is
// allowed to happen to it. The interface lives in window.cpp and reaches all
// of it through api::Surface rather than through these members, which is what
// lets a second interface -- the Python orchestrator, a test harness -- exist
// without this file knowing.
#include "app.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>


#include "crucible/config/paths.hpp"
#include "crucible/runtime/backend.hpp"
#include "crucible/util/http.hpp"

namespace crucible::gui {

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

App::App(Config config, std::vector<std::string> warnings, bool skip_trust)
    : config_(std::move(config)),
      trust_(paths::trust_file()),
      skip_trust_(skip_trust) {
    // What the config file's reader had to say about it: a number clamped, a
    // seat pointed at a provider that is not there. Shown rather than logged,
    // because the person who can fix it is the one looking at the window.
    for (std::string& warning : warnings) {
        state_.add_notice(std::move(warning));
    }

    state_.configure_seats(config_);

    // Poked whenever the engine's state moves. Nothing is listening until
    // run() opens a window and says what a poke should do.
    engine_ = std::make_unique<Engine>(config_, state_, [this] { wake(); });
    // No project, so no root and no history folder. The root is the permission
    // an expert acts under, and there is nothing to act on yet: a WRITE in this
    // state is refused, which is the right answer to "before you opened one".
    engine_->set_project({}, {});
}


std::filesystem::path App::project_root() const {
    return store_ ? store_->project().root : std::filesystem::path{};
}

std::filesystem::path App::project_dir() const {
    return store_ ? store_->project().dir : std::filesystem::path{};
}

std::filesystem::path App::pending_trust() const {
    return pending_trust_ ? *pending_trust_ : std::filesystem::path{};
}

App::~App() {
    // Whatever is waiting on the network stops waiting. An errand in the pool
    // may be mid-request, and the pool's destructor joins it: without this,
    // closing the window could take as long as somebody's server takes to
    // time out.
    util::http::shut_down();
    if (engine_) {
        engine_->stop();
    }
}

void App::say(std::string message) {
    // Into the engine's state rather than a list of this object's own, so it
    // reaches the page by the one route everything else does. The list this
    // replaced was written to faithfully and read by nothing.
    state_.add_notice(std::move(message));
    wake();
}

void App::wake() {
    const std::lock_guard<std::mutex> lock(wake_mutex_);
    if (wake_) {
        wake_();
    }
}

void App::set_wake(std::function<void()> wake) {
    const std::lock_guard<std::mutex> lock(wake_mutex_);
    wake_ = std::move(wake);
}

std::string App::apply_config(Config edited) {
    // The same path a change made anywhere else takes, so two callers cannot
    // apply one differently.
    update_config([&edited](Config& live) { live = std::move(edited); });
    return {};
}

void App::housekeeping() {
    persist_session();
    absorb_written_examples();
    absorb_finished_run();
    absorb_finished_build();
    collect_update_check();
}

void App::absorb_finished_run() {
    const lab::RunProgress run = trainer_.progress();
    if (run.phase != lab::RunProgress::Phase::Done || run.recipe_id.empty()) {
        if (run.running()) {
            run_absorbed_.clear();   // a new run, so its ending is news again
        }
        return;
    }
    if (run.recipe_id == run_absorbed_) {
        return;
    }
    run_absorbed_ = run.recipe_id;

    for (lab::Recipe recipe : lab::saved_recipes()) {
        if (recipe.id != run.recipe_id) {
            continue;
        }
        recipe.trained_path = run.produced.string();
        recipe.stage        = lab::Stage::Testing;
        std::string error;
        if (lab::save(recipe, error)) {
            say(recipe.name + " finished training -- it is ready to test");
        } else {
            say(recipe.name + " finished training, but the recipe could not be "
                "updated: " + error);
        }
    }
}

void App::absorb_finished_build() {
    const BuildProgress build = runtime_builder_.progress();
    if (build.phase != BuildProgress::Phase::Done || !build.error.empty()) {
        // Reset on anything else, so the next build reports itself too.
        build_absorbed_ = false;
        return;
    }
    if (build_absorbed_) {
        return;
    }
    build_absorbed_ = true;

    // A model picks its devices when it loads and keeps them. Dropping what
    // is loaded is what puts the next prompt on the new hardware.
    engine_->reload_models();
    say(std::string(backend_info(build.kind).name)
        + " is ready -- models will use it from the next prompt");
}

void App::update_config(const std::function<void(Config&)>& change) {
    change(config_);
    config_.resolve_models();
    state_.configure_seats(config_);
    if (!save_config(config_)) {
        say("could not write " + paths::config_file().string());
    }
    engine_->apply_config(config_);
}

void App::persist_session() {
    // Nothing to write a history into. A conversation cannot have happened
    // without a project -- the composer is closed until one is open -- but this
    // is called from housekeeping and from open_project, so it says so rather
    // than trusting that.
    if (!store_) {
        return;
    }
    const Snapshot snapshot = state_.snapshot();
    std::size_t finished = 0;
    for (const Turn& turn : snapshot.turns) {
        finished += turn.streaming ? 0 : 1;
    }
    if (finished == persisted_turns_) {
        return;
    }
    std::string error;
    if (store_->save(snapshot.turns, snapshot.session_usage, error)) {
        persisted_turns_ = finished;
    }
}

void App::absorb_written_examples() {
    const std::vector<std::pair<ExpertId, std::vector<std::string>>> written =
        engine_->take_written_examples();
    if (written.empty()) {
        return;
    }
    update_config([&written](Config& config) {
        for (const auto& [id, examples] : written) {
            if (const std::optional<std::size_t> seat = config.roster.find(id)) {
                Expert expert = config.roster.at(*seat);
                expert.examples = examples;
                config.roster.update(id, expert);
            }
        }
    });
    for (const auto& [id, examples] : written) {
        say(expert_label(config_.roster, id) + ": the delegator wrote "
            + std::to_string(examples.size()) + " example questions to route on");
    }
}

std::string App::open_project(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return root.string() + " is not a directory";
    }
    if (engine_->cooking() || engine_->is_busy()) {
        // A cook is about the directory it started in, and its journal is keyed
        // to it. Moving the ground under it would produce a record of work done
        // somewhere it was not. The same goes for a turn still being written.
        return "finish or stop what is running before opening another project";
    }

    // Asked once per directory, and remembered. `--no-trust` is the way past
    // it for a scripted run, where there is nobody to answer a modal.
    if (!skip_trust_ && !trust_.is_trusted(root)) {
        pending_trust_ = root;
        return {};
    }

    const Project project = Project::at(root);
    persist_session();

    store_ = std::make_unique<SessionStore>(project);
    engine_->set_project(project.root, project.dir);
    engine_->reset_history();
    state_.clear_turns();
    state_.clear_notices();
    state_.set_cook(nullptr);
    state_.set_project_usage(store_->project_usage());
    persisted_turns_ = 0;

    remember_project(project.root);
    // The name, not the path. The path is in the top bar; a notice is a line
    // in the transcript and a three-line path wrapping across it is the
    // loudest thing on an empty screen.
    say("opened " + (project.name.empty() ? project.root.string() : project.name));
    return {};
}

void App::answer_trust(bool trusted) {
    if (!pending_trust_) {
        return;
    }
    const std::filesystem::path root = *pending_trust_;
    pending_trust_.reset();
    if (!trusted) {
        if (!project_open()) {
            say("not trusted -- choose a folder to work in");
        }
        return;
    }
    trust_.trust(root);
    if (const std::string error = open_project(root); !error.empty()) {
        say(error);
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

bool App::retry_turn(std::size_t index) {
    if (engine_->is_busy()) {
        return false;   // the buttons are not offered then; this is the belt to that brace
    }
    const Snapshot snapshot = state_.snapshot();
    if (index >= snapshot.turns.size()) {
        return false;
    }
    const std::string prompt = snapshot.turns[index].prompt;
    std::vector<attach::Attachment> attachments;
    for (const TurnAttachment& tile : snapshot.turns[index].attachments) {
        attachments.push_back(attach::from_tile(tile));
    }
    state_.truncate_turns(index);
    rebuild_history();
    engine_->submit(prompt, std::nullopt, std::move(attachments));
    return true;
}

bool App::delete_turn(std::size_t index) {
    if (engine_->is_busy()) {
        return false;
    }
    state_.remove_turn(index);
    rebuild_history();
    persist_session();
    return true;
}

void App::rebuild_history() {
    const Snapshot snapshot = state_.snapshot();
    std::vector<ChatMessage> history;
    for (const Turn& turn : snapshot.turns) {
        // The same rule the session resume uses: only exchanges that actually
        // produced an answer go back to the expert. A failed or canceled turn
        // in the context teaches it that not answering is a thing that happens
        // here.
        if (!turn.failed && !turn.canceled && !turn.reply.empty()) {
            history.push_back({"user", attach::recalled(turn.attachments) + turn.prompt});
            history.push_back({"assistant", turn.reply});
        }
    }
    engine_->restore_history(std::move(history));

    // The session file is written from the turn count, so a shorter transcript
    // has to reset it or the next save believes it has already stored turns
    // that are no longer there.
    persisted_turns_ = 0;
}

std::string App::open_session(const std::string& id) {
    if (!store_) {
        return "no project is open, and conversations are kept per project";
    }
    if (engine_->is_busy() || engine_->cooking()) {
        return "finish what is running before opening another conversation";
    }

    std::vector<Turn> turns;
    TokenUsage        usage;
    std::string       error;
    if (!store_->load(id, turns, usage, error)) {
        return error.empty() ? "could not read that conversation" : error;
    }

    // What is on screen and what the expert can see are set together. Putting
    // the turns back without the history would give an expert a transcript it
    // has no memory of, and it would answer the next question as though the
    // conversation had not happened.
    persist_session();
    state_.clear_turns();
    for (Turn& turn : turns) {
        state_.restore_turn(std::move(turn));
    }
    store_->adopt(id);
    rebuild_history();
    persisted_turns_ = state_.snapshot().turns.size();
    return {};
}

// ---------------------------------------------------------------------------
// Trying a fine-tune before keeping it
// ---------------------------------------------------------------------------

namespace {

/// The recipe with that id, from what is on disk.
std::optional<lab::Recipe> recipe_by_id(const std::string& id) {
    for (const lab::Recipe& saved : lab::saved_recipes()) {
        if (saved.id == id) {
            return saved;
        }
    }
    return std::nullopt;
}

}  // namespace

std::string App::test_begin(const std::string& recipe_id) {
    const std::optional<lab::Recipe> recipe = recipe_by_id(recipe_id);
    if (!recipe) {
        return "no such expert";
    }
    if (recipe->trained_path.empty()) {
        return "that one has not produced a file yet";
    }
    std::error_code ec;
    if (!std::filesystem::exists(recipe->trained_path, ec)) {
        return "the trained file is not where the recipe says it is:\n"
               + recipe->trained_path;
    }
    if (engine_->is_busy() || engine_->cooking()) {
        return "finish what is running first";
    }

    Config edited = config_;
    Expert expert;
    expert.id    = kTestSeat;
    expert.name  = recipe->name + " (testing)";
    expert.blurb = recipe->purpose.empty()
                       ? std::string("A fine-tune being tried before it is kept.")
                       : recipe->purpose;

    std::string error;
    if (!edited.roster.add(std::move(expert), error)) {
        return error;
    }
    ModelParams params = edited.defaults;
    params.model       = recipe->trained_path;
    edited.experts[ExpertId(kTestSeat)] = params;

    // To the engine and nowhere else. update_config would write this to the
    // config file, and the seat is meant to last exactly as long as the
    // window that asked for it.
    engine_->apply_config(edited);
    testing_ = recipe_id;
    return {};
}

void App::test_end() {
    if (testing_.empty()) {
        return;
    }
    testing_.clear();
    // The real configuration back, which is the one that was never changed.
    engine_->apply_config(config_);
}

std::string App::keep(const std::string& recipe_id) {
    std::optional<lab::Recipe> recipe = recipe_by_id(recipe_id);
    if (!recipe) {
        return "no such expert";
    }
    test_end();

    recipe->stage       = lab::Stage::Finished;
    recipe->finished_at = lab::now_seconds();
    std::string error;
    if (!lab::save(*recipe, error)) {
        return error;
    }

    // And a seat, which is what keeping it is for. Skipped where the roster
    // already has one by that name: keeping the same expert twice should not
    // produce two of it.
    if (config_.roster.find(recipe->name)) {
        say(recipe->name + " is finished");
        return {};
    }

    Config edited = config_;
    Expert expert;
    expert.name  = recipe->name;
    expert.blurb = recipe->purpose;
    if (!edited.roster.add(expert, error)) {
        return error;
    }
    const ExpertId id = make_expert_id(recipe->name);
    // From the defaults rather than from nothing, so the new seat samples the
    // way every other seat does until somebody says otherwise.
    ModelParams    params = edited.defaults;
    params.model       = recipe->trained_path;
    edited.experts[id] = params;
    update_config([&edited](Config& config) { config = edited; });

    // So the delegator has something to route on besides the name.
    engine_->write_examples(id);
    say(recipe->name + " is finished and has joined the experts");
    return {};
}

void App::begin_update_check() {
    // The cache first, and always: it is a file read, it is what the last check
    // found, and it is what the window shows until a new answer arrives.
    update::read_cache(update_);

    if (!config_.ui.check_updates || update_checking_ != nullptr) {
        return;
    }
    // On a thread, like every other errand that leaves the machine. A version
    // check that made the window wait for a network round trip at startup would
    // be a worse bug than the one it is trying to tell you about.
    auto check = std::make_shared<UpdateCheck>();
    update_checking_ = check;
    // In the pool rather than on a thread of its own left to finish whenever:
    // the pool is joined before this object goes away, and a detached thread
    // that woke up afterwards would be calling into one that had.
    workers_.post([this, check]() {
        update::State state = update::refresh(/*allowed_to_ask=*/true);
        {
            const std::lock_guard<std::mutex> lock(check->mutex);
            check->state = std::move(state);
            check->done  = true;
        }
        wake();
    });
}

void App::collect_update_check() {
    if (update_checking_ == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(update_checking_->mutex);
    if (!update_checking_->done) {
        return;
    }
    update_ = update_checking_->state;
    update_checking_.reset();

    // Said once, when the answer comes back. The gear carries a dot from then
    // on, and this is the line that says what the dot means.
    if (update_available()) {
        say("Crucible " + update_.latest + " is available  ·  "
            + std::string(update::update_command()));
    }
}

}  // namespace crucible::gui
