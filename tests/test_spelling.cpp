// SPDX-License-Identifier: MIT
//
// One spelling, everywhere: American.
//
// This is a lint rather than a unit test, and it is here rather than in a
// script because this is the suite that actually runs on every build. The
// codebase was written in British English throughout -- colour, centre,
// behaviour, recognise -- and converting it was a one-off sweep over eighty-five
// files. Without something watching, it comes back one comment at a time, and
// then half the interface says "colour" and half says "color", which is worse
// than either.
//
// Identifiers count. `text_coloured` and `IconHit::centre` were function and
// field names, not prose, and a rule that only covered comments would have left
// the two spellings arguing inside the same expression.
//
// The one exception is a name owned by somebody else. FTXUI's palette really is
// spelled `Color::Grey62`, and renaming that is not a spelling correction, it is
// a compile error -- so the check skips what a third-party API insists on and
// says so, rather than being switched off for the file that contains it.
#include "test_helpers.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

/// Every source file the project owns.
///
/// Walked from the configured source directory rather than from the working
/// directory, so it does not matter where the test binary is run from. Skips
/// `build/`, which is where the vendored dependencies land -- llama.cpp and
/// FTXUI may spell things however they like.
std::vector<std::filesystem::path> project_files() {
    std::vector<std::filesystem::path> found;
    const std::filesystem::path root{CRUCIBLE_SOURCE_DIR};
    std::error_code ec;

    static const std::vector<std::string> kExtensions{
        ".cpp", ".hpp", ".h", ".c", ".sh", ".ps1", ".md", ".txt", ".in", ".cmake",
        ".svg", ".json",
    };

    for (std::filesystem::recursive_directory_iterator it{root, ec}, end;
         it != end && !ec; it.increment(ec)) {
        const std::filesystem::path& path = it->path();
        const std::string name = path.filename().string();
        if (it->is_directory(ec)) {
            if (name == "build" || name == ".git" || name == "node_modules") {
                it.disable_recursion_pending();
            }
            continue;
        }
        // This file is the one place the flagged words are allowed to appear:
        // it is the list of them. Skipping it by name is honest -- an exclusion
        // list of "files this does not check" would grow, and this one cannot.
        if (name == "test_spelling.cpp") {
            continue;
        }
        const std::string extension = path.extension().string();
        const bool wanted =
            std::find(kExtensions.begin(), kExtensions.end(), extension) != kExtensions.end()
            || name == "CMakeLists.txt";
        if (wanted) {
            found.push_back(path);
        }
    }
    return found;
}

/// The British spellings that were actually in this codebase, plus the ones it
/// would most likely grow next. Lower case; the search is case-insensitive.
const std::vector<std::string>& british() {
    static const std::vector<std::string> words{
        "colour",    "centre",     "behaviour",  "recognis",   "organis",
        "normalis",  "serialis",   "initialis",  "apologis",   "summaris",
        "minimis",   "maximis",    "optimis",    "customis",   "authoris",
        "utilis",    "synchronis", "visualis",   "prioritis",  "categoris",
        "tokenis",   "sanitis",    "randomis",   "localis",    "isation",
        "analyse",   "analysed",   "analysing",  "paralyse",   "emphasise",
        "favour",    "honour",     "neighbour",  "labour",     "grey",
        "defence",   "offence",    "pretence",   "licence",    "practise",
        "catalogue", "programme",  "metre",      "litre",      "sceptic",
        "judgement", "skilful",    "storey",     "plough",     "draught",
        "mould",     "smoulder",   "manoeuvre",  "aluminium",  "sulphur",
        "travell",   "cancell",    "modell",     "labell",     "signall",
        "whilst",    "amongst",    "artefact",   "calibre",    "fibre",
        "spectre",   "orientate",
    };
    return words;
}

/// Names a third-party header spells the British way, which this project has to
/// spell their way or not compile. Matched before the word list, so the line
/// they appear on is not reported.
std::string without_foreign_names(std::string line) {
    static const std::vector<std::string> theirs{"Color::Grey", "ftxui::Color::Grey"};
    for (const std::string& name : theirs) {
        for (std::string::size_type at = line.find(name); at != std::string::npos;
             at = line.find(name, at)) {
            line.erase(at, name.size());
        }
    }
    return line;
}

std::string lowered(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

/// The whole word `at` sits inside, so a flagged run can be judged in context.
std::string word_around(const std::string& lower, std::string::size_type at) {
    const auto is_letter = [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) != 0;
    };
    std::string::size_type start = at;
    while (start > 0 && is_letter(lower[start - 1])) {
        --start;
    }
    std::string::size_type end = at;
    while (end < lower.size() && is_letter(lower[end])) {
        ++end;
    }
    return lower.substr(start, end - start);
}

/// American words that happen to contain a flagged run.
///
/// "analysis" holds "analyse"'s stem, "organism" holds "organis", "specialist"
/// holds "specialis" -- all three are spelled the same on both sides of the
/// Atlantic, and flagging them would make the lint something people switch off.
bool spelled_the_same_everywhere(const std::string& word) {
    static const std::vector<std::string> fine{
        "analysis",  "analyses", "analyst",     "analysts",  "organism",
        "organisms", "realistic", "specialist", "specialists", "liaison",
        "cancellation", "cancellations", "excellence", "modelling_note",
    };
    return std::find(fine.begin(), fine.end(), word) != fine.end();
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(the_whole_project_is_spelled_american) {
    const std::vector<std::filesystem::path> files = project_files();
    // If the walk found nothing the test is passing for the wrong reason, which
    // is the one failure mode a lint like this has.
    CHECK(files.size() > 50);

    std::vector<std::string> found_words;
    for (const std::filesystem::path& path : files) {
        std::ifstream in(path);
        if (!in) {
            continue;
        }
        std::string line;
        int number = 0;
        while (std::getline(in, line)) {
            ++number;
            const std::string lower = lowered(without_foreign_names(line));
            for (const std::string& word : british()) {
                const std::string::size_type at = lower.find(word);
                if (at == std::string::npos
                    || spelled_the_same_everywhere(word_around(lower, at))) {
                    continue;
                }
                std::ostringstream os;
                os << std::filesystem::relative(path, CRUCIBLE_SOURCE_DIR).string()
                   << ":" << number << "  \"" << word << "\"";
                found_words.push_back(os.str());
                break;
            }
        }
    }

    for (const std::string& where : found_words) {
        harness::report_failure(__FILE__, __LINE__, "British spelling: " + where);
    }
}
