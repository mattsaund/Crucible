// SPDX-License-Identifier: MIT
//
// What a frontier model's tokens cost.
//
// Providers do not say, in the answer or anywhere a program can ask: a price
// is on a web page. So the list comes from LiteLLM's, which is kept up to date
// by the people who maintain it for exactly this -- every provider's chat
// models, priced per token, fresh, cached and written to cache -- and is
// public, with nothing about this machine in the asking. It is fetched once a
// day at most, and only on a machine with a provider added, into
// data_dir()/prices.json; without it a model's tokens are counted and no cost
// is put on them, rather than a guess.
//
// A list price is what a provider charges anybody. A discount, a batch rate or
// a credit is between you and them, so the cost shown is an estimate and is
// labeled as one.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "crucible/llm/spend.hpp"

namespace crucible::prices {

/// US dollars per token.
struct Price {
    double input       = 0.0;
    double output      = 0.0;
    double cache_read  = 0.0;   ///< 0 when the list does not say: then input's
    double cache_write = 0.0;   ///< likewise
};

/// The list price of `model` at the provider behind `endpoint`, or nothing
/// when the list has none -- a server on your own network, a model too new or
/// too obscure, or no list fetched yet.
std::optional<Price> find(const std::string& endpoint, const std::string& model);

/// What `tokens` cost at `price`.
double cost(const spend::Tokens& tokens, const Price& price);

/// Fetch the list when it is missing or a day old. Blocks for the length of a
/// download, so it is run on a worker; a failure leaves the old list in place.
void refresh();

// --- exposed for the tests ---------------------------------------------------

namespace detail {

/// The list, by model name as LiteLLM spells it.
using List = std::map<std::string, std::pair<std::string, Price>>;   // name -> (provider, price)

/// LiteLLM's file, cut down to the chat models with a price.
List reduce(std::string_view litellm);

/// Who LiteLLM says serves a model at `endpoint`: "anthropic", "openai",
/// "gemini" and so on. Empty for an address it has no name for -- a server on
/// your own network, which costs nothing to ask.
std::string provider_for(std::string_view endpoint);

/// `model` in `list`, as served at `endpoint`.
std::optional<Price> lookup(const List& list, const std::string& endpoint, const std::string& model);

}  // namespace detail

}  // namespace crucible::prices
