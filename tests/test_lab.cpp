// SPDX-License-Identifier: MIT
//
// The lab: what a custom model is made of, and whether this machine can make it.
//
// The arithmetic here is what the Create tab promises a user before they spend
// an hour on a run, so it is pinned: a size that is wrong by a factor of two
// is the difference between "this fits" and a run that dies at epoch one. The
// hub replies are canned, because a test that needs Hugging Face to be up is a
// test that fails for reasons that have nothing to do with this code.
#include "test_helpers.hpp"

#include "crucible/lab/hub.hpp"
#include "crucible/lab/recipe.hpp"

using namespace crucible::lab;

namespace {

Recipe started(const std::string& name) {
    Recipe recipe;
    recipe.name = name;
    recipe.id   = slug_of(name);
    return recipe;
}

Asset hub_asset(const std::string& id) {
    Asset asset;
    asset.source = Source::Hub;
    asset.id     = id;
    return asset;
}

constexpr std::uint64_t kGiB = 1024ULL * 1024 * 1024;

}  // namespace

// ---------------------------------------------------------------------------
// The recipe
// ---------------------------------------------------------------------------

TEST(a_name_becomes_something_a_filesystem_can_hold) {
    CHECK_EQ(slug_of("Kitchen Physicist"), std::string("kitchen-physicist"));
    CHECK_EQ(slug_of("  C++ / Rust helper!  "), std::string("c-rust-helper"));
    CHECK_EQ(slug_of("already-a-slug"), std::string("already-a-slug"));
    CHECK_EQ(slug_of("!!!"), std::string(""));
}

TEST(a_recipe_says_what_it_is_still_missing_in_the_order_it_is_asked_for) {
    Recipe recipe;
    const std::vector<std::string> empty_recipe = missing(recipe);
    CHECK_EQ(empty_recipe.size(), std::size_t{3});
    CHECK_EQ(empty_recipe[0], std::string("a name"));

    recipe = started("Kitchen Physicist");
    CHECK_EQ(missing(recipe).size(), std::size_t{2});

    recipe.base = hub_asset("unsloth/Llama-3.2-1B");
    const std::vector<std::string> last = missing(recipe);
    CHECK_EQ(last.size(), std::size_t{1});
    CHECK_EQ(last[0], std::string("training data"));

    recipe.data.push_back(hub_asset("wikitext"));
    CHECK(missing(recipe).empty());
}

TEST(the_export_size_is_what_the_quantization_actually_weighs) {
    // A 1B model at Q4_K_M is a little over half a gigabyte, and at F16 two
    // gigabytes. These are the numbers the tab puts beside each choice, and
    // choosing a quantization is choosing a size.
    const std::uint64_t small = export_bytes(1.0, "Q4_K_M");
    CHECK(small > 550ULL * 1000 * 1000);
    CHECK(small < 700ULL * 1000 * 1000);

    const std::uint64_t half = export_bytes(1.0, "F16");
    CHECK(half > 1900ULL * 1000 * 1000);
    CHECK(half < 2100ULL * 1000 * 1000);

    // Bigger quantization, bigger file, every time.
    CHECK(export_bytes(7.0, "Q4_K_M") < export_bytes(7.0, "Q5_K_M"));
    CHECK(export_bytes(7.0, "Q5_K_M") < export_bytes(7.0, "Q8_0"));

    // Nothing known about it is nothing claimed about it.
    CHECK_EQ(export_bytes(7.0, "Q4_K_S_MADE_UP"), std::uint64_t{0});
    CHECK_EQ(export_bytes(0.0, "Q4_K_M"), std::uint64_t{0});
}

TEST(qlora_is_what_puts_a_seven_billion_model_on_one_card) {
    // A four-bit base and its activations: a 7B lands near 7 GB, which is the
    // whole point of the method -- it fits a card people already own.
    const Fit seven = estimate_fit(7.0, Method::Qlora, 12 * kGiB, 32 * kGiB);
    CHECK(seven.possible);
    CHECK(seven.needed > 5ULL * kGiB);
    CHECK(seven.needed < 10ULL * kGiB);

    // The same model in sixteen bits does not fit that card, and the estimate
    // says which way out there is rather than only refusing.
    const Fit lora = estimate_fit(7.0, Method::Lora, 12 * kGiB, 32 * kGiB);
    CHECK(!lora.possible);
    CHECK(lora.note.find("QLoRA") != std::string::npos);
}

TEST(a_small_model_fits_either_way_and_a_huge_one_fits_neither) {
    CHECK(estimate_fit(1.0, Method::Lora, 8 * kGiB, 16 * kGiB).possible);
    CHECK(estimate_fit(1.0, Method::Qlora, 8 * kGiB, 16 * kGiB).possible);

    // Seventy billion is a data-center model whatever the method: an adapter
    // makes the trainable part small, not the frozen base it sits on.
    CHECK(!estimate_fit(70.0, Method::Qlora, 40 * kGiB, 46 * kGiB).possible);
}

TEST(with_no_base_model_chosen_the_estimate_asks_for_one_rather_than_guessing) {
    const Fit nothing = estimate_fit(0.0, Method::Qlora, 40 * kGiB, 46 * kGiB);
    CHECK(!nothing.possible);
    CHECK(nothing.note.find("pick a base model") != std::string::npos);
}

TEST(a_recipe_survives_the_round_trip_through_disk) {
    Recipe recipe   = started("Kitchen Physicist");
    recipe.purpose  = "answers questions about induction hobs";
    recipe.base     = hub_asset("unsloth/Llama-3.2-1B");
    recipe.base.file  = "model.safetensors";
    recipe.base.bytes = 2500000000ULL;
    recipe.data.push_back(hub_asset("wikitext"));
    recipe.tools.push_back(hub_asset("crucible/tool-calls"));
    recipe.tools.back().source = Source::Local;
    recipe.method       = Method::Lora;
    recipe.format       = Export::Mlx;
    recipe.quantization = "Q5_K_M";
    recipe.parameters_b = 1.24;
    recipe.epochs       = 3;
    recipe.context      = 1024;

    Recipe      back;
    std::string error;
    CHECK(parse(serialize(recipe), back, error));
    CHECK(error.empty());
    CHECK_EQ(back.name, recipe.name);
    CHECK_EQ(back.id, std::string("kitchen-physicist"));
    CHECK_EQ(back.purpose, recipe.purpose);
    CHECK_EQ(back.base.id, recipe.base.id);
    CHECK_EQ(back.base.file, recipe.base.file);
    CHECK_EQ(back.base.bytes, recipe.base.bytes);
    CHECK_EQ(back.data.size(), std::size_t{1});
    CHECK_EQ(back.tools.size(), std::size_t{1});
    CHECK(back.tools[0].source == Source::Local);
    CHECK(back.method == Method::Lora);
    CHECK(back.format == Export::Mlx);
    CHECK_EQ(back.quantization, std::string("Q5_K_M"));
    CHECK_EQ(back.epochs, 3);
    CHECK_EQ(back.context, 1024);
}

TEST(nonsense_on_disk_is_refused_rather_than_loaded_as_an_empty_recipe) {
    Recipe      recipe;
    std::string error;
    CHECK(!parse("this is not json", recipe, error));
    CHECK(!error.empty());
    CHECK(!parse("[1, 2, 3]", recipe, error));
}

TEST(a_saved_recipe_is_found_again) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    Recipe recipe = started("Kitchen Physicist");
    recipe.base   = hub_asset("unsloth/Llama-3.2-1B");
    std::string error;
    CHECK(save(recipe, error));
    CHECK(error.empty());

    const std::vector<Recipe> found = saved_recipes();
    CHECK_EQ(found.size(), std::size_t{1});
    if (!found.empty()) {
        CHECK_EQ(found[0].name, std::string("Kitchen Physicist"));
        CHECK_EQ(found[0].base.id, std::string("unsloth/Llama-3.2-1B"));
    }

    // And a recipe with nothing to name it is refused, rather than writing a
    // directory called "".
    Recipe unnamed;
    CHECK(!save(unnamed, error));
    CHECK(!error.empty());
}

TEST(a_finished_model_is_offered_to_the_roster_and_a_missing_one_is_not) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    // One that was trained and is still there.
    const std::filesystem::path file = temp.path() / "math.Q4_K_M.gguf";
    { std::ofstream out(file); out << "not really a model"; }
    Recipe done         = started("Math");
    done.base           = hub_asset("unsloth/Llama-3.2-1B");
    done.purpose        = "algebra, calculus, proofs";
    done.trained_path   = file.string();
    std::string error;
    CHECK(save(done, error));

    // One still being assembled, and one whose file has been deleted since.
    Recipe building = started("Physics");
    CHECK(save(building, error));
    Recipe gone       = started("Chemistry");
    gone.trained_path = (temp.path() / "went-away.gguf").string();
    CHECK(save(gone, error));

    const std::vector<Made> made = finished_models();
    CHECK_EQ(made.size(), std::size_t{1});
    if (!made.empty()) {
        CHECK_EQ(made[0].name, std::string("Math"));
        CHECK_EQ(made[0].purpose, std::string("algebra, calculus, proofs"));
        CHECK_EQ(made[0].path, file);
        CHECK(made[0].bytes > 0);
    }
}

// ---------------------------------------------------------------------------
// The hub
// ---------------------------------------------------------------------------

TEST(a_search_asks_for_the_most_downloaded_first) {
    const std::string url = hub::search_url(hub::Kind::Model, "physics tutor", 20);
    CHECK(url.find("https://huggingface.co/api/models") == 0);
    CHECK(url.find("search=physics%20tutor") != std::string::npos);
    CHECK(url.find("limit=20") != std::string::npos);
    CHECK(url.find("sort=downloads") != std::string::npos);
    // A plain search carries no "gated" flag, and a gated repository is a
    // download that fails an hour in. Expanding replaces the default fields,
    // so everything the list shows has to be asked for by name.
    CHECK(url.find("expand%5B%5D=gated") != std::string::npos);
    CHECK(url.find("expand%5B%5D=safetensors") != std::string::npos);
    // A dataset has neither weights nor a pipeline tag, and asking for them is
    // an error rather than an empty field.
    const std::string data_url = hub::search_url(hub::Kind::Dataset, "wiki", 5);
    CHECK(data_url.find("expand%5B%5D=gated") != std::string::npos);
    CHECK(data_url.find("safetensors") == std::string::npos);
    CHECK(data_url.find("pipeline_tag") == std::string::npos);

    // Datasets are a different shelf of the same library.
    CHECK(hub::search_url(hub::Kind::Dataset, "wiki", 5).find("/api/datasets") != std::string::npos);
    // And a download is a different host path again, with datasets prefixed.
    CHECK_EQ(hub::download_url(hub::Kind::Model, "a/b", "model.gguf"),
             std::string("https://huggingface.co/a/b/resolve/main/model.gguf"));
    CHECK_EQ(hub::download_url(hub::Kind::Dataset, "a/b", "train.jsonl"),
             std::string("https://huggingface.co/datasets/a/b/resolve/main/train.jsonl"));
}

TEST(a_hub_reply_becomes_a_list_to_choose_from) {
    const std::string reply = R"([
        {"id": "unsloth/Llama-3.2-1B", "downloads": 412000, "likes": 250,
         "pipeline_tag": "text-generation", "gated": false,
         "safetensors": {"total": 1235814400}},
        {"id": "meta-llama/Llama-3.1-8B", "downloads": 9000000, "likes": 4100,
         "pipeline_tag": "text-generation", "gated": "auto"},
        {"modelId": "tiny/whatever", "downloads": 3},
        {"nothing": "useful"}
    ])";
    const std::vector<hub::Item> items = hub::parse_search(reply);
    CHECK_EQ(items.size(), std::size_t{3});   // the one with no id is dropped

    CHECK_EQ(items[0].id, std::string("unsloth/Llama-3.2-1B"));
    CHECK_EQ(items[0].author, std::string("unsloth"));
    CHECK_EQ(items[0].downloads, std::uint64_t{412000});
    CHECK(!items[0].gated);
    // The metadata says 1.24 billion, and that beats reading the name.
    CHECK(items[0].parameters_b > 1.2);
    CHECK(items[0].parameters_b < 1.3);

    // Gated is anything but false: this one needs a browser and a license.
    CHECK(items[1].gated);
    // No metadata, so the name carries it.
    CHECK_EQ(items[1].parameters_b, 8.0);

    // An id under the older key is still an id.
    CHECK_EQ(items[2].id, std::string("tiny/whatever"));
}

TEST(a_hub_reply_that_is_not_a_list_yields_nothing_rather_than_breaking_the_step) {
    CHECK(hub::parse_search("").empty());
    CHECK(hub::parse_search("<html>rate limited</html>").empty());
    CHECK(hub::parse_search(R"({"error": "not found"})").empty());
}

TEST(a_file_listing_reports_what_a_download_will_actually_cost) {
    const std::string reply = R"([
        {"type": "file", "path": "README.md", "size": 1200},
        {"type": "directory", "path": "onnx"},
        {"type": "file", "path": "model.safetensors", "size": 135,
         "lfs": {"size": 2471645608}}
    ])";
    const std::vector<hub::File> files = hub::parse_tree(reply);
    CHECK_EQ(files.size(), std::size_t{2});   // the directory is not a file
    CHECK_EQ(files[0].path, std::string("README.md"));
    // The pointer file says 135 bytes; the model is two and a half gigabytes,
    // and that is the number worth showing.
    CHECK_EQ(files[1].bytes, std::uint64_t{2471645608});
}

TEST(a_size_in_a_model_name_is_read_from_the_right) {
    CHECK_EQ(hub::parameters_from_name("unsloth/Llama-3.2-1B"), 1.0);
    CHECK_EQ(hub::parameters_from_name("Qwen2.5-7B-Instruct"), 7.0);
    CHECK_EQ(hub::parameters_from_name("HuggingFaceTB/SmolLM-135M"), 0.135);
    CHECK_EQ(hub::parameters_from_name("google/gemma-2-2b-it"), 2.0);
    // A version number is not a size, and reading left to right made Llama 3.2
    // a three-billion model.
    CHECK_EQ(hub::parameters_from_name("meta/Llama-3.2"), 0.0);
    CHECK_EQ(hub::parameters_from_name("mistralai/Mistral-Nemo"), 0.0);
    CHECK_EQ(hub::parameters_from_name(""), 0.0);
}
