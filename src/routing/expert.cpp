// SPDX-License-Identifier: MIT
//
// The roster, and everything generated from it.
//
// The list is the single source of truth. The labels the delegator chooses
// between, its system prompt, its worked examples, the keyword sets the
// model-free router scores with, and the order experts appear in the panel
// are all derived from it, so they cannot drift apart -- adding a seat adds it
// everywhere, whether the seat came from the defaults below or from a user
// typing `/newexpert` a minute ago.
#include "crucible/routing/expert.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace crucible {
namespace {

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view text) {
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) != 0)) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

/// Words that appear in every description ever written and therefore separate
/// nothing. The keyword router scores by count, so one of these in a seat's
/// list would win every prompt containing the word "with".
const std::set<std::string>& stop_words() {
    static const std::set<std::string> kStop{
        "about", "above", "after", "against", "along", "also", "among", "and", "another",
        "any", "anything", "are", "around", "because", "been", "before", "being", "below",
        "beside", "besides", "between", "both", "build", "building", "can", "come",
        "could", "deal", "deals", "does", "doing", "done", "down", "during", "each",
        "either", "else", "etc", "even", "ever", "every", "everything", "few", "find",
        "first", "for", "from", "general", "generally", "get", "give", "goes", "going",
        "good", "handle", "handles", "has", "have", "having", "help", "here", "how",
        "however", "into", "its", "just", "kind", "kinds", "know", "knowledge", "last",
        "like", "lot", "made", "make", "makes", "making", "many", "may", "might", "more",
        "most", "much", "must", "need", "needs", "new", "not", "now", "off", "often",
        "one", "only", "onto", "other", "others", "out", "over", "own", "part", "parts",
        "per", "put", "questions", "related", "same", "see", "seen", "set", "should",
        "similar", "since", "some", "something", "sort", "specific", "still", "stuff",
        "such", "take", "takes", "than", "that", "the", "their", "them", "then", "there",
        "these", "they", "thing", "things", "this", "those", "though", "through", "thus",
        "topic", "topics", "toward", "under", "until", "upon", "use", "used", "uses",
        "using", "very", "want", "was", "well", "were", "what", "when", "where", "which",
        "while", "who", "whose", "why", "will", "with", "within", "without", "work",
        "working", "works", "would", "you", "your"};
    return kStop;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
//
// Crucible ships no experts.
//
// There used to be nine built-in seats here -- Mathematics, Programming,
// Physics and the rest -- seeded into every fresh config. They are gone, and a
// new install now starts with an empty roster that the user fills.
//
// The reason is that they were never really defaults; they were a guess at what
// somebody else's machine is for, complete with model assignments nobody had
// made. A roster the user owns from the first seat is simpler to explain, has
// no "is this one mine or theirs" state to carry, and cannot present eight
// unconfigured experts as though they were ready to answer.

Roster Roster::bare() {
    return Roster{};
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

const Expert& Roster::at(std::size_t index) const {
    // Out of range resolves to a blank expert rather than to whatever is at
    // zero. An index can go stale -- a seat ejected while a turn was in flight
    // -- and reading "nobody in particular" is true, where reading index zero
    // would silently attribute the work to the first expert in the list.
    static const Expert kNobody{};
    return index < experts_.size() ? experts_[index] : kNobody;
}

std::optional<std::size_t> Roster::find(std::string_view key) const {
    const std::string needle = to_lower(trim(key));
    if (needle.empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < experts_.size(); ++i) {
        const Expert& expert = experts_[i];
        if (needle == to_lower(expert.id) || needle == to_lower(expert.tag) ||
            needle == to_lower(expert.name)) {
            return i;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

bool Roster::add(Expert expert, std::string& error) {
    expert.name = trim(expert.name);
    if (expert.name.empty()) {
        error = "an expert needs a name";
        return false;
    }
    if (expert.id.empty()) {
        expert.id = make_expert_id(expert.name);
    }
    if (expert.id.empty()) {
        error = "\"" + expert.name + "\" has no letters or digits in it to make a name from";
        return false;
    }
    if (find(expert.id) || find(expert.name)) {
        error = "there is already an expert called " + expert.name;
        return false;
    }

    expert.blurb = trim(expert.blurb);
    if (expert.blurb.empty()) {
        error = "describe what " + expert.name + " is trained in, or the delegator "
                "has nothing to route on";
        return false;
    }

    if (expert.tag.empty()) {
        std::vector<std::string> taken;
        taken.reserve(experts_.size());
        for (const Expert& other : experts_) {
            taken.push_back(other.tag);
        }
        expert.tag = make_expert_tag(expert.name, taken);
    }
    if (expert.keywords.empty()) {
        expert.keywords = derive_keywords(expert.name, expert.blurb);
    }

    experts_.push_back(std::move(expert));
    return true;
}

bool Roster::remove(std::string_view key, std::string& error) {
    const std::optional<std::size_t> index = find(key);
    if (!index) {
        error = "no expert called \"" + std::string(key) + "\"";
        return false;
    }
    experts_.erase(experts_.begin() + static_cast<std::ptrdiff_t>(*index));
    return true;
}

bool Roster::update(const ExpertId& id, const Expert& replacement) {
    for (Expert& expert : experts_) {
        if (expert.id == id) {
            // The id is the one field that cannot move: it is what the config
            // file and the stored sessions are keyed on.
            const ExpertId keep = expert.id;
            expert    = replacement;
            expert.id = keep;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// What the delegator sees
// ---------------------------------------------------------------------------

std::vector<std::string> Roster::router_labels() const {
    // The name, not the tag.
    //
    // This is worth 39 points. Scored on Crucible's 54-prompt benchmark with
    // LFM2.5-1.2B, answering with the four-letter tags gets 48% and answering
    // with the names gets 87%. The tags are not words: a delegator choosing
    // between "PHIL" and "PHYS" is comparing two spellings that share a first
    // token, and every philosophy question in the set went to physics.
    // "Philosophy" and "Physics" are things the model knows about.
    //
    // The tags stay in the system prompt, where they measurably help (dropping
    // them costs 6 points), and in the expert panel, where a fixed width is what
    // makes the chips line up.
    std::vector<std::string> labels;
    labels.reserve(experts_.size());
    for (const Expert& expert : experts_) {
        labels.push_back(expert.name);
    }
    return labels;
}

std::string Roster::router_system_prompt() const {
    // Routable seats only, and deliberately compact. An earlier version
    // described each subject in prose and closed by naming a catch-all; small
    // models latched onto that closing line and answered it for almost
    // everything (16% accurate). The delegator's job is to pick a specialist,
    // so it is never shown a way to decline.
    std::string prompt =
        "You label a question with the one subject it belongs to.\n"
        "Reply with only a tag and a confidence.\n\n";
    for (const Expert& expert : experts_) {
        // Tag, name, then the remit. The tag earns its place here even though
        // the delegator answers with the name: on the 54-prompt benchmark,
        // listing the options without their tags costs 6 points (87% to 81%).
        // Reading it as a labeled menu appears to be what helps.
        prompt += expert.tag;
        prompt += "  ";
        prompt += expert.name;
        prompt += ": ";
        prompt += expert.blurb;
        prompt += "\n";
    }
    return prompt;
}

std::vector<std::pair<std::string, std::string>> Roster::router_examples() const {
    std::vector<std::pair<std::string, std::string>> examples;
    for (const Expert& expert : experts_) {
        for (const std::string& question : expert.examples) {
            if (question.empty()) {
                continue;
            }
            // The answer is exactly one of router_labels() and nothing else.
            // The delegator does not write a confidence -- that comes from
            // comparing the labels against each other -- and an example that
            // showed one would teach it to continue past the string being
            // scored.
            examples.emplace_back(question, expert.name);
        }
    }
    return examples;
}

// ---------------------------------------------------------------------------
// Deriving the fields a user is not asked for
// ---------------------------------------------------------------------------

std::string expert_label(const Roster& roster, const ExpertId& id) {
    if (const std::optional<std::size_t> index = roster.find(id)) {
        return roster.at(*index).name;
    }
    return id;
}

std::string make_expert_id(std::string_view name) {
    std::string id;
    bool pending_hyphen = false;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0) {
            if (pending_hyphen && !id.empty()) {
                id += '-';
            }
            pending_hyphen = false;
            id += static_cast<char>(std::tolower(byte));
        } else {
            pending_hyphen = true;
        }
    }
    return id;
}

std::string make_expert_tag(std::string_view name, const std::vector<std::string>& taken) {
    // Initials for a multi-word name, so "Rust Async" is RA rather than RUST
    // and cannot collide with a future "Rust" seat on its first four letters.
    std::string initials;
    bool at_start = true;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0) {
            if (at_start && initials.size() < 4) {
                initials += static_cast<char>(std::toupper(byte));
            }
            at_start = false;
        } else {
            at_start = true;
        }
    }

    std::string tag = initials.size() >= 2 ? initials : std::string();
    if (tag.empty()) {
        for (const char c : name) {
            const auto byte = static_cast<unsigned char>(c);
            if ((std::isalnum(byte) != 0) && tag.size() < 4) {
                tag += static_cast<char>(std::toupper(byte));
            }
        }
    }
    if (tag.empty()) {
        tag = "EXPT";
    }

    const auto is_taken = [&taken](const std::string& candidate) {
        return std::any_of(taken.begin(), taken.end(), [&](const std::string& other) {
            return to_lower(other) == to_lower(candidate);
        });
    };
    if (!is_taken(tag)) {
        return tag;
    }
    // Two seats sharing a chip is a bug you only notice once the wrong one
    // lights up, so a collision is broken rather than tolerated.
    for (int suffix = 2; suffix <= 9; ++suffix) {
        std::string candidate = tag.substr(0, 3) + std::to_string(suffix);
        if (!is_taken(candidate)) {
            return candidate;
        }
    }
    return tag;
}

std::vector<std::string> derive_keywords(std::string_view name, std::string_view blurb) {
    // Whole words of four characters or more that are not stop words. The
    // keyword router matches whole words and scores by count, so a short or
    // common word costs far more in false positives than it earns in recall --
    // this is the same principle the shipped keyword sets were curated on.
    std::vector<std::string> words;
    std::set<std::string>    seen;

    const auto harvest = [&](std::string_view text) {
        std::string current;
        const auto flush = [&]() {
            if (current.size() >= 4 && stop_words().count(current) == 0 &&
                seen.insert(current).second) {
                words.push_back(current);
            }
            current.clear();
        };
        for (const char c : text) {
            const auto byte = static_cast<unsigned char>(c);
            if ((std::isalnum(byte) != 0) || c == '+' || c == '#') {
                current += static_cast<char>(std::tolower(byte));
            } else {
                flush();
            }
        }
        flush();
    };

    harvest(name);
    harvest(blurb);
    return words;
}

std::string example_request_prompt(std::string_view name, std::string_view blurb) {
    // Asks for bare questions and nothing else. Every constraint here exists
    // because a model broke it in testing: they number lists, they explain
    // first, they answer their own question, and they drift towards the general
    // ("what is chemistry") when not told to be specific.
    std::string prompt = "An expert called \"";
    prompt += name;
    prompt += "\" handles: ";
    prompt += blurb;
    prompt +=
        "\n\nWrite exactly two short questions a person would ask that this expert "
        "should obviously answer.\n"
        "Rules: one question per line. No numbering, no bullets, no quotes, no "
        "explanation. Each question must be specific enough that it could not be "
        "asked of a different expert.\n";
    return prompt;
}

std::vector<std::string> parse_examples(std::string_view reply, std::size_t wanted) {
    std::vector<std::string> questions;
    std::istringstream       stream{std::string(reply)};
    std::string              line;

    while (std::getline(stream, line) && questions.size() < wanted) {
        std::string text = trim(line);
        if (text.empty()) {
            continue;
        }

        // Strip the decoration models add whatever they were told: "1. ",
        // "- ", "* ", "Q: ", and surrounding quotes.
        std::size_t start = 0;
        while (start < text.size() &&
               ((std::isdigit(static_cast<unsigned char>(text[start])) != 0) ||
                text[start] == '.' || text[start] == ')' || text[start] == '-' ||
                text[start] == '*' || text[start] == ' ')) {
            ++start;
        }
        text = trim(std::string_view(text).substr(start));
        if (text.size() > 2 && (text.compare(0, 2, "Q:") == 0 || text.compare(0, 2, "A:") == 0)) {
            text = trim(std::string_view(text).substr(2));
        }
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
            text = trim(std::string_view(text).substr(1, text.size() - 2));
        }

        // A line of preamble ("Here are two questions:") ends in a colon and is
        // not a question; a real one is long enough to be worth showing a
        // model. Both filters are cheap and both fire in practice.
        if (text.size() < 12 || text.back() == ':') {
            continue;
        }
        questions.push_back(std::move(text));
    }
    return questions;
}

}  // namespace crucible
