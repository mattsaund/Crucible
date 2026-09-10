// SPDX-License-Identifier: MIT
//
// The command list and the matching. See completion.hpp for the why.
#include "crucible/routing/completion.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

#include "crucible/routing/expert.hpp"

namespace crucible::ui {
namespace {

std::vector<CommandInfo> build_commands(const Roster& roster) {
    std::vector<CommandInfo> commands{
        {"help",     "this list", false},
        {"resume",   "reopen an earlier conversation about this project", false},
        {"new",      "start a fresh conversation, keeping this one on disk", false},
        {"usage",    "tokens spent this session and on this project", false},
        {"cook",     "work on this project until you stop it: /cook [30m] <goal>", true},
        {"stop",     "wrap up the cook: finishing touches, then done", false},
        {"cooks",    "what past cooks changed, and how long they took", false},
        {"experts",  "which seats are filled", false},
        {"newexpert", "add an expert: a name, and what it is trained in", false},
        {"ejectexpert", "remove an expert from the list", true},
        {"runtimes", "install or remove CUDA / Vulkan / CPU backends", false},
        {"devices",  "compute devices llama.cpp found", false},
        {"search",   "look something up on the internet", true},
        {"effort",    "how hard a thinking model works: low, medium, high", true},
        {"showthinking", "keep a thinking model's working on screen", false},
        {"hidethinking", "replace a thinking model's working with its answer", false},
        {"auto",      "apply file edits without asking first", false},
        {"release",  "unload the resident expert and free its memory", false},
        {"clear",    "clear the transcript and the experts' history", false},
        {"settings", "assign models, tune sampling, choose hardware", false},
        {"models",   "list the .gguf files in the models directory", false},
        {"paths",    "where the config, models and log live", false},
        {"quit",     "leave", false},
    };

    // The experts, so "/phy<tab>" reaches Physics. Their summaries come from
    // the roster rather than being written again here, which is what makes a
    // seat added a minute ago completable without a restart.
    for (const Expert& seat : roster.experts()) {
        commands.push_back({seat.id, "send straight to " + seat.name, true});
    }
    return commands;
}

/// The word after the slash, or nothing when `input` is not a bare command
/// being typed.
std::optional<std::string> typed_word(std::string_view input) {
    if (input.size() < 1 || input.front() != '/') {
        return std::nullopt;
    }
    const std::string_view rest = input.substr(1);
    // A space means the command is settled and the user has moved on to the
    // argument, so there is nothing left to complete.
    if (rest.find_first_of(" \t") != std::string_view::npos) {
        return std::nullopt;
    }

    std::string lowered(rest);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

}  // namespace

std::vector<CommandInfo> all_commands(const Roster& roster) {
    return build_commands(roster);
}

std::vector<CommandInfo> command_matches(std::string_view input, const Roster& roster) {
    const std::optional<std::string> word = typed_word(input);
    if (!word) {
        return {};
    }

    std::vector<CommandInfo> matches;
    for (const CommandInfo& command : all_commands(roster)) {
        if (command.name.rfind(*word, 0) == 0) {
            matches.push_back(command);
        }
    }

    // An exact and only match is not a suggestion, it is the thing already
    // typed -- offering to complete "/help" to "/help" is noise.
    if (matches.size() == 1 && matches.front().name == *word) {
        return {};
    }
    return matches;
}

std::string command_completion(std::string_view input, const CommandInfo& choice) {
    const std::optional<std::string> word = typed_word(input);
    if (!word) {
        return {};
    }
    if (choice.name.rfind(*word, 0) != 0) {
        return {};
    }

    std::string completion = choice.name.substr(word->size());
    // A command that takes a prompt is never the whole line, so the space that
    // has to follow it is part of completing it.
    if (choice.takes_prompt) {
        completion += ' ';
    }
    return completion;
}

}  // namespace crucible::ui
