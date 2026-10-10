// SPDX-License-Identifier: MIT
//
// What the frontier models have been asked.
//
// Every request to a provider is counted as it ends -- the chat's, each of a
// build's agents', the architect's -- by the one place they all go through,
// remote::Hub. Counted the way providers bill: tokens read fresh, read from
// the provider's cache, written to it, and written out. A cached read costs
// about a tenth of a fresh one, so a total that lumped them together would
// put a long conversation's cost at several times what it was.
//
// Two totals per model: since Crucible started, and this calendar month,
// which is kept in data_dir()/spend.json across starts. And what the
// provider's last answer said is left of its rate limits, which is the only
// measure of "how much more can I ask" a provider gives.
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "crucible/util/http.hpp"

namespace crucible {
struct Provider;
}

namespace crucible::spend {

/// Tokens, as they are billed.
struct Tokens {
    std::uint64_t input       = 0;   ///< read fresh
    std::uint64_t cache_read  = 0;   ///< read from the provider's cache
    std::uint64_t cache_write = 0;   ///< written to it, to be read cheaply later
    std::uint64_t output      = 0;
    std::uint64_t requests    = 0;

    void add(const Tokens& other);
};

/// How much of one of a provider's rate limits is left.
struct Limit {
    std::string  what;            ///< "requests", "tokens", "input tokens", "output tokens"
    std::int64_t limit     = 0;
    std::int64_t remaining = 0;
    std::string  resets;          ///< when it fills again, as the provider wrote it
};

/// One model at one provider.
struct Model {
    std::string provider_id;
    std::string provider;         ///< its name, as Settings shows it
    std::string kind;             ///< "anthropic" or "openai": which wire it speaks
    std::string endpoint;         ///< whose model it is, for its price
    std::string model;
    Tokens      session;          ///< since Crucible started
    Tokens      month;            ///< this calendar month
    int         prompt  = 0;      ///< tokens in the newest request
    int         context = 0;      ///< the model's context, 0 when it would not say
    std::int64_t last   = 0;      ///< when it was last asked, unix seconds
    std::vector<Limit> limits;    ///< as of its newest answer
};

/// The rate limits a response's headers report: Anthropic's own, and the
/// x-ratelimit shape OpenAI set and most of the rest copied. Empty for a
/// provider that says nothing.
std::vector<Limit> limits_from(const std::vector<util::http::Header>& headers);

/// The counts, shared by every thread that asks a provider anything.
class Ledger {
public:
    /// `file` keeps the months; empty keeps nothing, which is what a test
    /// wants.
    explicit Ledger(std::filesystem::path file = {});

    /// One request, ended. `prompt` is the whole prompt's size whatever was
    /// cached, for how full the context is; `headers` are the response's.
    void record(const Provider& provider, const std::string& model, const Tokens& used,
                int prompt, int context, const std::vector<util::http::Header>& headers);

    /// Every model asked this session or this month, the most recently asked
    /// first.
    std::vector<Model> models() const;

private:
    void load();
    void save() const;

    std::filesystem::path file_;
    std::string           month_;   ///< "2026-10": what `month` counts
    mutable std::mutex    mutex_;
    std::vector<Model>    models_;
};

}  // namespace crucible::spend
