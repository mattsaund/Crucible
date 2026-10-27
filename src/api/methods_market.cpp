// SPDX-License-Identifier: MIT
//
// The Models tab: searching Hugging Face, what a repository can be downloaded
// as and whether each will run here, and downloads into the models folder.
// See market/hub.hpp and market/downloads.hpp.
#include "methods.hpp"

#include <system_error>

#include "crucible/market/downloads.hpp"
#include "crucible/market/hub.hpp"
#include "crucible/market/logos.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/text.hpp"

namespace crucible::api {
namespace {

/// A string parameter, or empty: never a throw for one sent as something else.
std::string text_of(const json& params, const char* key) {
    const auto found = params.find(key);
    return found != params.end() && found->is_string() ? found->get<std::string>() : std::string();
}

json listing_json(const market::Listing& l) {
    return json{{"id", l.id}, {"author", l.author}, {"name", l.name},
                {"downloads", l.downloads}, {"likes", l.likes}, {"updated", l.updated},
                {"params_b", l.params_b}, {"architecture", l.architecture}, {"context", l.context},
                {"pipeline", l.pipeline}, {"vision", l.vision}, {"tools", l.tools},
                {"reasoning", l.reasoning}, {"gated", l.gated}, {"license", l.license},
                {"base_model", l.base_model}, {"logo", market::logo_owner(l.id, l.base_model)}};
}

/// A page of a search: the first, or the one the last page said was next.
Reply market_search(const json& params, const Scene&) {
    const market::Sort sort = market::sort_from_id(text_of(params, "sort"));
    std::string url = text_of(params, "next");
    if (url.empty()) {
        url = market::search_url(text_of(params, "query"), market::format_from_id(text_of(params, "format")),
                                 sort, 40);
    }
    market::Page page;
    std::string  error;
    if (!market::search(url, page, error)) {
        return bad(error);
    }
    if (sort == market::Sort::Best) {
        market::rank_best(page.listings);
    }
    // So market.logo, asked by each one's id, does not ask what was just said.
    market::remember_logos(page.listings);
    json listings = json::array();
    for (const market::Listing& listing : page.listings) {
        listings.push_back(listing_json(listing));
    }
    return good(json{{"listings", std::move(listings)}, {"next", page.next}});
}

/// One repository: its options, whether each will run here, which are here
/// already, and the one to suggest.
Reply market_model(const json& params, const Scene& scene) {
    const std::string    id     = text_of(params, "id");
    const market::Format format = market::format_from_id(text_of(params, "format"));
    if (id.empty()) {
        return bad("which model?");
    }
    market::Detail detail;
    std::string    error;
    if (!market::model(id, format, detail, error)) {
        return bad(error);
    }
    market::remember_logos({detail.listing});
    const market::Machine       machine = market::this_machine();
    const std::filesystem::path models  = scene.config.resolved_models_dir();

    json options = json::array();
    int  best    = -1;
    int  best_rank = 99;
    for (std::size_t i = 0; i < detail.options.size(); ++i) {
        const market::Option& option = detail.options[i];
        const market::Fit     fit    = market::fit(option.bytes, format, machine);
        bool here = !option.files.empty();
        const std::filesystem::path folder = market::destination(models, option.repo);
        for (const market::File& file : option.files) {
            std::error_code     ec;
            const std::uintmax_t size = std::filesystem::file_size(market::landing(folder, file, format), ec);
            if (ec || (file.bytes > 0 && size != file.bytes)) {
                here = false;
                break;
            }
        }
        // The one to suggest: the largest that runs on the GPUs alone -- the
        // most of the model this machine can run at full speed -- or, when
        // none does, the largest that runs at all. Options are smallest first,
        // so a later one of the same rank is larger.
        const int rank = fit == market::Fit::Gpu ? 0 : fit == market::Fit::Partial ? 1
                       : fit == market::Fit::Cpu ? 2 : 99;
        if (rank <= best_rank && rank < 99) {
            best      = static_cast<int>(i);
            best_rank = rank;
        }
        json one{{"name", option.name}, {"repo", option.repo}, {"bytes", option.bytes},
                 {"files", option.files.size()},
                 {"fit", std::string(market::fit_id(fit))}, {"here", here}};
        // A GGUF option's files by name, so the page can open the repository
        // on the one file it was asked for: a model on Bench's board.
        if (format == market::Format::Gguf) {
            json names = json::array();
            for (const market::File& file : option.files) {
                names.push_back(std::filesystem::path(file.path).filename().string());
            }
            one["names"] = std::move(names);
        }
        options.push_back(std::move(one));
    }
    if (best >= 0) {
        options[static_cast<std::size_t>(best)]["recommended"] = true;
    }
    json out          = listing_json(detail.listing);
    out["format"]     = std::string(market::format_id(format));
    out["options"]    = std::move(options);
    out["machine"]    = json{{"vram", machine.vram}, {"ram", machine.ram}, {"gpus", machine.gpus},
                             {"apple", machine.apple}};
    return good(out);
}

/// A repository's README, for the page to tidy and draw.
Reply market_readme(const json& params, const Scene&) {
    std::string text;
    std::string error;
    if (!market::readme(text_of(params, "id"), text, error)) {
        return bad(error);
    }
    // A card can be enormous: tables of benchmarks, pages of examples. The
    // start of it is what anybody reads.
    if (text.size() > 200000) {
        text.resize(200000);
    }
    return good(json{{"text", crucible::detail::scrub_utf8(text)}});
}

/// The logo beside a repository, by its id: whose it is, and the picture as
/// a data: URL the page can show, or empty for none. The Models tab asks
/// once per owner a list shows, after the list; anything else that names a
/// repository can ask the same. See market/logos.hpp.
Reply market_logo(const json& params, const Scene&) {
    const std::string id = text_of(params, "id");
    market::Logo      logo;
    std::string       error;
    if (!market::logo(id, logo, error)) {
        return bad(error);
    }
    return good(json{{"id", id}, {"owner", logo.owner},
                     {"image", logo.mime.empty() ? std::string()
                                                 : "data:" + logo.mime + ";base64," + format::base64(logo.bytes)}});
}

/// Start one. An action: it only starts a thread, and what the option holds
/// is asked of the hub again over there.
Reply market_download(const json& params, Host& host) {
    market::Downloads* downloads = host.downloads();
    if (downloads == nullptr) {
        return bad("this Crucible cannot download models");
    }
    const std::string repo   = text_of(params, "repo");
    const std::string option = text_of(params, "option");
    if (repo.find('/') == std::string::npos || option.empty()) {
        return bad("which model, and which of it?");
    }
    std::string error;
    if (!downloads->start(repo, market::format_from_id(text_of(params, "format")), option,
                          host.config().resolved_models_dir(), error)) {
        return bad(error);
    }
    return good();
}

Reply market_cancel(const json& params, Host& host) {
    if (market::Downloads* downloads = host.downloads()) {
        downloads->cancel(text_of(params, "id"));
    }
    return good();
}

Reply market_dismiss(const json& params, Host& host) {
    if (market::Downloads* downloads = host.downloads()) {
        downloads->dismiss(text_of(params, "id"));
    }
    return good();
}

}  // namespace

void market_methods(std::vector<Method>& table) {
    table.push_back({"market.search",   market_search, nullptr});
    table.push_back({"market.model",    market_model, nullptr});
    table.push_back({"market.readme",   market_readme, nullptr});
    table.push_back({"market.logo",     market_logo, nullptr});
    table.push_back({"market.download", nullptr, market_download});
    table.push_back({"market.cancel",   nullptr, market_cancel});
    table.push_back({"market.dismiss",  nullptr, market_dismiss});
}

}  // namespace crucible::api
