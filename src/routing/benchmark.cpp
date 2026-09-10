// SPDX-License-Identifier: MIT
//
// See benchmark.hpp.
#include "crucible/routing/benchmark.hpp"

#include <string>
#include <utility>
#include <vector>

namespace crucible {

const std::vector<RouteCase>& benchmark_cases() {
    static const std::vector<RouteCase> kCases{
    {"mathematics", "compute the derivative of x^3 sin(x)"},
    {"mathematics", "prove there are infinitely many prime numbers"},
    {"mathematics", "what is the determinant of a 3x3 matrix"},
    {"mathematics", "solve the quadratic equation 2x^2 - 5x + 3 = 0"},
    {"mathematics", "how many ways can I arrange 5 books on a shelf"},
    {"mathematics", "what is the sum of an infinite geometric series"},

    {"programming", "why does my C++ program segfault when I dereference this pointer"},
    {"programming", "write a binary search function in Python"},
    {"programming", "how do I undo the last commit in git"},
    {"programming", "what is the difference between a list and a tuple in Python"},
    {"programming", "my unit tests pass locally but fail in CI"},
    {"programming", "explain how a hash table handles collisions"},

    {"physics", "why is the sky blue?"},
    {"physics", "what is the Lagrangian of a simple pendulum"},
    {"physics", "explain time dilation in special relativity"},
    {"physics", "how much kinetic energy does a 2 kg ball have at 10 m/s"},
    {"physics", "why does a gyroscope resist being tilted"},
    {"physics", "what happens to entropy in an isolated system"},

    {"chemistry", "balance the combustion reaction for propane"},
    {"chemistry", "what is the pH of a 0.1 molar HCl solution"},
    {"chemistry", "why is water a polar molecule"},
    {"chemistry", "how many grams of NaCl are in 2 moles"},
    {"chemistry", "what does a catalyst do to activation energy"},
    {"chemistry", "explain the difference between an alkane and an alkene"},

    {"biology", "how does DNA replication work in eukaryotic cells"},
    {"biology", "what role do mitochondria play in the cell"},
    {"biology", "how do vaccines produce immunity"},
    {"biology", "what is the difference between mitosis and meiosis"},
    {"biology", "why do antibiotics not work on viruses"},
    {"biology", "how does photosynthesis convert light into sugar"},

    {"engineering", "what torque should I use on an M8 steel bolt"},
    {"engineering", "how do I size a steel beam for a 3 meter span"},
    {"engineering", "what gauge wire do I need for a 20 amp circuit"},
    {"engineering", "how do I choose a bearing for a rotating shaft"},
    {"engineering", "what tolerance should this press fit have"},
    {"engineering", "how do I design a heat sink for a 50 watt device"},

    {"philosophy", "is free will compatible with determinism?"},
    {"philosophy", "explain Kant's categorical imperative"},
    {"philosophy", "what is the trolley problem meant to show"},
    {"philosophy", "can we know anything with certainty"},
    {"philosophy", "what makes an action morally right"},
    {"philosophy", "what is the mind-body problem"},

    {"sociology", "how does urbanization affect social mobility"},
    {"sociology", "what causes inflation in a modern economy"},
    {"sociology", "why do voter turnout rates differ between countries"},
    {"sociology", "what is the difference between a norm and a law"},
    {"sociology", "how did the industrial revolution change family structure"},
    {"sociology", "what does social capital mean"},

    {"language", "proofread this paragraph and improve its tone"},
    {"language", "translate 'good morning' into French"},
    {"language", "when should I use a semicolon instead of a comma"},
    {"language", "rewrite this sentence in the active voice"},
    {"language", "what is the etymology of the word quarantine"},
    {"language", "summarize this essay in two sentences"},
};
    return kCases;
}

namespace {

/// The roster the cases above are written against.
///
/// These nine used to ship as `Roster::defaults()` and were seeded into every
/// fresh config. They do not any more: Crucible ships no experts, and a new
/// install starts with an empty roster the user fills.
///
/// They live on here as what they always really were -- the answer sheet's other
/// half. `benchmark_cases()` names seats by id, so the ids have to exist
/// somewhere, and their blurbs, examples and keyword sets are measured rather
/// than guessed: this exact table is what scores 96% on the 54-prompt benchmark
/// with LFM2.5-1.2B. Change one and the numbers stop being comparable.
///
/// Nothing in a user's config is built from this. It is evaluation data.
Expert seat(const char* id, const char* name, const char* tag, const char* blurb,
            std::vector<std::string> examples, std::vector<std::string> keywords) {
    Expert expert;
    expert.id       = id;
    expert.name     = name;
    expert.tag      = tag;
    expert.blurb    = blurb;
    expert.examples = std::move(examples);
    expert.keywords = std::move(keywords);
    return expert;
}

}  // namespace

const Roster& benchmark_roster() {
    static const Roster kRoster = [] {
        Roster roster;
        std::string error;
        for (Expert& expert : std::vector<Expert>{
        seat("mathematics", "Mathematics", "MATH",
            "mathematics, algebra, calculus, proofs, geometry, statistics, probability, number theory",
            {"what is the integral of x squared",
             "what is the probability of rolling two sixes in a row"},
            {"integral","derivative","theorem","proof","matrix","algebra","calculus",
             "equation","polynomial","topology","geometry","probability","modulo",
             "eigenvalue","factorial","logarithm","prime","vector space","summation",
             "limit","differential","combinatorics","cardinality","isomorphism"}),

        seat("programming", "Programming", "PROG",
            "programming, code, software, algorithms, data structures, debugging, systems, a codebase",
            {"my python script throws a KeyError",
             "my docker container exits the moment it starts"},
            {"code","function","compile","debug","refactor","python","c++","rust",
             "javascript","algorithm","segfault","repository","git","api","pointer",
             "recursion","runtime","syntax","framework","typescript","kernel","binary",
             "linked list","stack trace"}),

        seat("physics", "Physics", "PHYS",
            "physics, forces, energy, light, thermodynamics, relativity, quantum, astronomy",
            {"why do heavy and light objects fall together",
             "why does a helium balloon rise"},
            {"quantum","relativity","momentum","thermodynamics","entropy","electromagnetic",
             "photon","lagrangian","hamiltonian","velocity","acceleration","gravity",
             "particle","wavelength","voltage","kinetic","newton","tensor field",
             "spacetime","fermion","boson","optics","friction","orbital mechanics"}),

        seat("chemistry", "Chemistry", "CHEM",
            "chemistry, reactions, molecules, bonding, acids, pH, materials, the laboratory",
            {"what happens when sodium touches water", "why does salt melt ice"},
            {"molecule","reaction","atom","bond","stoichiometry","titration","catalyst",
             "organic","ion","ph","enthalpy","reagent","solvent","isotope",
             "periodic table","valence","oxidation","polymer","acid","alkane","molarity",
             "chromatography","electrolysis","compound"}),

        seat("biology", "Biology", "BIO",
            "biology, cells, DNA, genetics, physiology, medicine, ecology, evolution",
            {"how do vaccines train the immune system",
             "how do muscles get oxygen during exercise"},
            {"cell","dna","protein","enzyme","gene","evolution","organism","mitochondria",
             "neuron","bacteria","virus","photosynthesis","chromosome","ecosystem",
             "species","metabolism","antibody","tissue","rna","physiology","genome",
             "receptor","hormone","allele"}),

        seat("engineering", "Engineering", "ENG",
            "engineering, designing or building a physical thing, mechanical electrical and civil design, bolts, beams, loads, circuits, wiring, tolerances, materials, hardware, CAD",
            {"what preload should this bolted joint have",
             "how do I stop this bracket from vibrating"},
            {"circuit","torque","stress","beam","cad","tolerance","bearing","hydraulic",
             "actuator","load bearing","weld","gear","pcb","transistor","chassis",
             "structural","machining","alloy","fastener","turbine","thermal design","cnc",
             "schematic","manufacturing"}),

        seat("philosophy", "Philosophy", "PHIL",
            "philosophy, ethics, right and wrong, logic, metaphysics, epistemology, free will, consciousness, knowledge, existence, meaning",
            {"can someone be blamed for an unavoidable act",
             "can a machine ever be said to understand anything"},
            {"ethics","epistemology","metaphysics","ontology","kant","stoic","morality",
             "consciousness","free will","utilitarian","existential","dialectic",
             "nietzsche","aristotle","virtue","phenomenology","determinism","socratic",
             "meaning of life","normative","a priori","solipsism","teleology","nihilism"}),

        seat("sociology", "Sociology", "SOC",
            "society, economics, politics, history, psychology, culture, institutions, law, education, inequality, cities, populations, why people or groups behave as they do",
            {"why did rents rise faster than wages",
             "what makes a protest movement succeed"},
            {"society","culture","class","institution","survey","demographic","inequality",
             "norms","capitalism","policy","election","market","psychology","behavior",
             "behavior","community","migration","ethnography","bureaucracy",
             "socialization","urbanization","gdp","labor","kinship"}),

        seat("language", "Language", "LANG",
            "writing, grammar, spelling, punctuation, a sentence, a paragraph, an essay, proofreading, editing, rewriting, tone, style, summarizing, translation, a word or its meaning, literature",
            {"what is the difference between affect and effect",
             "what is the plural of octopus"},
            {"grammar","translate","essay","sentence","rhetoric","poem","metaphor",
             "etymology","syntax rules","proofread","paragraph","literature","novel",
             "tone","phonetic","vocabulary","idiom","narrative","rewrite","spelling",
             "linguistic","prose","dialogue","summarize"}),

        }) {
            roster.add(std::move(expert), error);
        }
        return roster;
    }();
    return kRoster;
}

}  // namespace crucible
