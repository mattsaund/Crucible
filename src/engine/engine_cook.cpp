// SPDX-License-Identifier: MIT
//
// The cook loop.
//
// A prompt is one question and one answer. A cook is a goal and an hour: the
// expert reads the project, changes it, runs it, reads the failure, changes it
// again, and keeps going until the time is up or you stop it. What makes that
// possible is not a bigger model, it is a loop that feeds the result of each
// action back in as the next turn.
//
// Four things here are worth knowing before reading it.
//
// The context is bounded. An hour of build output does not fit in any window,
// and a conversation that overflows one does not fail loudly -- it quietly
// pushes the goal out and the expert starts working on whatever is left. So the
// history is trimmed to the goal, a short account of what has been done, and
// the last few exchanges.
//
// The loop is driven by the clock, not by the model's opinion of its own work.
// The prospectus asks for "cook for thirty minutes" and "cook until I stop it",
// and both of those are budgets. DONE closes a piece of work and the loop asks
// for the next one; it does not end the cook.
//
// Stopping is not canceling. Stop means "wrap up": the cook stops taking new
// work and makes a finishing pass whose whole job is to leave the project in a
// state that runs. Ctrl-C is still there for "stop now".
//
// Everything is journalled as it happens rather than summarized at the end, so
// a cook killed at minute fifty can still say what it changed.
#include "crucible/engine/engine.hpp"

#include <cctype>
#include <chrono>
#include <ctime>
#include <deque>
#include <memory>
#include <set>
#include <utility>

#include "crucible/config/paths.hpp"
#include "crucible/engine/route_policy.hpp"
#include "crucible/llm/response_filter.hpp"
#include "crucible/tools/workshop.hpp"

namespace crucible {
namespace {

using Clock = std::chrono::steady_clock;

long ms_between(Clock::time_point start) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

/// How many past exchanges stay in the window.
///
/// Six is two or three actions with their results. Fewer and the expert forgets
/// the error it was fixing; more and a couple of file listings crowd out the
/// goal. The running account below is what carries anything older.
constexpr std::size_t kRecentTurns = 6;

/// How many journal lines the running account carries.
constexpr std::size_t kAccountLines = 12;

// A cook going in circles is not making progress, and a model that was going to
// notice that itself would not be going in circles. So the loop notices.
//
// This is not hypothetical, and the shape of it is worth stating because the
// obvious check does not catch it. A 1.2B expert given a two-minute cook spent
// sixty-nine steps on one goal, sixty of them the same LIST with the same
// error. Fixed that, and it started alternating LIST and a RUN that could not
// work -- two actions, neither repeated *consecutively*, forever. A
// same-as-last-time counter sees nothing wrong with that.
//
// So the test is over a window: if the last few actions were drawn from a
// handful of distinct ones and none of them changed a file, it is cycling.
// Requiring "changed nothing" is what keeps a real edit-test-edit-test rhythm
// out of it -- that is also two distinct actions repeating, and it is the sound
// of a cook working.
constexpr std::size_t kCycleWindow   = 8;  ///< actions looked at
constexpr std::size_t kCycleDistinct = 3;  ///< at most this many distinct ones
constexpr int kNudgeAt   = 1;   ///< say plainly that it is going in circles
constexpr int kRestartAt = 4;   ///< throw the context away and restate the goal
constexpr int kAbandonAt = 10;  ///< it is stuck; stop rather than burn the budget

/// How many actions may pass before the loop stops and asks what is next.
///
/// The handover exists so a cook can pass from the expert that writes the code
/// to the one that writes the documentation. Finishing a piece with DONE
/// prompts for it -- but a small model may never say DONE, and then a
/// three-hour cook is one expert doing everything, which is the thing the
/// expert list is for.
///
/// So the loop asks on its own schedule as well. Twelve actions is far enough
/// that a piece of work has usually been finished and near enough that a wrong
/// expert is not left in the chair for an hour. The answer is usually the seat
/// already there, which costs one routing pass and no reload.
constexpr int kCheckpointEvery = 12;

/// What the expert is told it is doing, over and above its own system prompt.
std::string cook_system_prompt(const Config& config, const std::string& goal,
                               const tools::WorkshopSettings& workshop,
                               const std::filesystem::path& root) {
    std::string prompt = config.system_prompt;
    prompt +=
        "\n\nYou are working on a real project on disk, over a long session, one "
        "action at a time. The goal is:\n\n";
    prompt += goal;
    prompt += "\n\nThe project is at ";
    prompt += root.string();
    prompt +=
        "\n\nWork in small steps. Look before you change: read a file before "
        "rewriting it, and run the project to find out whether a change worked "
        "rather than assuming. Prefer the smallest change that makes something "
        "measurably better. When a piece of work is finished, say DONE and what "
        "you did, and you will be asked for the next one.";
    prompt += tools::workshop_instructions(workshop, tools::ToolAudience::Cook);
    return prompt;
}

/// A short account of what the cook has done, for the top of the window.
///
/// This is what survives trimming, so it is the cook's memory of itself. Kept
/// to the last few lines: older than that and it is in the journal, which the
/// user can read and the expert does not need to.
std::string running_account(const Cook& cook) {
    if (cook.steps.empty()) {
        return {};
    }
    const std::size_t from = cook.steps.size() > kAccountLines
                           ? cook.steps.size() - kAccountLines : 0;
    std::string text = "What you have done so far:\n";
    for (std::size_t i = from; i < cook.steps.size(); ++i) {
        const CookStep& step = cook.steps[i];
        text += "- ";
        text += step.summary;
        if (!step.ok) {
            text += "  (failed)";
        }
        text += '\n';
    }
    const std::vector<std::string> files = cook.files_touched();
    if (!files.empty()) {
        text += "Files you have changed: ";
        for (std::size_t i = 0; i < files.size(); ++i) {
            text += (i == 0 ? "" : ", ") + files[i];
        }
        text += '\n';
    }
    return text;
}

/// The reply as the model should see its own turn.
///
/// Only the answer. The reasoning is deliberately dropped: the format's own
/// guidance is that previous reasoning is not fed back, and doing it here would
/// fill the window with the expert's working rather than with the project.
std::string turn_text(const std::string& answer) {
    return answer.empty() ? std::string("(no reply)") : answer;
}

}  // namespace

// ---------------------------------------------------------------------------
// Queueing and control
// ---------------------------------------------------------------------------

void Engine::set_project(std::filesystem::path root, std::filesystem::path project_dir) {
    project_root_ = std::move(root);
    cook_log_     = std::make_unique<CookLog>(std::move(project_dir));
}

tools::WorkshopSettings Engine::workshop_for(const std::filesystem::path& root) const {
    tools::WorkshopSettings workshop;
    // Having a root is the permission. It is only ever set to a folder the user
    // answered the trust question for, and that question is asked in the terms
    // this is actually about: read, write, run, inside this directory.
    workshop.enabled             = !root.empty();
    workshop.root                = root;
    workshop.allow_run           = !root.empty();
    workshop.run_timeout_seconds = config_.tools.workshop_timeout;
    return workshop;
}

void Engine::start_cook(std::string goal, int budget_seconds, std::filesystem::path root) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Request request;
        request.kind           = RequestKind::Cook;
        request.prompt         = std::move(goal);
        request.budget_seconds = budget_seconds;
        request.root           = std::move(root);
        pending_.push_back(std::move(request));
    }
    queued_.notify_one();
}

void Engine::stop_cook() {
    cook_stop_.store(true, std::memory_order_relaxed);
    // A cook parked on a question is inside await_cook_answer, not looking at
    // the flag. Waking it is what lets /stop end a cook that is waiting on you.
    cook_answered_.notify_all();
}

void Engine::answer_cook(std::string answer) {
    {
        const std::lock_guard<std::mutex> lock(cook_answer_mutex_);
        cook_answer_ = std::move(answer);
    }
    cook_answered_.notify_all();
}

std::optional<std::string> Engine::await_cook_answer() {
    std::unique_lock<std::mutex> lock(cook_answer_mutex_);
    cook_answered_.wait(lock, [this] {
        return cook_answer_.has_value() || cook_stop_.load(std::memory_order_relaxed)
            || !running_.load(std::memory_order_relaxed);
    });
    return std::exchange(cook_answer_, std::nullopt);
}

void Engine::publish_cook() {
    state_.set_cook(std::make_shared<const Cook>(cook_));
    if (wake_) {
        wake_();
    }
}

void Engine::note_step(CookStep step) {
    cook_.steps.push_back(std::move(step));
    publish_cook();
    // Written after every step rather than at the end. A cook killed at minute
    // fifty should still be able to say what it changed, and the file is tens
    // of kilobytes -- cheaper than the model call that produced the step.
    if (cook_log_) {
        std::string error;
        cook_log_->save(cook_, error);
    }
}

// ---------------------------------------------------------------------------
// One round with the expert
// ---------------------------------------------------------------------------

Engine::CookRound Engine::cook_round(LoadedModel& model, const ModelParams& params,
                                     const std::vector<ChatMessage>& messages) {
    CookRound round;
    const auto start = Clock::now();

    // The same filter the ordinary turn path uses, so a reasoning model's
    // channel markers do not end up being parsed as tool calls.
    ResponseFilter filter;
    const CancelCallback cancel = [this] {
        return cancel_.load(std::memory_order_relaxed)
            || !running_.load(std::memory_order_relaxed);
    };

    model.generate(model.format_chat(messages, true), params,
                   [&](std::string_view chunk) {
                       const ResponseFilter::Piece piece = filter.feed(chunk);
                       round.answer    += piece.answer;
                       round.reasoning += piece.reasoning;
                   },
                   cancel);
    const ResponseFilter::Piece tail = filter.flush();
    round.answer    += tail.answer;
    round.reasoning += tail.reasoning;
    round.ms = ms_between(start);
    return round;
}

// ---------------------------------------------------------------------------
// Who is in the seat
// ---------------------------------------------------------------------------

Engine::CookSeat Engine::take_the_seat(const std::string& work, const CookSeat& current) {
    // Routed like any other prompt, which is the point: the delegator that
    // picks an expert for a question is the same one that picks an expert for
    // the next piece of work, and it does it from the same roster with the same
    // measured prompt.
    Request routing;
    routing.kind   = RequestKind::Prompt;
    routing.prompt = work;
    const RouteDecision decision = resolve(routing);

    CookSeat seat;
    seat.id   = decision.expert;
    seat.name = expert_label(config_.roster, decision.expert);

    if (!config_.has_expert(seat.id)) {
        seat.error = seat.name.empty()
            ? "no expert model is configured to cook with"
            : seat.name + " has no model, and nothing else is configured either";
        return seat;
    }

    seat.params = config_.expert(seat.id);

    // The common case, and the one that must not cost a reload: the delegator
    // chose whoever is already in the chair. A cook that swapped models every
    // iteration because it re-asked the same question would spend most of its
    // budget loading weights.
    if (current.model != nullptr && current.id == seat.id) {
        seat.model = current.model;
        return seat;
    }

    state_.set_mood(Mood::Loading, "swapping in " + seat.name);
    state_.set_seat(seat.id, SeatPhase::Loading, 0.0F);
    state_.set_linked(seat.id);
    if (wake_) {
        wake_();
    }

    // acquire_expert frees whoever was resident before loading the next, which
    // is the whole memory argument for the design: the peak is the larger of
    // the two experts, never their sum.
    std::string error;
    seat.model = host_->acquire_expert(
        seat.id, seat.params,
        [this, &seat](float progress) { state_.set_seat_progress(seat.id, progress); },
        [this] { return cancel_.load(std::memory_order_relaxed); },
        error);
    if (seat.model == nullptr) {
        seat.error = error;
        state_.set_seat(seat.id, SeatPhase::Dormant);
        return seat;
    }
    state_.set_resident(seat.id);
    if (wake_) {
        wake_();
    }
    return seat;
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------

void Engine::do_cook(const std::string& goal, int budget_seconds,
                     const std::filesystem::path& root) {
    cooking_.store(true, std::memory_order_relaxed);
    cook_stop_.store(false, std::memory_order_relaxed);
    cancel_.store(false, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock(cook_answer_mutex_);
        cook_answer_.reset();
    }

    cook_               = Cook{};
    cook_.id            = CookLog::new_id();
    cook_.goal          = goal;
    cook_.state         = CookState::Working;
    cook_.budget_seconds = budget_seconds;
    cook_.started_unix  = static_cast<std::int64_t>(std::time(nullptr));
    publish_cook();

    const tools::WorkshopSettings workshop = workshop_for(root);

    tools::SearchSettings search;
    search.enabled         = config_.tools.web_search;
    search.provider        = config_.tools.search_provider;
    search.endpoint        = config_.tools.search_endpoint;
    search.api_key         = config_.tools.search_api_key;
    search.max_results     = config_.tools.search_results;
    search.timeout_seconds = config_.tools.search_timeout;

    // Who is in the seat, which is no longer fixed for the whole cook: see
    // take_the_seat and the HANDOFF verb.
    CookSeat seat = take_the_seat(goal, CookSeat{});
    if (seat.model == nullptr) {
        cook_.state      = CookState::Failed;
        cook_.outcome    = seat.error.empty()
            ? std::string("no expert model is configured to cook with") : seat.error;
        cook_.ended_unix = static_cast<std::int64_t>(std::time(nullptr));
        publish_cook();
        state_.set_mood(Mood::Error, cook_.outcome);
        cooking_.store(false, std::memory_order_relaxed);
        return;
    }

    const std::string system = cook_system_prompt(config_, goal, workshop, root);
    // The exchanges since the last trim. The goal and the running account are
    // rebuilt from the journal each round, so only this has to be carried.
    std::vector<ChatMessage> recent;

    const auto deadline = budget_seconds > 0
        ? std::optional<Clock::time_point>{Clock::now() + std::chrono::seconds(budget_seconds)}
        : std::nullopt;
    const auto out_of_time = [&deadline] {
        return deadline && Clock::now() >= *deadline;
    };

    // The first thing the expert sees is what is actually in the project.
    //
    // Asking it to "look at the project first" and leaving it to work out how
    // produced experts that invented plausible file names and then spent twenty
    // steps failing to open them. One listing, performed by the loop rather
    // than by the model, removes the whole class of that -- and it is the same
    // thing a person does before touching an unfamiliar directory.
    tools::ToolCall opening;
    opening.kind     = tools::ToolKind::List;
    opening.argument = ".";
    const tools::ToolResult listing = tools::run_tool(opening, workshop, search, {});

    std::string next_instruction =
        "Here is what is in the project:\n\n" + listing.output
        + "\nStart by reading whichever of these files the goal is about, then make "
          "your first improvement. Only these files exist -- do not guess at others.";
    cook_.iterations = 1;

    // The last few actions, and whether any of them changed anything. See
    // kCycleWindow above for why it is a window rather than a counter.
    std::deque<std::pair<std::string, bool>> window;
    int  strikes = 0;
    bool looping = false;

    // Actions since the last time the delegator was asked who should be doing
    // this. See kCheckpointEvery.
    int since_checkpoint = 0;

    // The line the loop uses to ask, in both places that ask it.
    const std::string handoff_request =
        "Say what the next most valuable piece of work towards the goal is, in one "
        "line, as:\n\nHANDOFF: <the next piece of work>\n\nSay nothing else. If it "
        "needs a different kind of expertise than yours, say so plainly in that line.";

    while (!cook_stop_.load(std::memory_order_relaxed)
           && !cancel_.load(std::memory_order_relaxed)
           && running_.load(std::memory_order_relaxed)
           && !out_of_time()) {

        std::vector<ChatMessage> messages;
        messages.push_back({"system", system});
        if (const std::string account = running_account(cook_); !account.empty()) {
            messages.push_back({"user", account});
            messages.push_back({"assistant", "Understood. Continuing."});
        }
        // Trimmed to the last few exchanges. Everything older is in the account
        // above, which is what stops an hour of build output pushing the goal
        // out of the window.
        const std::size_t from = recent.size() > kRecentTurns * 2
                               ? recent.size() - kRecentTurns * 2 : 0;
        messages.insert(messages.end(), recent.begin() + static_cast<std::ptrdiff_t>(from),
                        recent.end());
        messages.push_back({"user", next_instruction});

        state_.set_mood(Mood::Thinking, seat.name + " is working");
        if (wake_) {
            wake_();
        }
        const CookRound round = cook_round(*seat.model, seat.params, messages);
        if (cancel_.load(std::memory_order_relaxed)) {
            break;
        }

        recent.push_back({"user", next_instruction});
        recent.push_back({"assistant", turn_text(round.answer)});

        const std::optional<tools::ToolCall> call =
            tools::parse_tool_call(round.answer, round.reasoning);

        if (!call) {
            // It talked instead of acting. Recorded as a step so the user can
            // see the thinking, and answered with the one instruction that gets
            // it back on the protocol.
            CookStep step;
            step.iteration = cook_.iterations;
            step.expert    = seat.id;
            step.kind      = "think";
            step.summary   = round.answer.empty() ? std::string("(said nothing)")
                                                  : round.answer.substr(0, 200);
            step.ms        = round.ms;
            note_step(std::move(step));

            // Distinguish "did not try" from "tried and got the syntax wrong".
            // A model writing `WRITE /path "fixed it"` is doing the work and
            // failing on one character, and answering it with a generic "take
            // an action" is how a whole cook goes by with nothing written.
            const tools::ToolKind attempted =
                tools::attempted_tool_call(round.answer, round.reasoning);
            if (attempted == tools::ToolKind::Write) {
                next_instruction =
                    "That was nearly right, but it cannot be run. WRITE needs a colon "
                    "after it, and the new contents of the file go in a fenced block "
                    "on the following lines -- never on the same line, and never as a "
                    "description of the change. Exactly this shape:\n\n"
                    "WRITE: path/to/file\n```\n<the complete new contents>\n```\n\n"
                    "Try that again.";
            } else if (attempted != tools::ToolKind::None) {
                // Named in upper case, because that is how the protocol wants
                // it back -- the lower-case spelling is the journal's.
                std::string verb(tools::tool_kind_name(attempted));
                for (char& c : verb) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                next_instruction =
                    "That was nearly right, but it cannot be run: " + verb
                    + " needs a colon after it, like `" + verb
                    + ": ...`. Write it again with the colon.";
            } else {
                next_instruction =
                    "Take one action now, using exactly one of the commands you were "
                    "given, on a line of its own, with its colon.";
            }
            continue;
        }

        if (call->kind == tools::ToolKind::Ask) {
            cook_.state    = CookState::Asking;
            cook_.question = call->argument;
            CookStep step;
            step.iteration = cook_.iterations;
            step.expert    = seat.id;
            step.kind      = "ask";
            step.summary   = "asked: " + call->argument;
            step.ms        = round.ms;
            note_step(std::move(step));

            state_.set_mood(Mood::Idle, "waiting for your answer");
            if (wake_) {
                wake_();
            }
            const std::optional<std::string> answer = await_cook_answer();
            cook_.question.clear();
            cook_.state = CookState::Working;
            publish_cook();
            if (!answer) {
                break;  // stopped while waiting
            }
            recent.push_back({"user", "The user answered: " + *answer});
            next_instruction = "Carry on with that in mind.";
            continue;
        }

        if (call->kind == tools::ToolKind::Handoff) {
            const std::string work = call->argument.empty() ? goal : call->argument;

            CookStep step;
            step.iteration = cook_.iterations;
            step.expert    = seat.id;
            step.kind      = "handoff";
            step.summary   = work;
            step.ms        = round.ms;
            note_step(std::move(step));

            const CookSeat next = take_the_seat(work, seat);
            if (next.model == nullptr) {
                // Nobody could take it. Rather than end the cook, the expert
                // that is already loaded carries on with the work it described
                // -- a worse specialist finishing the job beats no job.
                CookStep note;
                note.iteration = cook_.iterations;
                note.expert    = seat.id;
                note.kind      = "note";
                note.ok        = false;
                note.summary   = next.error + " -- carrying on with " + seat.name;
                note_step(std::move(note));
            } else {
                if (next.id != seat.id) {
                    CookStep note;
                    note.iteration = cook_.iterations;
                    note.expert    = next.id;
                    note.kind      = "note";
                    note.summary   = seat.name + " handed over to " + next.name;
                    note_step(std::move(note));
                }
                seat = next;
            }

            // A new expert has none of the old one's conversation, and giving
            // it one would be giving it someone else's turns as its own. The
            // running account, which is rebuilt from the journal every round,
            // is what carries the history across the handover.
            ++cook_.iterations;
            recent.clear();
            since_checkpoint = 0;
            next_instruction = "Do this now: " + work;
            continue;
        }

        if (call->kind == tools::ToolKind::Done) {
            CookStep step;
            step.iteration = cook_.iterations;
            step.expert    = seat.id;
            step.kind      = "done";
            step.summary   = call->argument.empty() ? std::string("finished a piece of work")
                                                    : call->argument;
            step.ms        = round.ms;
            note_step(std::move(step));

            // A piece of work is finished, not the cook. The loop is driven by
            // the budget, and the whole point is that it comes back round.
            //
            // Asking for the next piece of work as a HANDOFF line rather than
            // just saying "carry on" is what lets the roster change hands: the
            // line goes back through the delegator, and if it describes
            // documentation rather than code, a writing expert takes the seat.
            ++cook_.iterations;
            recent.clear();
            since_checkpoint = 0;
            next_instruction = "That piece is done. " + handoff_request;
            continue;
        }

        const auto started = Clock::now();
        const tools::ToolResult result = tools::run_tool(*call, workshop, search,
            [this] { return cancel_.load(std::memory_order_relaxed); });

        CookStep step;
        step.iteration = cook_.iterations;
        step.expert    = seat.id;
        step.kind      = std::string(tools::tool_kind_name(call->kind));
        step.summary   = result.summary;
        step.detail    = result.detail;
        step.ok        = result.ok;
        step.ms        = ms_between(started);
        step.changed   = result.changed;
        note_step(std::move(step));

        recent.push_back({"user", result.output});
        next_instruction = "Continue.";

        // --- has anyone else got a better claim on this? -------------------
        //
        // Asked on a schedule rather than only when the expert thinks to. See
        // kCheckpointEvery: without this a model that never says DONE keeps the
        // seat for the whole budget, and the expert list may as well be one
        // chair.
        if (++since_checkpoint >= kCheckpointEvery) {
            since_checkpoint = 0;
            next_instruction =
                "Stop and take stock. " + handoff_request
                + "\nIf you are the right expert for it, say so in the line and carry "
                  "on.";
            continue;
        }

        // --- am I going in circles? ---------------------------------------
        window.emplace_back(step.kind + "|" + call->argument, !result.changed.empty());
        if (window.size() > kCycleWindow) {
            window.pop_front();
        }

        bool cycling = window.size() == kCycleWindow;
        if (cycling) {
            std::set<std::string> distinct;
            for (const auto& [signature, changed] : window) {
                if (changed) {
                    cycling = false;  // something moved; this is work, not a loop
                    break;
                }
                distinct.insert(signature);
            }
            cycling = cycling && distinct.size() <= kCycleDistinct;
        }
        strikes = cycling ? strikes + 1 : 0;

        if (strikes >= kAbandonAt) {
            // Stopping beats spending the rest of an hour on it. The finishing
            // pass below still runs, so the project is left tidy.
            looping = true;
            CookStep note;
            note.iteration = cook_.iterations;
            note.expert    = seat.id;
            note.kind      = "note";
            note.ok        = false;
            note.summary   = "stopped: going in circles with nothing changing";
            note_step(std::move(note));
            break;
        }
        if (strikes >= kRestartAt) {
            // The context is what is keeping it here: a window full of the same
            // failures reads as confirmation that this is the job. Throwing it
            // away and restating the goal is the only move that changes
            // anything.
            recent.clear();
            window.clear();
            ++cook_.iterations;
            strikes          = 0;
            since_checkpoint = 0;
            // Handed the listing again, because the thing it has most likely
            // lost by now is what the project actually contains -- and telling
            // it to "read a file you have not read" without saying which files
            // exist is how it invented paths in the first place.
            next_instruction =
                "Stop. The last several actions changed nothing. Forget them and "
                "start again from the goal: " + goal
                + "\n\nHere is what is in the project:\n\n" + listing.output
                + "\nYour next action must be to READ one of those files.";
        } else if (strikes >= kNudgeAt) {
            next_instruction =
                "You are going in circles: the last several actions changed nothing "
                "and told you nothing new. Do something different -- READ a file you "
                "have not read, or WRITE a change to one.";
        }
    }

    // --- the finishing pass ------------------------------------------------
    //
    // The reason /stop is not the same as Ctrl-C. A cook interrupted mid-edit
    // has a project in a state nobody asked for; this is the pass that gets it
    // back to something that runs, and then says what happened.
    const bool interrupted = cancel_.load(std::memory_order_relaxed)
                          || !running_.load(std::memory_order_relaxed);
    if (!interrupted) {
        cook_.state = CookState::Finishing;
        publish_cook();
        state_.set_mood(Mood::Thinking, seat.name + " is finishing up");
        if (wake_) {
            wake_();
        }

        // Its own budget, so "wrap up" cannot itself run forever.
        constexpr int kFinishingRounds = 6;
        std::string instruction =
            "Time is up. Do not start anything new. Make only the changes needed to "
            "leave the project in a working state -- finish a half-made edit, fix "
            "what you broke, and check it runs. When there is nothing left to "
            "repair, reply with DONE: and a short account of what you changed "
            "overall and what is left to do.";

        for (int i = 0; i < kFinishingRounds; ++i) {
            if (cancel_.load(std::memory_order_relaxed)) {
                break;
            }
            std::vector<ChatMessage> messages;
            messages.push_back({"system", system});
            if (const std::string account = running_account(cook_); !account.empty()) {
                messages.push_back({"user", account});
                messages.push_back({"assistant", "Understood."});
            }
            messages.push_back({"user", instruction});

            const CookRound round = cook_round(*seat.model, seat.params, messages);
            const std::optional<tools::ToolCall> call =
                tools::parse_tool_call(round.answer, round.reasoning);

            if (!call || call->kind == tools::ToolKind::Done || call->kind == tools::ToolKind::Ask) {
                cook_.outcome = call && !call->argument.empty() ? call->argument : round.answer;
                break;
            }

            const auto started = Clock::now();
            const tools::ToolResult result = tools::run_tool(*call, workshop, search,
                [this] { return cancel_.load(std::memory_order_relaxed); });

            CookStep step;
            step.iteration = cook_.iterations;
            step.expert    = seat.id;
            step.kind      = std::string(tools::tool_kind_name(call->kind));
            step.summary   = result.summary;
            step.detail    = result.detail;
            step.ok        = result.ok;
            step.ms        = ms_between(started);
            step.changed   = result.changed;
            note_step(std::move(step));

            instruction = result.output +
                "\n\nAnything else that must be repaired before this is left alone? If "
                "not, reply DONE: and a short account of what you changed.";
        }
    }

    cook_.state = interrupted || cook_stop_.load(std::memory_order_relaxed)
                      ? CookState::Stopped
                      : looping ? CookState::Failed
                                : CookState::Done;
    if (cook_.outcome.empty()) {
        cook_.outcome = interrupted ? "interrupted"
                      : looping ? "stopped: the expert repeated the same action without "
                                  "making progress -- a larger model may be needed for "
                                  "this goal"
                                : "finished";
    }
    cook_.ended_unix = static_cast<std::int64_t>(std::time(nullptr));
    publish_cook();
    if (cook_log_) {
        std::string save_error;
        cook_log_->save(cook_, save_error);
    }

    state_.set_linked(std::nullopt);
    state_.set_mood(Mood::Idle, "cook finished -- " + cook_.headline());
    cooking_.store(false, std::memory_order_relaxed);
    cook_stop_.store(false, std::memory_order_relaxed);
    if (wake_) {
        wake_();
    }
}

}  // namespace crucible
